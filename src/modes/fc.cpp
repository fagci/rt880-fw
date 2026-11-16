/* FC — частотомер BK4819 («быстрый скан»).
 *
 * Аппаратный счётчик BK4819 ловит несущую передатчика на всём диапазоне за
 * 50–200 мс — гораздо быстрее RSSI-развёртки, но находит только активные
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
 * список и дальше, MENU или '*' или PTT — частоту в VFO и выход, EXIT —
 * выход без неё, длинный MENU — список находок.
 */
#include "fc.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/log.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "loot_screen.h"
#include "mode.h"
#include "vfo.h"
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
};
static const FcBand BANDS[] = {
    {"VHF", 14500000, 3000000, 24000000},
    {"UHF", 43300000, 24000000, 80000000},
    {"800", 90000000, 80000000, 130000000},
};
#define BAND_N (sizeof(BANDS) / sizeof(BANDS[0]))

struct {
  uint8_t scanSpeed;
  uint16_t hz;
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
  uint8_t scanVfo;
  uint32_t savedFreq;
  uint32_t captureF; /* частота в VFO при выходе (0 — вернуть прежнюю) */
} fcCtx;

/* кэш строк экрана: перерисовываем только изменившиеся */
#define FC_LINES 12
static char s_line[FC_LINES][34];
static uint16_t s_lineColor[FC_LINES];

static void drawLine(uint8_t idx, int16_t x, int16_t y, int16_t clearH,
                     TextPos pos, uint16_t color, const GFXfont *font,
                     const char *text) {
  if (s_lineColor[idx] == color && !strcmp(s_line[idx], text))
    return;
  FillRect(0, y - clearH + 4, LCD_WIDTH, clearH, C_BLACK);
  PrintfEx(x, y, pos, color, C_BLACK, font, "%s", text);
  strncpy(s_line[idx], text, sizeof(s_line[idx]) - 1);
  s_line[idx][sizeof(s_line[idx]) - 1] = 0;
  s_lineColor[idx] = color;
}

static void invalidateLines(void) {
  for (uint8_t i = 0; i < FC_LINES; i++) {
    s_line[i][0] = 1; /* заведомо не совпадёт с текстом */
    s_line[i][1] = 0;
    s_lineColor[i] = 0xFFFF;
  }
}

static void selectBand(void) {
  BK4819_SelectChip(0);
  BK4819_SelectFilterByFrequency(BANDS[fcCtx.band].rep);
}

static void enableScan() {
  BK4819_EnableFrequencyScanEx2(
      (FreqScanTime)(fcCtx.scanSpeed > F_SC_T_COUNT - 1 ? 0 : fcCtx.scanSpeed),
      fcCtx.hz);
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

/* Остановились на находке: настройка приёмника, учёт в списке */
static void stopOnHit(uint32_t freq) {
  disableScan();
  Radio_TuneTo(freq, true); /* precise: модуляция, фильтр диапазона, шумодав */
  fcCtx.stopped = true;
  fcCtx.stopFreq = freq;
  fcCtx.stoppedAt = millis();
  fcCtx.resumeAt = 0;
  fcCtx.frequencyHits = 0;
  Log("FC hit %lu", (unsigned long)freq);
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

class FcMode : public Mode {
public:
  void enter() override {
    Log("FC enter");
    fcCtx.savedVfo = currentVfo;
    if (vfos[currentVfo].radio != RADIO_BK4819A)
      currentVfo = 0;
    fcCtx.scanVfo = currentVfo;
    Radio_Init();
    fcCtx.savedFreq = vfos[fcCtx.scanVfo].rxF; /* после Radio_Init */

    fcCtx.band = 0;
    fcCtx.scanSpeed = 0;
    fcCtx.hz = 0x244;
    fcCtx.bandAutoSwitch = true;
    fcCtx.isScanning = false;
    fcCtx.currentFrequency = 0;
    fcCtx.lastDetectedFrequency = 0;
    fcCtx.frequencyHits = 0;
    fcCtx.bandSwitchCounter = 0;
    fcCtx.stopped = false;
    fcCtx.stopFreq = 0;
    fcCtx.resumeAt = 0;
    fcCtx.captureF = 0;

    FillRect(0, FC_STATUS_H, LCD_WIDTH, LCD_HEIGHT - FC_STATUS_H, C_BLACK);
    invalidateLines();
    Radio_ScanAudio(false);
    selectBand();
    enableScan();
  }

  void exit() override {
    disableScan();
    LOOT_Standby();
    /* BK остаётся настроенным на последнюю частоту/фильтр счётчика —
     * возвращаем VFO его частоту и настраиваем чип (иначе шумодав VFO читается
     * по чужой частоте). MENU/PTT/'*' на находке — забрать её в VFO. */
    if (fcCtx.captureF && fcCtx.scanVfo == fcCtx.savedVfo) {
      currentVfo = fcCtx.savedVfo;
      Radio_TuneToAuto(fcCtx.captureF); /* модуляция и шаг диапазона */
    } else {
      currentVfo = fcCtx.scanVfo;
      vfos[currentVfo].rxF = fcCtx.savedFreq;
      Radio_TuneTo(fcCtx.savedFreq, true);
    }
    currentVfo = fcCtx.savedVfo;
    Radio_ScanAudio(false); /* precise-настройка приоткрыла звук — глушим;
                               дальше решает шумодав VFO (Radio_Update) */
  }

  void update() override {
    if (fcCtx.isScanning) {
      uint32_t freq;
      if (BK4819_GetFrequencyScanResult(&freq)) {
        fcCtx.currentFrequency = freq;
        bool ok = freqAccepted(freq);

        if (ok && DeltaF(freq, fcCtx.lastDetectedFrequency) < HIT_TOL)
          fcCtx.frequencyHits++;
        else
          fcCtx.frequencyHits = ok ? 1 : 0;
        fcCtx.lastDetectedFrequency = ok ? freq : 0;

        if (fcCtx.frequencyHits >= REQUIRED_HITS) {
          stopOnHit(freq);
        } else {
          disableScan();
          enableScan();
        }
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

    /* строка настроек */
    snprintf(buf, sizeof(buf), "%s %ums HZ %u SQ %u%s", BANDS[fcCtx.band].name,
             200u << fcCtx.scanSpeed, (unsigned)fcCtx.hz,
             (unsigned)squelchLevel, fcCtx.bandAutoSwitch ? " [A]" : "");
    drawLine(0, 4, FC_STATUS_H + 16, 16, POS_L, C_WHITE, F_SM, buf);

    /* частота: последняя найденная счётчиком / на которой стоим */
    uint32_t f = fcCtx.stopped ? fcCtx.stopFreq : fcCtx.currentFrequency;
    snprintf(buf, sizeof(buf), "%lu.%03lu", (unsigned long)(f / MHZ),
             (unsigned long)((f % MHZ) / 1000));
    drawLine(1, LCD_XCENTER, FC_STATUS_H + 52, 34, POS_C,
             fcCtx.stopped ? C_GREEN : C_YELLOW, F_MONO_LG, buf);

    /* шумодав на находке */
    if (fcCtx.stopped)
      snprintf(buf, sizeof(buf), "Squelch: %s",
               BK4819_IsSquelchOpen() ? "OPEN" : "CLOSED");
    else
      snprintf(buf, sizeof(buf), "scanning...");
    drawLine(2, LCD_XCENTER, FC_STATUS_H + 76, 16, POS_C,
             fcCtx.stopped ? C_GREEN : C_GRAY, F_SM, buf);

    /* последние находки из списка (свежие — выше) */
    uint16_t n = LOOT_Size();
    for (uint8_t r = 0; r < 6; r++) {
      if (r < n) {
        const Loot *l = LOOT_Item(n - 1 - r); /* добавлялись в конец */
        uint32_t ago = LOOT_SecondsAgo(l);
        snprintf(buf, sizeof(buf), "%c%4u.%05u %2u:%02u",
                 l->blacklist ? '-' : l->whitelist ? '+' : ' ',
                 (unsigned)(l->f / MHZ), (unsigned)(l->f % MHZ),
                 (unsigned)(ago / 60 % 100), (unsigned)(ago % 60));
      } else {
        buf[0] = ' ';
        buf[1] = 0;
      }
      drawLine(3 + r, 4, FC_STATUS_H + 110 + r * 17, 16, POS_L,
               (r < n && LOOT_Item(n - 1 - r)->open) ? C_YELLOW : C_WHITE,
               F_SM, buf);
    }

    drawLine(9, 4, LCD_HEIGHT - 12, 12, POS_L, C_GRAY, F_SS,
             "1/7 SPD 3/9 SQ 5 HZ # BAND 0 AUTO");
    drawLine(10, 4, LCD_HEIGHT - 5, 8, POS_L, C_GRAY, F_SS,
             "S1 -BL S2 +WL MENU>VFO EXIT");
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
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
      fcCtx.hz = fcCtx.hz == 0x244 ? 0x580 : 0x244;
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
    case KEY_MENU:
    case KEY_STAR:
    case KEY_PTT:
      fcCtx.captureF = fcCtx.stopped ? fcCtx.stopFreq : 0;
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
