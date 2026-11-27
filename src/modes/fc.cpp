/* FC — частотомер BK4819 («быстрый скан»).
 *
 * Аппаратный счётчик BK4819 ловит несущую передатчика на всём диапазоне за
 * 0.2–1.6 с — гораздо быстрее RSSI-развёртки, но находит только активные
 * передатчики. Диапазонный фильтр платы (VHF/UHF/800) переключается по
 * частоте через BK4819_SelectFilterByFrequency — тот же код, что у VFO и
 * сканера (раньше стояли пины BK-K5, на RT-880 другие маски).
 *
 * Находка (2 подряд совпадения в пределах 3 кГц): скан останавливается,
 * приёмник настраивается на частоту, звук — через обычный шумодав
 * (Radio_Update); запись попадает в список находок (loot). Чёрный и белый
 * списки пропускаются. Пропал сигнал (шумодав закрыт 55 мс) — скан дальше.
 *
 * Клавиши: 1/7 скорость, 3/9 шумодав, 5 порог счётчика (Гц), '#' диапазон
 * вручную, 0 автопереключение диапазонов, SIDE1/SIDE2 — находку в чёрный/белый
 * список и дальше, MENU или PTT — частоту в VFO и выход, STAR короткий —
 * список находок, EXIT — выход без неё, длинный MENU — список находок.
 * UP/DOWN — встроенный список находок (как в сканере): по кругу, сортировка
 * 1/2/3/4, SIDE1/SIDE2 — BL/WL, 0 — удалить, длинный SIDE2 — в канал,
 * MENU — выбранную в VFO, EXIT — назад к скану. Крупная частота на экране
 * округлена до шага VFO (точная — в строке находки).
 */
#include "modes.h"
#include "vfo_session.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/channels.h"
#include "../helper/log.h"
#include "../helper/lootlist.h"
#include "../helper/names.h"
#include "../helper/settings.h"
#include "../helper/measurements.h"
#include "../helper/tone.h"
#include "../helper/unit_fmt.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/lv_port.h"
#include "../ui/screen_layout.h"
#include "../ui/statusline.h"
#include "mode.h"
#include "printf.h"
#include <stdlib.h>
#include <string.h>

#define FC_STATUS_H 24

#define REQUIRED_HITS 2
#define BAND_SWITCH_INTERVAL 2 /* тиков по (200<<speed) мс */
#define RESUME_MS 55           /* сколько шумодав должен быть закрыт */
#define HOLD_MAX_SQ0_MS 5000   /* SQ=0 (всегда открыт): не залипать на хите */
#define HIT_TOL 300            /* 3 кГц (ед. 10 Гц) — «та же частота» */

/* Диапазоны счётчика: репрезентативная частота выбирает фильтр платы;
 * результат принимается, только если лежит в окне текущего диапазона. */
struct FcBand {
  const char *name;
  uint32_t rep, lo, hi; /* ед. 10 Гц */
  Filter filter;
};
static const FcBand BANDS[] = {
    {"VHF", 14500000, 3000000, 24000000, FILTER_VHF},
    {"UHF", 43300000, 24000000, 80000000, FILTER_UHF},
    {"800", 90000000, 80000000, 130000000, FILTER_800},
};
#define BAND_N (sizeof(BANDS) / sizeof(BANDS[0]))

struct {
  uint8_t scanSpeed;
  /* Младшие 14 бит REG_32: бит0 — enable, <13:1> — порог счётчика.
   * Это не герцы; 0x244/0x580 — значения из UV-K5. */
  uint16_t thr;
  uint8_t band;
  bool bandAutoSwitch;
  bool isScanning;
  uint32_t currentFrequency;
  uint32_t lastDetectedFrequency;
  uint8_t frequencyHits;
  uint8_t bandSwitchCounter;
  uint32_t lastScanTime;
  uint32_t stopFreq;
  uint32_t stoppedAt;
  uint32_t resumeAt; /* 0 — не ждём; иначе момент возобновления скана */
  bool stopped;
  uint8_t savedVfo;
  uint8_t savedMonitors; /* multiwatch на время FC выключен */
  uint8_t scanVfo;
  uint32_t savedFreq;
  uint32_t captureF; /* частота в VFO при выходе (0 — вернуть прежнюю) */
  /* Встроенный список находок — те же переходы и действия, что в сканере:
   * 0 — текущий скан, 1..N — запись loot (индекс = listSel-1). */
  uint16_t listSel;
  uint16_t listTop;
  uint8_t sortIdx;
  bool sortRev;
  bool listCapture; /* MENU в списке: в VFO уже настроено, exit не откатывать */
  char msg[16];
  uint32_t msgUntil;
  VfoSnapshot listSaved;
} fcCtx;

/* Список находок (live) — те же строки, что в сканере: свежие сверху. */
static uint16_t lootRowIdx[UI_LOOT_ROWS];
static uint16_t lootRowAge[UI_LOOT_ROWS];
static const char *s_titleState;

static void selectBand(void) {
  BK4819_SelectChip(0);
  /* FC сам знает свой диапазон: ручной фильтр из VFO здесь не подходит. */
  BK4819_SelectFilter(BANDS[fcCtx.band].filter);
  /* Антенное реле в FC не трогаем: иначе оно щёлкает на каждой смене
   * диапазона. Для 800 МГц пользователь сам ставит A2 коротким ALARM. */
}

static void enableScan() {
  BK4819_EnableFrequencyScanEx2(
      (FreqScanTime)(fcCtx.scanSpeed > F_SC_T_COUNT - 1 ? 0 : fcCtx.scanSpeed),
      fcCtx.thr);
  fcCtx.isScanning = true;
  fcCtx.lastScanTime = millis();
}

static void disableScan() {
  BK4819_DisableFrequencyScan();
  BK4819_RX_TurnOn();
  BK4819_ToggleAFDAC(false); /* RX_TurnOn включает DAC — учёт звука = выкл */
  fcCtx.isScanning = false;
}

static void nextBand() {
  fcCtx.band = (fcCtx.band + 1) % BAND_N;
  selectBand();
  fcCtx.bandSwitchCounter = 0;
}

static bool freqAccepted(uint32_t freq) {
  const FcBand &b = BANDS[fcCtx.band];
  if (freq < b.lo || freq >= b.hi)
    return false;
  if (freq >= 88 * MHZ && freq <= 108 * MHZ) /* FM-вещание — мусор */
    return false;
  return !LOOT_IsSkipped(freq, HIT_TOL, false); /* чёрный/белый список */
}

/* Счётчик на слабом сигнале отдаёт «хвост» развёртки/шум. Проверяем по RSSI,
 * что на найденной частоте есть несущая, а не ровный шум. */
static bool carrierPresent(uint32_t freq) {
  uint32_t off = freq + 200u * KHZ; /* +200 кГц, вне полосы приёма */
  if (off > BK4819_F_MAX)
    off = freq - 200u * KHZ;
  BK4819_TuneTo(freq, false);
  delay_ms(5);
  uint16_t on = BK4819_GetRSSI();
  BK4819_TuneTo(off, false);
  delay_ms(5);
  uint16_t noise = BK4819_GetRSSI();
  return on > noise + 12; /* 12 отсчётов ≈ 6 дБ */
}

/* Остановились на находке: настройка приёмника, учёт в списке */
static void stopOnHit(uint32_t freq) {
  disableScan();
  Radio_TuneTo(freq, true); /* precise: модуляция, фильтр диапазона, шумодав */
  fcCtx.stopped = true;
  fcCtx.stopFreq = freq;
  fcCtx.stoppedAt = millis();
  fcCtx.resumeAt = 0;
  fcCtx.frequencyHits = 0;
  LOOT_Hit(freq, BK4819_GetRSSI(), vfos[currentVfo].modulation,
           vfos[currentVfo].bw, vfos[currentVfo].radio);
}

/* Вернуться к скану (после потери сигнала или после чёрного/белого списка) */
static void resumeScan() {
  LOOT_Standby();
  Radio_ScanAudio(false);
  fcCtx.stopped = false;
  fcCtx.stopFreq = 0;
  fcCtx.resumeAt = 0;
  fcCtx.lastDetectedFrequency = 0;
  fcCtx.frequencyHits = 0;
  selectBand();
  enableScan();
}

/* ─── Встроенный список находок (как в сканере) ─── */

static uint32_t roundToStep(uint32_t f) {
  /* Шаг целевого VFO: в списке это scanVfo, при capture — savedVfo. */
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  if (!step)
    return f;
  return ((f + step / 2) / step) * step;
}

static void msgSet(const char *text) {
  snprintf(fcCtx.msg, sizeof(fcCtx.msg), "%s", text);
  fcCtx.msgUntil = millis() + 1500;
}

static void msgSetSaved(const char *tag, int16_t n) {
  if (n < 0) {
    msgSet("ERR");
    return;
  }
  char b[16];
  snprintf(b, sizeof(b), "%s %d", tag, (int)n);
  msgSet(b);
}

/* Свежие находки сверху: сортируем индексы по «сколько назад», сам loot не
 * трогаем (его порядок общий для списка и хранилища). */
static uint8_t collectRecentLoot(void) {
  uint16_t n = LOOT_Size();
  uint16_t nowS = (uint16_t)(millis() / 1000);
  uint8_t cnt = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const Loot *it = LOOT_Item(i);
    uint16_t age = (uint16_t)(nowS - it->lastTimeOpen);
    uint8_t p = 0;
    while (p < cnt && lootRowAge[p] <= age)
      p++;
    if (p >= UI_LOOT_ROWS)
      continue;
    if (cnt < UI_LOOT_ROWS)
      cnt++;
    for (uint8_t j = (uint8_t)(cnt - 1); j > p; --j) {
      lootRowAge[j] = lootRowAge[j - 1];
      lootRowIdx[j] = lootRowIdx[j - 1];
    }
    lootRowAge[p] = age;
    lootRowIdx[p] = i;
  }
  return cnt;
}

static TextStyle lootRowStyle(const Loot *it) {
  Color c = it->blacklist ? C_GRAY
            : it->whitelist ? C_GREEN
            : it->open      ? C_YELLOW
                            : C_WHITE;
  return text_style(c, C_BLACK, POS_L, F_M);
}

static void drawLootList(void) {
  uint16_t nowS = (uint16_t)(millis() / 1000);
  if (!fcCtx.listSel) {
    uint8_t cnt = collectRecentLoot();
    for (uint8_t r = 0; r < UI_LOOT_ROWS; ++r) {
      uint16_t y =
          (uint16_t)(UI_LOOT_Y + r * UI_LOOT_ROW_H + FontAscent(F_M));
      if (r >= cnt) {
        PrintfT(UI_LEFT_X, y, text_style(C_WHITE, C_BLACK, POS_L, F_M), "%s",
                "");
        continue;
      }
      const Loot *it = LOOT_Item(lootRowIdx[r]);
      char fb[16], age[5];
      fmt_mhz5(fb, sizeof(fb), it->f);
      fmt_duration(age, sizeof(age), (uint16_t)(nowS - it->lastTimeOpen));
      char mark =
          it->blacklist ? '-' : it->whitelist ? '+' : it->open ? '!' : ' ';
      PrintfT(UI_LEFT_X, y, lootRowStyle(it), "%c%s %s %+4d", mark, fb, age,
              (int)it->peak - 160);
    }
    return;
  }

  uint16_t n = LOOT_Size();
  for (uint8_t r = 0; r < UI_LOOT_ROWS; ++r) {
    uint16_t y = (uint16_t)(UI_LOOT_Y + r * UI_LOOT_ROW_H + FontAscent(F_M));
    uint16_t idx = (uint16_t)(fcCtx.listTop + r);
    if (idx >= n) {
      PrintfT(UI_LEFT_X, y, text_style(C_WHITE, C_BLACK, POS_L, F_M), "%s",
              "");
      continue;
    }
    const Loot *it = LOOT_Item(idx);
    bool sel = (uint16_t)(idx + 1) == fcCtx.listSel;
    char fb[16], age[5];
    fmt_mhz5(fb, sizeof(fb), it->f);
    fmt_duration(age, sizeof(age), (uint16_t)(nowS - it->lastTimeOpen));
    char mark =
        it->blacklist ? '-' : it->whitelist ? '+' : it->open ? '!' : ' ';
    TextStyle st = sel ? text_style(C_BLACK, C_CYAN, POS_L, F_M)
                       : lootRowStyle(it);
    PrintfT(UI_LEFT_X, y, st, "%c%c%s %s %+4d", sel ? '>' : ' ', mark, fb,
            age, (int)it->peak - 160);
  }
}

static void clampListSel(void) {
  uint16_t n = LOOT_Size();
  if (!fcCtx.listSel || !n) {
    fcCtx.listSel = 0;
    fcCtx.listTop = 0;
    return;
  }
  if (fcCtx.listSel > n)
    fcCtx.listSel = n;
  uint16_t idx = (uint16_t)(fcCtx.listSel - 1);
  if (idx < fcCtx.listTop)
    fcCtx.listTop = idx;
  if (idx >= (uint16_t)(fcCtx.listTop + UI_LOOT_ROWS))
    fcCtx.listTop = (uint16_t)(idx - UI_LOOT_ROWS + 1);
  uint16_t maxTop = n > UI_LOOT_ROWS ? (uint16_t)(n - UI_LOOT_ROWS) : 0;
  if (fcCtx.listTop > maxTop)
    fcCtx.listTop = maxTop;
}

static void sortList(uint8_t t) {
  uint16_t idx = fcCtx.listSel ? (uint16_t)(fcCtx.listSel - 1) : 0;
  const Loot *cur = LOOT_Item(idx);
  uint32_t f = cur ? cur->f : 0;
  if (t == fcCtx.sortIdx)
    fcCtx.sortRev = !fcCtx.sortRev;
  else {
    fcCtx.sortIdx = t;
    fcCtx.sortRev = false;
  }
  LOOT_Sort(LOOT_SORTERS[t], fcCtx.sortRev);
  if (f) {
    int16_t i = LOOT_IndexOf(LOOT_Get(f));
    fcCtx.listSel = i < 0 ? 0 : (uint16_t)(i + 1);
  }
  fcCtx.listTop = 0;
  clampListSel();
}

/* Настроить приём на выбранную находку (снимок скана — в listSaved). */
static void tuneLoot(const Loot *it) {
  if (!it)
    return;
  currentVfo = fcCtx.scanVfo;
  VFO *v = &vfos[fcCtx.scanVfo];
  v->rxF = it->f;
  v->modulation = static_cast<ModulationType>(it->modulation & 7);
  v->bw = it->bw;
  v->radio = static_cast<Radio>(it->radio <= 2 ? it->radio : 0);
  v->radioManual = true;
  Radio_TuneTo(it->f, true);
}

/* Выход из списка: вернуть частоту/модуляцию скана и продолжить FC. */
static void leaveLootList(void) {
  fcCtx.listSel = 0;
  fcCtx.listTop = 0;
  fcCtx.msg[0] = 0;
  /* precise-настройка Vfo_Restore снова приоткрывает звук — закрываем. */
  Radio_ScanAudio(false);
  Vfo_Restore(fcCtx.listSaved, true);
  currentVfo = fcCtx.scanVfo;
  BK4819_SetAFC(0);
  resumeScan();
}

static void listMove(int8_t d) {
  uint16_t n = LOOT_Size();
  if (!n) {
    if (fcCtx.listSel)
      leaveLootList();
    return;
  }
  if (!fcCtx.listSel) {
    /* Вход в список: снимок VFO скана, чтобы вернуть его на выходе. */
    fcCtx.listSaved = Vfo_Capture(fcCtx.scanVfo);
    fcCtx.listSel = d > 0 ? 1 : n;
    disableScan();
    LOOT_Standby();
    fcCtx.stopped = false;
    fcCtx.stopFreq = 0;
    fcCtx.resumeAt = 0;
    Radio_ScanAudio(false);
    Mode_SetDisplayRxGate(false); /* слушаем выбранную — без провалов звука */
    LOOT_Sort(LOOT_SORTERS[fcCtx.sortIdx], fcCtx.sortRev);
    fcCtx.listTop = 0;
  } else if (d > 0) {
    if (fcCtx.listSel < n)
      fcCtx.listSel++;
    else {
      leaveLootList();
      return;
    }
  } else {
    if (fcCtx.listSel > 1)
      fcCtx.listSel--;
    else {
      leaveLootList();
      return;
    }
  }
  clampListSel();
  tuneLoot(LOOT_Item((uint16_t)(fcCtx.listSel - 1)));
}

static void deleteSelectedLoot(void) {
  if (!fcCtx.listSel)
    return;
  LOOT_Remove((uint16_t)(fcCtx.listSel - 1));
  uint16_t n = LOOT_Size();
  if (!n) {
    leaveLootList();
    return;
  }
  if (fcCtx.listSel > n)
    fcCtx.listSel = n;
  clampListSel();
  tuneLoot(LOOT_Item((uint16_t)(fcCtx.listSel - 1)));
}

static void saveSelectedLootToChannel(void) {
  const Loot *it = LOOT_Item((uint16_t)(fcCtx.listSel - 1));
  if (!it)
    return;
  CH ch;
  memset(&ch, 0, sizeof(ch));
  ch.f = it->f;
  ch.modulation = (uint8_t)(it->modulation & 7);
  ch.bw = (uint8_t)(it->bw & 15);
  ch.radio = (uint8_t)(it->radio & 3);
  ch.tone = it->code;
  ch.toneSql = it->code != TONE_CODE_NONE;
  ch.scanlists = CH_GetScanlistMask();
  msgSet(CH_Add(&ch) ? "SAVED" : "ADD ERR");
}

static void captureSelectedLoot(void) {
  const Loot *it = LOOT_Item((uint16_t)(fcCtx.listSel - 1));
  if (!it)
    return;
  currentVfo = fcCtx.savedVfo;
  uint32_t f = roundToStep(it->f);
  vfos[currentVfo].modulation =
      static_cast<ModulationType>(it->modulation & 7);
  vfos[currentVfo].bw = it->bw;
  vfos[currentVfo].rxF = f;
  Radio_TuneTo(f, true);
  fcCtx.listCapture = true;
  fcCtx.captureF = 0;
  Mode_Switch(MODE_VFO);
}

class FcMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("FC SCAN");
    s_titleState = "FC SCAN";
    fcCtx.savedVfo = currentVfo;
    if (vfos[currentVfo].radio != RADIO_BK4819A)
      currentVfo = 0;
    fcCtx.scanVfo = currentVfo;
    fcCtx.savedMonitors = 0;
    for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
      if (vfos[i].monitor) {
        fcCtx.savedMonitors |= (uint8_t)(1u << i);
        vfos[i].monitor = false;
      }
    }
    Radio_Init();
    fcCtx.savedFreq = vfos[fcCtx.scanVfo].rxF; /* после Radio_Init */

    fcCtx.band = 0;
    fcCtx.scanSpeed = (uint8_t)Settings_Get(SET_FCTIME);
    if (fcCtx.scanSpeed > 3)
      fcCtx.scanSpeed = 3;
    fcCtx.thr = 0x244;
    fcCtx.bandAutoSwitch = false; /* без авто-переключения: реле не щёлкает */
    fcCtx.isScanning = false;
    fcCtx.currentFrequency = 0;
    fcCtx.lastDetectedFrequency = 0;
    fcCtx.frequencyHits = 0;
    fcCtx.bandSwitchCounter = 0;
    fcCtx.stopped = false;
    fcCtx.stopFreq = 0;
    fcCtx.resumeAt = 0;
    fcCtx.captureF = 0;
    fcCtx.listSel = 0;
    fcCtx.listTop = 0;
    fcCtx.sortIdx = LOOT_SORT_TIME;
    fcCtx.sortRev = false;
    fcCtx.listCapture = false;
    fcCtx.msg[0] = 0;
    fcCtx.msgUntil = 0;

    FillRect(0, FC_STATUS_H, LCD_WIDTH, LCD_HEIGHT - FC_STATUS_H, C_BLACK);
    lv_port_set_direct_areas(false); /* FC — обычный полноэкранный текст */
    /* FC не гасит приём на время flush (в отличие от сканера/анализатора);
     * сбрасываем флаг, чтобы не унаследовать его от предыдущего режима. */
    Mode_SetDisplayRxGate(false);
    Radio_ScanAudio(false);
    selectBand();
    enableScan();
  }

  void exit() override {
    disableScan();
    LOOT_Standby();
    Mode_SetDisplayRxGate(false);
    fcCtx.listSel = 0;
    BK4819_InvalidateFilterCache(); /* вернуть авто-фильтр VFO */
    /* BK остаётся настроенным на последнюю частоту/фильтр счётчика —
     * возвращаем VFO его частоту и настраиваем чип (иначе шумодав VFO читается
     * по чужой частоте). MENU/PTT/'*' на находке — забрать её в VFO. */
    if (fcCtx.listCapture) {
      /* Выход из списка по MENU: captureSelectedLoot уже настроил VFO. */
      currentVfo = fcCtx.savedVfo;
    } else if (fcCtx.captureF && fcCtx.scanVfo == fcCtx.savedVfo) {
      currentVfo = fcCtx.savedVfo;
      Radio_TuneToAuto(fcCtx.captureF); /* модуляция и шаг диапазона */
    } else {
      currentVfo = fcCtx.scanVfo;
      vfos[currentVfo].rxF = fcCtx.savedFreq;
      Radio_TuneTo(fcCtx.savedFreq, true);
    }
    fcCtx.listCapture = false;
    fcCtx.captureF = 0;
    for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++)
      if (fcCtx.savedMonitors & (1u << i))
        vfos[i].monitor = true;
    currentVfo = fcCtx.savedVfo;
    Radio_ScanAudio(false); /* precise-настройка приоткрыла звук — глушим;
                               дальше решает шумодав VFO (Radio_Update) */
  }

  void update() override {
    if (fcCtx.listSel) {
      Radio_Update(); /* шумодав/звук выбранной находки */
      return;         /* счётчик стоит, пока список выбран */
    }
    if (fcCtx.isScanning) {
      uint32_t freq;
      if (BK4819_GetFrequencyScanResult(&freq)) {
        disableScan();
        fcCtx.currentFrequency = freq;
        bool ok = freqAccepted(freq) && carrierPresent(freq);

        if (ok && DeltaF(freq, fcCtx.lastDetectedFrequency) < HIT_TOL)
          fcCtx.frequencyHits++;
        else
          fcCtx.frequencyHits = ok ? 1 : 0;
        fcCtx.lastDetectedFrequency = ok ? freq : 0;

        if (fcCtx.frequencyHits >= REQUIRED_HITS)
          stopOnHit(freq);
        else
          enableScan();
      }

      if (fcCtx.isScanning) {
        if (fcCtx.bandAutoSwitch &&
            fcCtx.bandSwitchCounter >= BAND_SWITCH_INTERVAL)
          nextBand();
        if (millis() - fcCtx.lastScanTime >= (200u << fcCtx.scanSpeed)) {
          fcCtx.bandSwitchCounter++;
          fcCtx.lastScanTime = millis();
        }
      }
    } else if (fcCtx.stopped) {
      Radio_Update(); /* шумодав/звук/LED */
      bool sqOpen = (squelchLevel == 0) || BK4819_IsSquelchOpen();
      uint32_t now = millis();
      if (sqOpen) {
        LOOT_Hit(fcCtx.stopFreq, BK4819_GetRSSI(), vfos[currentVfo].modulation,
                 vfos[currentVfo].bw, vfos[currentVfo].radio);
        fcCtx.resumeAt = 0;
        /* SQ=0 — «всегда открыт»: через HOLD_MAX_SQ0_MS идём дальше */
        if (squelchLevel == 0 && now - fcCtx.stoppedAt >= HOLD_MAX_SQ0_MS)
          resumeScan();
      } else if (!fcCtx.resumeAt) {
        fcCtx.resumeAt = now + RESUME_MS; /* без блокировки цикла */
      } else if ((int32_t)(now - fcCtx.resumeAt) >= 0) {
        resumeScan();
      }
    } else {
      enableScan();
    }
  }

  void render() override {
    STATUSLINE_render();

    char buf[34];

    /* шапка: диапазон | шаг VFO (крупно) | порог/шумодав */
    uint32_t step = StepFrequencyTable[vfos[fcCtx.scanVfo].step];
    char stepBuf[10];
    fmt_step(stepBuf, sizeof(stepBuf), step);
    PrintfT(UI_LEFT_X, UI_RANGE_BASE,
            text_style(C_WHITE, C_BLACK, POS_L, F_M), "%s",
            BANDS[fcCtx.band].name);
    PrintfT(LCD_XCENTER, UI_RANGE_BASE,
            text_style(C_YELLOW, C_BLACK, POS_C, F_M), "%s", stepBuf);
    snprintf(buf, sizeof(buf), "THR%03X SQ%u%s", (unsigned)fcCtx.thr,
             (unsigned)squelchLevel, fcCtx.bandAutoSwitch ? "A" : "");
    PrintfT(UI_RIGHT_X, UI_RANGE_BASE,
            text_style(C_WHITE, C_BLACK, POS_R, F_S), "%s", buf);

    /* Крупная частота: в FC округляем до шага VFO; точная — в строке находки. */
    bool browsing = fcCtx.listSel != 0;
    const Loot *selIt =
        browsing ? LOOT_Item((uint16_t)(fcCtx.listSel - 1)) : nullptr;
    uint32_t rawF = selIt ? selIt->f
                    : fcCtx.stopped ? fcCtx.stopFreq
                                    : fcCtx.currentFrequency;
    char bigFreq[16];
    fmt_mhz3(bigFreq, sizeof(bigFreq), roundToStep(rawF));
    PrintfT(LCD_XCENTER, UI_SCAN_FREQ_BASE,
            text_style(browsing ? C_CYAN : C_WHITE, C_BLACK, POS_C, F_XL), "%s",
            bigFreq);

    const char *state = browsing ? "FC LOOT"
                        : fcCtx.stopped ? "FC HOLD" : "FC SCAN";
    if (state != s_titleState) {
      s_titleState = state;
      STATUSLINE_SetTitle(state);
    }

    const uint16_t infoY = UI_SCAN_INFO_BASE;

    /* Строка деталей: находка/выдержка/счётчик или запись списка. */
    if (browsing && selIt) {
      if (fcCtx.msg[0] && (int32_t)(fcCtx.msgUntil - millis()) > 0) {
        PrintfT(UI_LEFT_X, infoY,
                text_style(C_YELLOW, C_BLACK, POS_L, F_S), "%s", fcCtx.msg);
      } else {
        fcCtx.msg[0] = 0;
        char dur[5];
        fmt_duration(dur, sizeof(dur), selIt->duration);
        PrintfT(UI_LEFT_X, infoY,
                text_style(C_WHITE, C_BLACK, POS_L, F_S), "%s %s %+ddBm",
                MOD_NAMES[selIt->modulation & 7],
                RADIO_NAMES[selIt->radio > 2 ? 2 : selIt->radio],
                (int)selIt->peak - 160);
        PrintfT(LCD_XCENTER, infoY,
                text_style(C_GRAY, C_BLACK, POS_C, F_S), "dur %s", dur);
      }
      PrintfT(UI_RIGHT_X, infoY,
              text_style(C_GRAY, C_BLACK, POS_R, F_S), "%u/%u %s%s",
              (unsigned)fcCtx.listSel, (unsigned)LOOT_Size(),
              LOOT_SORT_NAMES[fcCtx.sortIdx], fcCtx.sortRev ? " R" : "");
    } else {
      const Loot *ll = gLastActiveLoot;
      if (ll) {
        char fb[16];
        fmt_mhz5(fb, sizeof(fb), ll->f);
        PrintfT(UI_LEFT_X, infoY,
                text_style(ll->open ? C_YELLOW : C_WHITE, C_BLACK, POS_L, F_S),
                "%c%s", ll->blacklist ? '-' : ll->whitelist ? '+' : ' ', fb);
      } else {
        PrintfT(UI_LEFT_X, infoY,
                text_style(C_GRAY, C_BLACK, POS_L, F_S), "no loot yet");
      }
      /* Время на замер — то же место и шрифт, что в сканере. */
      PrintfT(LCD_XCENTER, infoY,
              text_style(C_GRAY, C_BLACK, POS_C, F_S), "%ums",
              (unsigned)(200u << fcCtx.scanSpeed));
      if (fcCtx.stopped)
        snprintf(buf, sizeof(buf), "L%u %s", (unsigned)LOOT_Size(),
                 BK4819_IsSquelchOpen() ? "OPEN" : "CLSD");
      else
        snprintf(buf, sizeof(buf), "L%u", (unsigned)LOOT_Size());
      PrintfT(UI_RIGHT_X, infoY,
              text_style(C_GRAY, C_BLACK, POS_R, F_S), "%s", buf);
    }

    drawLootList();

    /* Подсказки: в списке — управление находками, в скане — параметры FC. */
    if (browsing) {
      PrintfT(4, LCD_HEIGHT - 16,
              text_style(C_GRAY, C_BLACK, POS_L, F_S),
              "1-4 SORT S1 BL S2 WL 0 DEL");
      PrintfT(4, LCD_HEIGHT - 4,
              text_style(C_GRAY, C_BLACK, POS_L, F_S),
              "LS2 CH MENU>VFO EXIT SCAN");
    } else {
      PrintfT(4, LCD_HEIGHT - 16,
              text_style(C_GRAY, C_BLACK, POS_L, F_S),
              "1/7 SPD 3/9 SQ 5 THR #/0 BAND");
      PrintfT(4, LCD_HEIGHT - 4,
              text_style(C_GRAY, C_BLACK, POS_L, F_S),
              "UD LOOT S1-BL S2+WL *LST MENU>VFO");
    }
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    /* Встроенный список: те же переходы и действия, что в сканере. */
    if (fcCtx.listSel) {
      if (state == KEY_LONG_PRESSED) {
        switch (k) {
        case KEY_5: /* whitelist-находки → каналы */
          msgSetSaved("SAVED", CH_SaveWhitelistFromLoot());
          return true;
        case KEY_8: /* blacklist-находки → каналы */
          msgSetSaved("SAVED", CH_SaveBlacklistFromLoot());
          return true;
        case KEY_9: /* чёрный список каналов → loot */
          msgSetSaved("LOADED", CH_LoadBlacklistToLoot());
          return true;
        case KEY_SIDE2: /* выбранную находку — в канал */
          saveSelectedLootToChannel();
          return true;
        case KEY_0: /* очистить весь список и вернуться к скану */
          LOOT_Clear();
          leaveLootList();
          return true;
        default:
          return true;
        }
      }
      if (state == KEY_LONG_PRESSED_CONT &&
          (k == KEY_UP || k == KEY_DOWN)) {
        listMove(k == KEY_UP ? -1 : +1);
        return true;
      }
      if (state != KEY_RELEASED)
        return true;

      switch (k) {
      case KEY_UP:
        listMove(-1);
        return true;
      case KEY_DOWN:
        listMove(+1);
        return true;
      case KEY_1:
        sortList(LOOT_SORT_TIME);
        return true;
      case KEY_2:
        sortList(LOOT_SORT_DUR);
        return true;
      case KEY_3:
        sortList(LOOT_SORT_BL);
        return true;
      case KEY_4:
        sortList(LOOT_SORT_F);
        return true;
      case KEY_SIDE1:
        LOOT_ToggleBlacklist(LOOT_Item((uint16_t)(fcCtx.listSel - 1)));
        return true;
      case KEY_SIDE2:
        LOOT_ToggleWhitelist(LOOT_Item((uint16_t)(fcCtx.listSel - 1)));
        return true;
      case KEY_0:
        deleteSelectedLoot();
        return true;
      case KEY_MENU: /* выбранную частоту — в VFO и выйти */
        captureSelectedLoot();
        return true;
      case KEY_EXIT: /* назад к сканированию */
        leaveLootList();
        return true;
      default:
        return true;
      }
    }

    if (state == KEY_LONG_PRESSED && k == KEY_MENU) {
      LOOTLIST_OpenFrom(MODE_FC);
      return true;
    }
    if (state != KEY_EVT_RELEASE)
      return false;

    switch (k) {
    case KEY_1:
    case KEY_7:
      fcCtx.scanSpeed = IncDecU(fcCtx.scanSpeed, 0, 3, k == KEY_1);
      if (fcCtx.isScanning) {
        disableScan();
        enableScan();
      }
      return true;
    case KEY_3:
    case KEY_9:
      squelchLevel = IncDecU(squelchLevel, 0, 10, k == KEY_3);
      Radio_SetSquelch(squelchLevel);
      return true;
    case KEY_5:
      fcCtx.thr = fcCtx.thr == 0x244 ? 0x580 : 0x244;
      if (fcCtx.isScanning) {
        disableScan();
        enableScan();
      }
      return true;
    case KEY_HASH: /* диапазон вручную */
      nextBand();
      return true;
    case KEY_0:
      fcCtx.bandAutoSwitch = !fcCtx.bandAutoSwitch;
      return true;
    case KEY_SIDE1: /* находку в чёрный список и дальше */
      if (fcCtx.stopped) {
        LOOT_BlacklistLast();
        resumeScan();
      }
      return true;
    case KEY_SIDE2: /* в белый список и дальше */
      if (fcCtx.stopped) {
        LOOT_WhitelistLast();
        resumeScan();
      }
      return true;
    case KEY_STAR: /* список находок */
      LOOTLIST_OpenFrom(MODE_FC);
      return true;
    case KEY_UP: /* в список находок (0 — текущий скан, 1..N — находки) */
      listMove(-1);
      return true;
    case KEY_DOWN:
      listMove(+1);
      return true;
    case KEY_MENU:
    case KEY_PTT:
      fcCtx.captureF = fcCtx.stopped ? roundToStep(fcCtx.stopFreq) : 0;
      Mode_Switch(MODE_VFO);
      return true;
    case KEY_EXIT:
      fcCtx.captureF = 0;
      Mode_Switch(MODE_VFO);
      return true;
    default:
      break;
    }
    return false;
  }
};

FcMode fcModeInstance;
Mode &MODE_FC = fcModeInstance;
