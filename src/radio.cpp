#include "radio.h"
#include "bands.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/si473x.h"
#include "driver/w25q32.h"
#include "helper/log.h"
#include "helper/settings.h"
#include "helper/measurements.h"
#include "helper/store.h"
#include "helper/tone.h"
#include "misc.h"
#include "ui/graphics.h"
#include <string.h>

VFO vfos[DEVICE_VFO_COUNT] = {{}, {}, {}};
VFOChannel vfoChannels[DEVICE_VFO_COUNT] = {{}, {}, {}};
/* Частотное состояние VFO на время канального режима. */
static VFO vfoSaved[DEVICE_VFO_COUNT];

uint8_t currentVfo = 0;

Filter filters[] = {FILTER_HF, FILTER_VHF, FILTER_UHF, FILTER_800};
int8_t filterIndex = -1; /* -1 = AUTO, 0..3 = filters[] */

const uint16_t StepFrequencyTable[STEP_COUNT] = {
    2,   5,    50,   100,  250,  500,   625,   833,
    900, 1000, 1250, 2500, 5000, 10000, 50000,
};

/* Названия шагов для отображения */
const char *const STEP_NAMES[STEP_COUNT] = {
    "20Hz", "50Hz", "500Hz", "1k",  "2.5k", "5k",   "6.25k", "8.33k",
    "9k",   "10k",  "12.5k", "25k", "50k",  "100k", "500k",
};

uint8_t squelchLevel = 4;

static bool bkDacOn[2] = {false, false};
static bool siMuted = true; /* SI стартует замьюченным (громкость 0) */
static bool afMuteOn = false;
static bool s_pendingApply; /* SwitchToVfo отложил тяжёлое применение */
static bool s_bootMute;     /* старт: не открывать звук до первого опроса шумодава */

/* Как часто BK «бегает» по закреплённым VFO и сколько сидит на каждом */
#define MON_DWELL_MS 120
#define MON_SETTLE_MS 15  /* PLL/AGC после перестройки */
#define MON_CONFIRM_MS 20 /* второй отсчёт шумодава: отсечь всплеск */
#define MON_RETURN_MS 1500 /* BK молчит столько — вернуться на прежний VFO */
static uint32_t s_lastMon;
/* Возврат после перехвата: на каком VFO поймали и куда вернуться. */
static int8_t s_monReturn = -1;
static uint8_t s_monCaught;
static uint32_t s_monClosedAt;
static uint32_t s_monCheckAt;

/* Выставить модуляцию и шаг активного VFO по плану. false — вне плана. */
bool Radio_ApplyBandPlan(uint32_t f) {
  const BandPlan *b = Band_Find(f);
  if (!b)
    return false;
  vfos[currentVfo].modulation = b->mod;
  vfos[currentVfo].step = static_cast<Step>(b->step);
  return true;
}

/* ── BK4819 B (U400) ───────────────────────────────────────────
 * Приёмного входа у B нет (схема RT880 V2, замер: ≈ −133 дБм на любом
 * сигнале), передачи прошивка пока не делает — B спит, как в стоке
 * (sleep_b: REG_30 = 0x0000, REG_37 = 0x1D00; экономит порядка 15–20 мА).
 * BK_RX_CHIPS — сколько чипов участвует в приёме/звуке/шумодаве. */
#define BK_RX_CHIPS 1

static void bkSleepB(void) {
  BK4819_SelectChip(1);
  BK4819_WriteRegister(BK4819_REG_30, 0x0000);
  BK4819_WriteRegister(BK4819_REG_37, 0x1D00);
}

/* ── Выбор приёмника ───────────────────────────────────────────
 * f — в единицах 10 Гц.
 * Факты о плате (схема RT880 V2 и разбор стока, uv98-rust/research/dual-rx,
 * проверено замером RSSI на BK B):
 *  - BK4819 A (U201) — единственный приёмник на антенне: LNA/фильтры через
 *    PF0/K200, диапазоны 18–660 и 840–1300 МГц (в т. ч. 27 МГц, CB);
 *  - BK4819 B (U400) — вход LNAIN не подключён к антенне, выход EARO отключён
 *    (C433 = NC): второй BK нужен для передачи/ретранслятора, а НЕ как
 *    приёмник. Приём на B не выбираем;
 *  - SI4732 — всё ниже 18 МГц (где BK не принимает), AM/SSB до 30 МГц и
 *    FM-вещание 64–108 МГц (приёмник чипа: качество, стерео/RDS).
 * Цена SI в FM: AM/SSB ↔ FM = re-power и перезаливка патча (≈0.2–0.8 с).
 * Нет SI — откат на BK A (siEnsure). */
Radio Radio_PickDevice(uint32_t f, uint8_t mod) {
  if (mod == MOD_WFM && f >= 6400000 && f <= 10800000)
    return RADIO_SI4732;
  /* Upconverter: BK принимает сдвинутую частоту, даже если логическая
   * ниже 18 МГц. SI4732 к преобразователю не подключён. */
  int32_t up = Settings_Get(SET_UPCONVERTER) / 10;
  if (up && (int64_t)f + up >= 1800000 &&
      (int64_t)f + up <= BK4819_F_MAX)
    return RADIO_BK4819A;
  if (f < 1800000) /* ниже 18 МГц BK4819 не принимает */
    return RADIO_SI4732;
  if (f < 3000000 && (mod == MOD_AM || mod == MOD_LSB || mod == MOD_USB))
    return RADIO_SI4732;
  return RADIO_BK4819A; /* 18–30 МГц FM/прочее; CB AM по умолчанию SI */

}

/* Частотные окна приёмников для ручного переключения: там, где чип
 * физически работает, даже если авторежим выбрал бы другой. */
static bool deviceCovers(Radio r, uint32_t f) {
  if (r == RADIO_BK4819A) {
    int64_t hw = (int64_t)f + Settings_Get(SET_UPCONVERTER) / 10;
    return hw >= 1800000 && hw <= BK4819_F_MAX;
  }
  if (r == RADIO_SI4732)
    return (f >= 15300 && f <= 3000000) || /* 153 кГц–30 МГц (AM/SSB) */
           (f >= 6400000 && f <= 10800000); /* 64–108 МГц (FM) */
  return false;
}

/* Ручное переключение: AUTO → другой приёмник → AUTO. Если текущую
 * частоту поддерживает только один чип, переключения нет. */
void Radio_NextReceiver(void) {
  VFO *v = &vfos[currentVfo];
  Radio autoR = Radio_PickDevice(v->rxF, v->modulation);

  if (v->radioManual) {
    v->radioManual = false;
    v->radio = autoR;
  } else {
    Radio other = (autoR == RADIO_SI4732) ? RADIO_BK4819A : RADIO_SI4732;
    if (!deviceCovers(other, v->rxF))
      return;
    v->radioManual = true;
    v->radio = other;
  }
  Radio_TuneTo(v->rxF, true);
}

static SI47XX_MODE modToSi(uint8_t mod) {
  switch (mod) {
    case MOD_AM:  return SI47XX_AM;
    case MOD_LSB: return SI47XX_LSB;
    case MOD_USB: return SI47XX_USB;
    default:      return SI47XX_FM;
  }
}

/* Включает SI4732 при первом использовании; держит его в патч-режиме
   (патч покрывает AM+SSB): повторной заливки между AM↔SSB нет.
   Переходы внутри патч-режима — только смена глобалки si4732mode
   (драйвер сам кладёт её в аргументы команд). Re-power только если
   чип не в патче, а нужен SSB. При отсутствии чипа — откат на BK. */
/* SI4732: питание и нужный continuous-патч через SI47XX_Acquire.
 * Переходы AM↔SSB внутри патча — мгновенные; смена патча — re-power
 * (~1-2 с). При отсутствии чипа — откат VFO на BK. */
static void siEnsure(VFO *v) {
  SI47XX_MODE want = modToSi(v->modulation);
  /* FM-приёмник SI4732 — только 64–108 МГц; ниже (КВ) при «FM»-модуляции
   * работаем в AM */
  if (want == SI47XX_FM && v->rxF < 6400000)
    want = SI47XX_AM;
  if (!SI47XX_Acquire(want)) {
    Log("SI4732: not responding, fallback to BK A");
    v->radio = RADIO_BK4819A; /* B без антенны — не вариант */
    v->radioManual = false;
  }
}

/* ── Аудиотракт ────────────────────────────────────────────────
 * BK AF DAC — по кристаллу активного VFO (звук только активного).
 * SI4732 суммируется в тракт аналогово: слышен, когда активный VFO
   на нём (громкость 63), иначе глушим SetVolume(0) — чип живым.
 * AF_MUTE — общий гейт усилителя: для SI всегда открыт (шумодава
   у SI пока нет), для BK — по squelch. */
static void applyMute(bool sqOpen) {
  Radio ar = vfos[currentVfo].radio;
  bool activeSi = (ar == RADIO_SI4732);

  if (activeSi == siMuted) {
    if (isSi4732On)
      SI47XX_SetVolume(activeSi ? 63 : 0);
    siMuted = !activeSi;
  }

  uint8_t chip = (ar == RADIO_BK4819B) ? 1 : 0;
  bool wantBk = !activeSi && sqOpen;
  for (uint8_t c = 0; c < BK_RX_CHIPS; c++) {
    bool want = (c == chip) && wantBk;
    if (want != bkDacOn[c]) {
      BK4819_SelectChip(c);
      BK4819_ToggleAFDAC(want);
      bkDacOn[c] = want;
    }
  }

  led_rx_set(!activeSi && sqOpen); /* LED: шумодав BK открыт */

  bool unmute = activeSi ? true : sqOpen;
  if (unmute != afMuteOn) {
    gpio_pin_write(&PIN_AF_MUTE, unmute);
    afMuteOn = unmute;
  }
}

static uint8_t vfoChip(uint8_t idx) {
  return (vfos[idx].radio == RADIO_BK4819B) ? 1 : 0;
}

/* Частота для тракта: логическая + коррекция диапазона из флеш-списка.
 * Логическая частота (rxF) остаётся неизменной — по ней работают границы
 * диапазонов, поиск и отображение. */
static uint32_t hwFreq(uint32_t f10, bool upconv) {
  int32_t f = (int32_t)f10 + Band_Corr10(f10);
  if (upconv)
    f += (int32_t)(Settings_Get(SET_UPCONVERTER) / 10);
  if (f < 1530)
    f = 1530;
  if (f > 134000000)
    f = 134000000;
  return (uint32_t)f;
}

/* precise=true: полное применение (смена VFO, ручной ввод).
   precise=false: только частота (сканирование, мониторинг). */
static void applyVfo(bool precise) {
  VFO *v = &vfos[currentVfo];
  /* Ручной выбор приёмника живёт, пока чип поддерживает частоту; при
   * выходе за его окно возвращаемся к автовыбору. */
  if (v->radioManual && !deviceCovers(v->radio, v->rxF))
    v->radioManual = false;
  if (!v->radioManual)
    v->radio = Radio_PickDevice(v->rxF, v->modulation);

  if (v->radio == RADIO_SI4732) {
    siEnsure(v); /* может откатить v->radio на BK */
    if (v->radio == RADIO_SI4732) {
      SI47XX_TuneTo(hwFreq(v->rxF, false)); /* единицы 10 Гц */
      SI47XX_SetAutomaticGainControl(modToSi(v->modulation) == SI47XX_FM ? 0 : 1, 0);
    }
  }

  if (Settings_Get(SET_SI4732POWEROFF) && v->radio != RADIO_SI4732 &&
      isSi4732On) {
    SI47XX_PowerDown();
    siMuted = true;
  }

  if (v->radio != RADIO_SI4732) {
    uint8_t chip = vfoChip(currentVfo);
    BK4819_SelectChip(chip);
    BK4819_SelectFilterByFrequency(hwFreq(v->rxF, true));
    BK4819_TuneTo(hwFreq(v->rxF, true), precise);
    if (precise) {
      /* WFM на BK — свой широкий REG_43; в FM не подменяем */
      BK4819_SetModulation(
          static_cast<ModulationType>(v->modulation));
      /* как k1-fw: для AM отдельная (не FM) настройка AGC */
      BK4819_SetAGC(v->modulation != MOD_AM, v->gainIndex);
      /* WFM пишет REG_43 сама; обычная полоса её бы затёрла */
      if (v->modulation != MOD_WFM)
        BK4819_SetFilterBandwidth(
            static_cast<BK4819_FilterBandwidth_t>(v->bw));
      BK4819_SetupSquelch(GetSql(squelchLevel),
                       (uint8_t)Settings_Get(SET_SQLOPENTIME),
                       (uint8_t)Settings_Get(SET_SQLCLOSETIME));
      audio_path_set(chip);
    }
  }

  if (precise)
    applyMute(!s_bootMute);
}

/* Канал за VFO: приёмник B без антенны не используем, как в CH. */
static Radio channelRadio(uint8_t r) {
  if (r == RADIO_BK4819B)
    r = RADIO_BK4819A;
  if (r > RADIO_SI4732)
    r = RADIO_BK4819A;
  return (Radio)r;
}

static void applyChannelFields(VFO *v, const VFOChannel *c) {
  v->rxF = c->f;
  v->modulation = static_cast<ModulationType>(c->modulation & 7);
  v->bw = c->bw;
  v->radio = channelRadio(c->radio);
  v->radioManual = true;
  const BandPlan *b = Band_Find(c->f);
  if (b)
    v->step = static_cast<Step>(b->step);
}

/* Перехваченный BK слушаем, пока открыт шумодав; после MON_RETURN_MS
 * тишины возвращаемся на VFO, который был активен до перехвата. */
static void monitorReturnTick(void) {
  if (s_monReturn < 0)
    return;
  /* Пользователь сам сменил VFO — возврат больше не нужен. */
  if (currentVfo != s_monCaught) {
    s_monReturn = -1;
    return;
  }
  if (vfos[currentVfo].radio == RADIO_SI4732) {
    s_monReturn = -1;
    return;
  }

  uint32_t now = millis();
  if (now - s_monCheckAt < 100)
    return; /* шумодав активного BK и так читается в Radio_Update */
  s_monCheckAt = now;

  BK4819_SelectChip(vfoChip(currentVfo));
  bool open = (squelchLevel == 0) || BK4819_IsSquelchOpen();
  if (open) {
    s_monClosedAt = 0;
    return;
  }
  if (!s_monClosedAt) {
    s_monClosedAt = now;
    return;
  }
  if (now - s_monClosedAt < MON_RETURN_MS)
    return;

  uint8_t back = (uint8_t)s_monReturn;
  s_monReturn = -1;
  currentVfo = back;
  applyVfo(true);
}

/* ── Мониторинг: закреплённые VFO проверяются в фоне ──────────
 * Слушаем SI — всё равно проверяем BK: открылся шумодав → переходим на BK
 * (BK приоритетнее). Кандидаты — VFO с флагом monitor, НЕ активные и на
 * другом кристалле/приёмнике (один тракт не слушает два места).
 * SI-кандидаты не мониторим: у SI нет шумодава, а I2C-опрос щёлкает. */
static void monitorTick(void) {
  /* Шумодав выключен — критерия «открылся» нет, не прыгаем по каналам. */
  if (squelchLevel == 0)
    return;

  uint32_t now = millis();
  if (now - s_lastMon < MON_DWELL_MS)
    return;
  s_lastMon = now;

  Radio ar = vfos[currentVfo].radio;
  uint8_t cand[DEVICE_VFO_COUNT];
  uint8_t n = 0;
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    /* Канальный режим участвует в multiwatch сам, частотный — по метке. */
    bool chMode = VFO_ChannelMode(i);
    if (i == currentVfo || (!vfos[i].monitor && !chMode))
      continue;
    if (!chMode) {
      if (vfos[i].radioManual && !deviceCovers(vfos[i].radio, vfos[i].rxF))
        vfos[i].radioManual = false;
      if (!vfos[i].radioManual)
        vfos[i].radio = Radio_PickDevice(vfos[i].rxF, vfos[i].modulation);
    }
    if (vfos[i].radio == RADIO_SI4732 || vfos[i].radio == ar)
      continue;
    cand[n++] = i;
  }
  if (!n)
    return;

  static uint8_t slot;
  slot = (slot + 1) % n;
  uint8_t i = cand[slot];
  VFO *m = &vfos[i];
  uint8_t chip = vfoChip(i);

  BK4819_SelectChip(chip);
  /* Настраиваем кандидата полностью: при несовпадении модуляции/полосы с
   * тем, что осталось на BK, шумодав ложно открывается без сигнала. */
  BK4819_SelectFilterByFrequency(m->rxF);
  BK4819_TuneTo(hwFreq(m->rxF, true), false);
  BK4819_SetModulation(static_cast<ModulationType>(m->modulation));
  BK4819_SetAGC(m->modulation != MOD_AM, m->gainIndex);
  if (m->modulation != MOD_WFM)
    BK4819_SetFilterBandwidth(
        static_cast<BK4819_FilterBandwidth_t>(m->bw));
  BK4819_SetupSquelch(GetSql(squelchLevel),
                       (uint8_t)Settings_Get(SET_SQLOPENTIME),
                       (uint8_t)Settings_Get(SET_SQLCLOSETIME));
  delay_ms(MON_SETTLE_MS);
  /* Перестройка даёт короткий всплеск — требуем два открытых отсчёта. */
  bool open = BK4819_IsSquelchOpen();
  if (open) {
    delay_ms(MON_CONFIRM_MS);
    open = BK4819_IsSquelchOpen();
  }
  if (bkDacOn[chip]) { /* монитор не должен звучать */
    BK4819_ToggleAFDAC(false);
    bkDacOn[chip] = false;
  }

  if (open) {
    if (s_monReturn < 0) {
      s_monReturn = (int8_t)currentVfo; /* куда вернуться после перехвата */
      s_monClosedAt = 0;
    }
    s_monCaught = i;
    currentVfo = i;
    applyVfo(true);
  }
}

/* ── Передача (BK4819 A) ──────────────────────────────────────
 * Последовательность повторяет uv98-rust/rf-sequence для обычного FM TX:
 * внешние гейты, частота/фильтр/девиация/мощность из заводской калибровки
 * W25Q (offset 0 или 0x1000), PF0 по правилу stock, затем REG_36 последним.
 * B (U400) не используется: у него нет RX-входа, и отдельный relay-путь
 * в этой прошивке не реализован. */
#define TX_CAL_GENERAL_LEN 32u
#define TX_CAL_BAND_LEN 112u
#define TX_CAL_MARKER 0x9Au
#define TX_CAL_BACKUP 0x1000u
#define TX_RELAY_MIN_MS 300u

typedef struct {
  uint8_t tx3k;
  uint16_t devWide;
  uint16_t devNarrow;
  uint8_t high;
  uint8_t low;
} TxCal;

typedef struct {
  uint32_t lo;
  uint32_t hi;
  uint32_t origin;
  uint8_t band;
} TxCalWindow;

/* Порядок окон и повторы совпадают со stock selector 0x08012A30;
 * частотные коды — единицы 10 Гц (13 600 000 = 136 МГц). */
static const TxCalWindow TX_CAL_WINDOWS[] = {
    {13600000, 17400000, 13500000, 0},  {40000000, 48000000, 40000000, 1},
    {1800000, 6400000, 1500000, 2},     {6400000, 13600000, 6000000, 3},
    {17400000, 24000000, 17000000, 4},  {24000000, 32000000, 24000000, 5},
    {32000000, 40000000, 32000000, 6},  {48000000, 56000000, 48000000, 7},
    {56000000, 66000000, 56000000, 8},  {84000000, 92000000, 84000000, 9},
    {92000000, 100000000, 92000000, 10}, {100000000, 108000000, 100000000, 11},
    {108000000, 116000000, 108000000, 12},
    {116000000, 124000000, 116000000, 13},
    {124000000, 130000000, 124000000, 13},
};

static bool s_txActive;
static bool s_txTotLocked;
static uint32_t s_txStartMs;
static bool s_txSavedAnt;

static bool txCalSelect(uint32_t f, uint8_t *band, uint8_t *point) {
  for (uint8_t i = 0; i < (uint8_t)(sizeof(TX_CAL_WINDOWS) /
                                    sizeof(TX_CAL_WINDOWS[0]));
       ++i) {
    const TxCalWindow *w = &TX_CAL_WINDOWS[i];
    if (f < w->lo || f > w->hi)
      continue;
    uint32_t p = (f - w->origin) / 500000u;
    *band = w->band;
    *point = (uint8_t)(p > 15u ? 15u : p);
    return true;
  }
  return false;
}

static bool txReadCal(uint32_t f, TxCal *out) {
  if (!W25Q_Present())
    return false;

  uint8_t general[TX_CAL_GENERAL_LEN];
  uint32_t base = 0;
  W25Q_Read(base, general, sizeof(general));
  if (general[0] != TX_CAL_MARKER) {
    base = TX_CAL_BACKUP;
    W25Q_Read(base, general, sizeof(general));
    if (general[0] != TX_CAL_MARKER)
      return false;
  }

  uint8_t band;
  uint8_t point;
  if (!txCalSelect(f, &band, &point))
    return false;

  uint8_t rec[TX_CAL_BAND_LEN];
  W25Q_Read(base + TX_CAL_GENERAL_LEN + (uint32_t)band * TX_CAL_BAND_LEN,
            rec, sizeof(rec));
  out->tx3k = general[9];
  out->devWide = (uint16_t)(rec[3] | ((uint16_t)rec[4] << 8));
  out->devNarrow = (uint16_t)(rec[10] | ((uint16_t)rec[11] << 8));
  out->high = rec[16 + point];
  out->low = rec[32 + point];
  return true;
}

static bool txRoute(uint32_t f, uint8_t *route, bool *pf0High) {
  uint8_t r;
  if (f >= 1800000u && f <= 6400000u)
    r = (f < 3200000u) ? 1 : 3;
  else if (f > 6400000u && f < 26000000u)
    r = 3;
  else if (f >= 26000000u && f <= 66000000u)
    r = 4;
  else if (f >= 84000000u && f <= 130000000u)
    r = 5;
  else
    return false;

  *route = r;
  *pf0High = (r != 3 && r != 4);
  return true;
}

static uint8_t txPowerCode(int32_t mode, uint8_t high, uint8_t low) {
  if (mode >= 2)
    return high;
  if (mode == 1)
    return (uint8_t)(((uint16_t)high + low) / 2u);
  return low;
}

static uint16_t txPowerRegister(uint8_t code) {
  return (uint16_t)(((uint16_t)(code & 0x3Fu) << 10) | 0x007Fu |
                    (code ? 0x0080u : 0u));
}

static uint16_t txFilterWord(uint8_t index) {
  switch (index) {
  case 0:
    return 0xE61C;
  case 1:
    return 0xDF22;
  case 2:
    return 0xD42D;
  case 3:
    return 0xCC35;
  case 4:
    return 0xFA02;
  default:
    return 0xF50B;
  }
}

/* K200 — механическое реле: не дёргаем чаще 300 мс. */
static void txApplyAntenna(bool high) {
  if (ant_is_main() == high)
    return;
  uint32_t now = millis();
  uint32_t last = ant_last_switch_ms();
  if (last && (uint32_t)(now - last) < TX_RELAY_MIN_MS)
    delay_ms((uint32_t)(TX_RELAY_MIN_MS - (now - last)));
  ant_sw(high);
  delay_ms(5);
}

bool Radio_TxActive(void) { return s_txActive; }

void Radio_TxPttReleased(void) { s_txTotLocked = false; }

void Radio_TxUpdate(void) {
  if (!s_txActive)
    return;
  uint32_t timeout = Settings_TxTimeoutMs();
  if (timeout && (uint32_t)(millis() - s_txStartMs) >= timeout) {
    Radio_TxStop();
    s_txTotLocked = true;
  }
}

bool Radio_TxStart(void) {
  if (s_txActive || s_txTotLocked)
    return false;
  if (!Settings_Get(SET_TX_ENABLE) || Settings_Get(SET_PTT_LOCK))
    return false;

  VFO *v = &vfos[currentVfo];
  if (v->radio != RADIO_BK4819A || v->modulation != MOD_FM)
    return false;
  /* Upconverter сдвигает только приём: на передачу это вышло бы не туда. */
  if (Settings_Get(SET_UPCONVERTER) != 0)
    return false;

  uint8_t route;
  bool pf0High;
  if (!txRoute(v->rxF, &route, &pf0High))
    return false;

  TxCal cal;
  if (!txReadCal(v->rxF, &cal))
    return false;

  /* Как stock 04F0C: код усекается до 6 бит, а не отвергается. */
  uint8_t code =
      (uint8_t)(txPowerCode(Settings_Get(SET_TXPOWER), cal.high, cal.low) &
                0x3Fu);
  if (code == 0)
    return false;

  bool narrow = v->bw <= BK4819_FILTER_BW_10k;

  if (Settings_Get(SET_BUSYCHANNELTXLOCK)) {
    BK4819_SelectChip(0);
    if (squelchLevel == 0 || BK4819_IsSquelchOpen())
      return false;
  }

  s_txSavedAnt = ant_is_main();
  s_txActive = true;

  BK4819_SelectChip(0);
  gpio_pin_write(&PIN_AF_MUTE, false);
  board_tx_gates(false, false);
  BK4819_SetGpioOutState((uint16_t)(BK4819_GetGpioOutState() & 0xFFF3u));
  BK4819_WriteRegister(0x36, 0x007F);
  BK4819_WriteRegister(0x30, 0x0000);
  BK4819_WriteRegister(0x47, 0x6040);
  BK4819_WriteRegister(0x43, narrow ? 0x4048 : 0x3028);
  BK4819_SetGpioOutState((uint16_t)(BK4819_GetGpioOutState() & 0xFFCCu));
  BK4819_WriteRegister(0x47, 0x6140);

  uint32_t hw = hwFreq(v->rxF, true);
  BK4819_WriteRegister(0x38, (uint16_t)(hw & 0xFFFFu));
  BK4819_WriteRegister(0x39, (uint16_t)(hw >> 16));
  BK4819_InvalidateFrequencyCache();

  txApplyAntenna(pf0High);

  BK4819_WriteRegister(0x51, 0x0000);
  BK4819_WriteRegister(0x71, 0x68DC); /* option 0 */
  BK4819_WriteRegister(0x31,
                       (uint16_t)(BK4819_ReadRegister(0x31) & 0xFFFDu));
  BK4819_WriteRegister(0x74, txFilterWord(cal.tx3k));
  BK4819_WriteRegister(
      0x7D, (uint16_t)(0xE940u + (uint16_t)Settings_Get(SET_MIC)));
  BK4819_WriteRegister(0x40, narrow ? cal.devNarrow : cal.devWide);
  BK4819_WriteRegister(0x37, 0x1D0F);
  delay_ms(10);
  BK4819_WriteRegister(0x52, 0x028F);
  BK4819_WriteRegister(0x30, 0x0200);
  BK4819_WriteRegister(0x30, 0xC1FE); /* FM TX DSP + MIC ADC */

  board_tx_gates(false, false);
  uint16_t gpio = (uint16_t)(BK4819_GetGpioOutState() & 0xFFF3u);
  if (route == 1)
    board_tx_gates(true, false);
  else if (route == 2)
    board_tx_gates(true, true);
  else if (route == 3)
    gpio |= 0x0008u; /* V-TXPOWER-EN */
  else
    gpio |= 0x0004u; /* U-TXPOWER-EN */
  BK4819_SetGpioOutState(gpio);

  BK4819_WriteRegister(0x36, txPowerRegister(code));
  s_txStartMs = millis();
  return true;
}

void Radio_TxStop(void) {
  if (!s_txActive)
    return;
  s_txActive = false;

  BK4819_SelectChip(0);
  gpio_pin_write(&PIN_AF_MUTE, false);
  board_tx_gates(false, false);
  BK4819_SetGpioOutState(0);
  BK4819_WriteRegister(0x36, 0x007F);
  BK4819_WriteRegister(0x30, 0x0000);
  BK4819_WriteRegister(0x47, 0x6040);
  BK4819_InvalidateFrequencyCache();
  BK4819_InvalidateFilterCache();

  txApplyAntenna(s_txSavedAnt);

  /* applyVfo() заново включит DAC/шумодав; синхронизируем учёт железа. */
  bkDacOn[0] = bkDacOn[1] = false;
  afMuteOn = false;
  applyVfo(true);
  /* applyVfo останавливается на RX-частоте/фильтре, но REG_30 после TX
   * равен 0 — включаем RX-тракт и заново калибруем VCO. */
  BK4819_RxEnable();
  BK4819_SetDeviation((uint16_t)Settings_Get(SET_DEVIATION) * 10u);
  bkDacOn[0] = bkDacOn[1] = false;
  afMuteOn = false;
  applyMute((squelchLevel == 0) || BK4819_IsSquelchOpen());
}

/* ── Public API ─────────────────────────────────────────────── */

void Radio_Init(void) {
  /* Идемпотентно: повторные входы в режимы не сбрасывают VFO
     и не переинициализируют кристаллы */
  static bool inited;
  if (inited)
    return;
  inited = true;

  memset(vfos, 0, sizeof(vfos));
  memset(vfoChannels, 0, sizeof(vfoChannels));
  memset(vfoSaved, 0, sizeof(vfoSaved));

  /* Дефолты без SI на старте: SSB-патч грузится лениво (один раз за
     включение) при первом переходе на КВ, чтобы старт был мгновенным */
  vfos[0].rxF = 14550000; /* 145.5 МГц FM → BK A */
  vfos[0].step = STEP_25_0kHz;
  vfos[0].modulation = MOD_FM;
  vfos[1].rxF = 14200000; /* 14.2 МГц USB → SI4732 (лениво) */
  vfos[1].step = STEP_1_0kHz;
  vfos[1].modulation = MOD_USB;
  vfos[2].rxF = 10480000; /* 104.8 МГц WFM → BK A */
  vfos[2].step = STEP_100_0kHz;
  vfos[2].modulation = MOD_WFM;
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    vfos[i].bw = BK4819_FILTER_BW_12k;
    vfos[i].radio = Radio_PickDevice(vfos[i].rxF, vfos[i].modulation);
  }

  for (uint8_t c = 0; c < 2; c++) {
    BK4819_SelectChip(c);
    BK4819_Init(); /* сброс чипа в известное состояние */
    BK4819_SetDeviation((uint16_t)Settings_Get(SET_DEVIATION) * 10u);
    if (c == 1) {
      bkSleepB(); /* B: не принимает (нет антенны), передачи пока нет — спит */
      continue;
    }
    BK4819_RX_TurnOn();
    BK4819_SetAFC(0);
    BK4819_SetAGC(true, 1);
    /* RX_TurnOn включает AF DAC — выключаем, учёт bkDacOn = false верен.
       Иначе на старте шипит до первого опроса шумодава. */
    BK4819_ToggleAFDAC(false);
  }
  BK4819_SelectChip(0);

  /* RST и бит-банг I2C SI4732 — без этого чип в ресете, IsAlive = false */
  SI47XX_Init();

  currentVfo = 0;
  s_pendingApply = false;
  siMuted = true; /* SI ещё не включён: при включении включим громкость */
  bkDacOn[0] = bkDacOn[1] = false;
  s_bootMute = true;
  Store_ApplyRadio(); /* последние VFO/шумодав/фильтр/диапазоны */
  VFO_ApplySavedChannelModes();
  applyVfo(true);
  s_bootMute = false;
}

/* Каждый update-цикл: шумодав активного VFO + мониторинг. */
void Radio_Update(void) {
  Radio_TxUpdate();
  if (s_txActive)
    return;
  monitorReturnTick();

  Radio ar = vfos[currentVfo].radio;
  bool sqOpen = true;
  if (ar != RADIO_SI4732) {
    BK4819_SelectChip(vfoChip(currentVfo));
    sqOpen = (squelchLevel == 0) || BK4819_IsSquelchOpen();
  }
  applyMute(sqOpen);

  monitorTick();

  /* monitorTick мог переключить кристаллы — вернём контекст активного
   * и обновим субтоны/DTMF уже по фактическому приёмнику. */
  Radio now = vfos[currentVfo].radio;
  if (now != RADIO_SI4732) {
    uint8_t chip = vfoChip(currentVfo);
    BK4819_SelectChip(chip);
    sqOpen = (squelchLevel == 0) || BK4819_IsSquelchOpen();
    Tone_Update(true, chip, vfos[currentVfo].rxF, sqOpen);
  } else {
    Tone_Update(false, 0, vfos[currentVfo].rxF, sqOpen);
  }
}

/* Звук сканера. Железо задаём ЯВНО и приводим в соответствие учёт
   (bkDacOn/afMuteOn), а не сравниваем «хочу» с учётом: после манипуляций
   в VFO (смена модуляции, SI↔BK, VFO) учёт расходился с реальным DAC/AF_MUTE
   — сканер шипел при «закрытом» звуке, а после выхода шумодав не закрывался.
   on=true — слышно активный BK (чип активного VFO); on=false — тишина везде. */
void Radio_ScanAudio(bool on) {
  uint8_t chip = vfoChip(currentVfo);
  for (uint8_t c = 0; c < BK_RX_CHIPS; c++) {
    bool want = on && c == chip;
    BK4819_SelectChip(c);
    BK4819_ToggleAFDAC(want);
    bkDacOn[c] = want;
  }
  BK4819_SelectChip(chip);
  if (!siMuted) { /* SI во время скана молчит */
    SI47XX_SetVolume(0);
    siMuted = true;
  }
  gpio_pin_write(&PIN_AF_MUTE, on);
  afMuteOn = on;
  led_rx_set(on);
}

/* Звук SI4732 для режимов, которые сами управляют прослушиванием (BC-сканер).
 * Шумодава у SI нет: on=true — громкость 63 и AF_MUTE открыт, on=false —
 * громкость 0. Учёт siMuted/afMuteOn общий с applyMute, иначе VFO после
 * выхода видел бы рассинхрон и оставлял SI замьюченным. */
void Radio_SiAudio(bool on) {
  if (vfos[currentVfo].radio != RADIO_SI4732)
    return;
  SI47XX_SetVolume(on ? 63 : 0);
  siMuted = !on;
  gpio_pin_write(&PIN_AF_MUTE, on);
  afMuteOn = on;
  led_rx_set(on);
}

void Radio_ToggleSquelchForce(void) {
  static uint8_t saved = 4;
  static bool forced;
  if (forced) {
    forced = false;
    Radio_SetSquelch(saved ? saved : 4);
  } else {
    saved = squelchLevel;
    forced = true;
    Radio_SetSquelch(0); /* 0 = всегда открыт */
  }
}

void Radio_SetSquelch(uint8_t level) {
  squelchLevel = level;
  for (uint8_t c = 0; c < BK_RX_CHIPS; c++) {
    BK4819_SelectChip(c);
    BK4819_SetupSquelch(GetSql(level),
                       (uint8_t)Settings_Get(SET_SQLOPENTIME),
                       (uint8_t)Settings_Get(SET_SQLCLOSETIME));
  }
}

void Radio_TuneTo(uint32_t f, bool precise) {
  vfos[currentVfo].rxF = f;
  applyVfo(precise);
}

/* Шумодав диапазона применяется при входе в него; BAND_SQL_NONE — оставить
 * текущий (заводские записи так и сделаны). */
static void applyBandSquelch(const BandPlan *b) {
  if (b && b->squelch <= 10)
    Radio_SetSquelch(b->squelch);
}

void Radio_TuneToAuto(uint32_t f) {
  const BandPlan *b = Band_Find(f);
  if (!b)
    vfos[currentVfo].modulation = MOD_FM;
  else {
    vfos[currentVfo].modulation = b->mod;
    vfos[currentVfo].step = static_cast<Step>(b->step);
    applyBandSquelch(b);
  }
  vfos[currentVfo].rxF = f;
  applyVfo(true);
}

void Radio_TuneStep(int8_t dir) {
  VFO *v = &vfos[currentVfo];
  int64_t step = StepFrequencyTable[v->step];
  int64_t nf = (int64_t)v->rxF + step * dir;
  int8_t ai = Band_Index(v->rxF);

  /* внутри диапазона шаг зацикливается: вверх за край — на начало,
   * вниз за начало — на последнюю точку сетки у края */
  if (ai >= 0) {
    const BandPlan *a = &Band_At((uint8_t)ai);
    if (a->loop) {
      int64_t lo = (int64_t)a->startKhz * 100, hi = (int64_t)a->endKhz * 100;
      if (nf >= hi)
        nf = lo;
      else if (nf < lo)
        nf = lo + ((hi - 1 - lo) / step) * step;
      Radio_TuneTo((uint32_t)nf, false);
      return;
    }
  }

  /* вне «петли» (общие диапазоны): вошли в другой — берём его модуляцию,
   * шаг и шумодав. Band_At отдаёт общий снимок, поэтому сравниваем
   * индексы, а не указатели. */
  if (nf < 0)
    nf = 0;
  const BandPlan *b = Band_Find((uint32_t)nf);
  int8_t bi = b ? Band_Index((uint32_t)nf) : -1;
  bool changed = b && ai != bi;
  if (changed) {
    v->modulation = b->mod;
    v->step = static_cast<Step>(b->step);
    applyBandSquelch(b);
  }
  Radio_TuneTo((uint32_t)nf, changed); /* смена модуляции — только precise */
}

/* Группа, в которой сейчас VFO: по диапазону, а вне диапазонов — по
 * модуляции (SSB — любительские, AM/WFM — вещание, FM — службы). */
static uint8_t currentCat(const VFO *v) {
  int8_t i = Band_Index(v->rxF);
  if (i >= 0 && Band_At(i).cat != CAT_NONE)
    return Band_At(i).cat;
  if (v->modulation == MOD_LSB || v->modulation == MOD_USB)
    return CAT_HAM;
  if (v->modulation == MOD_AM || v->modulation == MOD_WFM)
    return CAT_BC;
  return CAT_SVC;
}

static void gotoBand(int8_t t) {
  uint32_t f = Band_Mem(t) ? Band_Mem(t) : Band_At(t).startKhz * 100;
  Band_GroupLastSet(Band_At(t).cat, t);
  Radio_TuneToAuto(f); /* модуляция и шаг диапазона */
}

/* Выбор диапазона: следующий/предыдущий по частоте В СВОЕЙ ГРУППЕ
 * (вещание — вещание, SSB — SSB-диапазоны). Частота возвращается к
 * последней, на которой были в этом диапазоне. */
void Radio_NextBand(int8_t dir) {
  VFO *v = &vfos[currentVfo];
  int8_t cur = Band_Index(v->rxF);
  if (cur >= 0)
    Band_MemSet(cur, v->rxF);
  uint8_t cat = currentCat(v);

  uint32_t key = (cur >= 0 && Band_At(cur).loop && Band_At(cur).cat == cat)
                     ? Band_At(cur).startKhz
                     : v->rxF / 100;
  int8_t best = -1, edge = -1; /* ближайший по направлению / крайний (оборот) */
  for (uint8_t i = 0; i < Band_Count(); i++) {
    if (!Band_At(i).loop || Band_At(i).cat != cat)
      continue;
    uint32_t s = Band_At(i).startKhz;
    if (dir > 0) {
      if (s > key && (best < 0 || s < Band_At(best).startKhz))
        best = i;
      if (edge < 0 || s < Band_At(edge).startKhz)
        edge = i;
    } else {
      if (s < key && (best < 0 || s > Band_At(best).startKhz))
        best = i;
      if (edge < 0 || s > Band_At(edge).startKhz)
        edge = i;
    }
  }
  int8_t t = best >= 0 ? best : edge;
  if (t >= 0)
    gotoBand(t);
}

/* Следующая группа (вещание → любительские → службы → ...): на последний
 * диапазон, где были в этой группе, или на её самый низкий. */
void Radio_NextGroup(void) {
  VFO *v = &vfos[currentVfo];
  int8_t cur = Band_Index(v->rxF);
  if (cur >= 0)
    Band_MemSet(cur, v->rxF);
  uint8_t ng = (currentCat(v) + 1) % CAT_COUNT;

  int8_t t = Band_GroupLast(ng);
  if (t < 0) {
    for (uint8_t i = 0; i < Band_Count(); i++) {
      /* Band_At отдаёт общий снимок: сначала копируем поля, потом
       * сравниваем с Band_At(t) — иначе два вызова в одном выражении
       * затирают друг друга. */
      const BandPlan &bp = Band_At(i);
      bool loop = bp.loop;
      uint8_t cat = bp.cat;
      uint32_t s = bp.startKhz;
      if (loop && cat == ng &&
          (t < 0 || s < (uint32_t)Band_At((uint8_t)t).startKhz))
        t = i;
    }
  }
  if (t >= 0)
    gotoBand(t);
}

void Radio_NextVfo(void) { Radio_SwitchToVfo(currentVfo + 1); }

void Radio_SwitchToVfo(uint8_t idx) {
  currentVfo = idx % DEVICE_VFO_COUNT;
  /* Тяжёлое применение (инициализация кристалла, SSB-патч SI4732)
     откладываем — экран успеет перерисовать новый активный ряд,
     радио догоняет через Radio_ApplyPending. */
  s_pendingApply = true;
}

/* Вызывать ПОСЛЕ отрисовки (из render режима): выполняет отложенное
   применение VFO, не блокируя перерисовку. */
void Radio_ApplyPending(void) {
  if (s_txActive)
    return; /* не перестраивать RX посреди передачи */
  if (s_pendingApply) {
    s_pendingApply = false;
    applyVfo(true);
  }
}

void Radio_ToggleMonitor(uint8_t idx) {
  /* Текущий VFO тоже можно закрепить: он просто не опрашивается, пока
   * активен (monitorTick пропускает currentVfo), и начнёт мониториться,
   * когда пользователь переключится на другой. */
  vfos[idx].monitor = !vfos[idx].monitor;
}

const VFOChannel *VFO_GetChannel(uint8_t idx) {
  if (idx >= DEVICE_VFO_COUNT ||
      !(vfoChannels[idx].flags & VFO_CH_VALID))
    return nullptr;
  return &vfoChannels[idx];
}

bool VFO_HasChannel(uint8_t idx) { return VFO_GetChannel(idx) != nullptr; }

bool VFO_ChannelMode(uint8_t idx) {
  return idx < DEVICE_VFO_COUNT &&
         (vfoChannels[idx].flags & VFO_CH_MODE);
}

/* Только настройка тракта: monitor/gain принадлежат VFO, а не каналу. */
static void copyTuning(VFO *v, const VFO *s) {
  v->rxF = s->rxF;
  v->step = s->step;
  v->modulation = s->modulation;
  v->bw = s->bw;
  v->radio = s->radio;
  v->radioManual = s->radioManual;
}

bool VFO_SetChannelMode(uint8_t idx, bool on, bool retune) {
  if (idx >= DEVICE_VFO_COUNT)
    return false;
  if (on == VFO_ChannelMode(idx))
    return true;
  if (on) {
    const VFOChannel *c = VFO_GetChannel(idx);
    if (!c)
      return false;
    vfoSaved[idx] = vfos[idx];
    vfoChannels[idx].flags |= VFO_CH_MODE;
    applyChannelFields(&vfos[idx], c);
  } else {
    copyTuning(&vfos[idx], &vfoSaved[idx]);
    vfoChannels[idx].flags &= (uint8_t)~VFO_CH_MODE;
  }
  if (retune && idx == currentVfo)
    applyVfo(true);
  return true;
}

bool VFO_ToggleChannelMode(uint8_t idx) {
  return VFO_SetChannelMode(idx, !VFO_ChannelMode(idx));
}

void VFO_ApplySavedChannelModes(void) {
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; ++i) {
    if (!(vfoChannels[i].flags & VFO_CH_MODE))
      continue;
    const VFOChannel *c = VFO_GetChannel(i);
    if (!c) {
      vfoChannels[i].flags &= (uint8_t)~VFO_CH_MODE;
      continue;
    }
    vfoSaved[i] = vfos[i];
    applyChannelFields(&vfos[i], c);
  }
}

const VFO *VFO_FrequencyState(uint8_t idx) {
  if (idx >= DEVICE_VFO_COUNT)
    return nullptr;
  return VFO_ChannelMode(idx) ? &vfoSaved[idx] : &vfos[idx];
}

void VFO_AssignChannel(uint8_t idx, uint32_t f, uint8_t mod, uint8_t bw,
                       uint8_t radio, const char *name) {
  if (idx >= DEVICE_VFO_COUNT)
    return;
  VFOChannel *c = &vfoChannels[idx];
  bool chMode = (c->flags & VFO_CH_MODE) != 0;
  memset(c, 0, sizeof(*c));
  c->f = f;
  c->modulation = mod & 7;
  c->bw = bw;
  if (c->bw > 9)
    c->bw = 0;
  c->radio = (uint8_t)channelRadio(radio);
  c->flags = VFO_CH_VALID;
  if (name) {
    for (uint8_t i = 0; i < VFO_CH_NAME_LEN - 1 && name[i]; ++i) {
      uint8_t ch = (uint8_t)name[i];
      c->name[i] = (ch < 32 || ch > 126 || ch == '#') ? ' ' : (char)ch;
    }
  }
  if (chMode) {
    c->flags |= VFO_CH_MODE;
    applyChannelFields(&vfos[idx], c);
    if (idx == currentVfo)
      applyVfo(true);
  }
}

void VFO_ClearChannel(uint8_t idx) {
  if (idx >= DEVICE_VFO_COUNT)
    return;
  if (VFO_ChannelMode(idx))
    VFO_SetChannelMode(idx, false);
  memset(&vfoChannels[idx], 0, sizeof(vfoChannels[idx]));
}

/* Скопировать частоту/параметры канала в собственное состояние VFO. */
bool VFO_CopyChannelToVfo(uint8_t idx) {
  const VFOChannel *c = VFO_GetChannel(idx);
  if (!c)
    return false;
  if (VFO_ChannelMode(idx))
    VFO_SetChannelMode(idx, false, false);
  applyChannelFields(&vfos[idx], c);
  if (idx == currentVfo)
    applyVfo(true);
  return true;
}

void Radio_NextStep(void) {
  VFO *v = &vfos[currentVfo];
  v->step = static_cast<Step>((v->step + 1) % STEP_COUNT);
}

/* AUTO → HF → VHF → UHF → 800 → AUTO. В AUTO фильтр выбирается по частоте
 * (BK4819_SelectFilterByFrequency), в ручном — держится до следующего шага. */
void Radio_NextFilter(void) {
  filterIndex = (filterIndex >= 3) ? -1 : filterIndex + 1;
  if (filterIndex < 0) {
    BK4819_SetFilterOverride(filters[0], false);
    BK4819_SelectFilterByFrequency(vfos[currentVfo].rxF);
  } else {
    BK4819_SetFilterOverride(filters[filterIndex], true);
  }
}

/* Сканирующие режимы работают в AUTO: ручной фильтр из VFO мог остаться от
 * другого диапазона и «погасить» весь спектр. */
void Radio_FilterAuto(void) {
  filterIndex = -1;
  BK4819_SetFilterOverride(filters[0], false);
  BK4819_SelectFilterByFrequency(vfos[currentVfo].rxF);
}
