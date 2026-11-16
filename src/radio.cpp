#include "radio.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/si473x.h"
#include "helper/log.h"
#include "helper/measurements.h"
#include "ui/graphics.h"
#include <string.h>

VFO vfos[DEVICE_VFO_COUNT] = {{}, {}, {}};

uint8_t currentVfo = 0;

Filter filters[] = {FILTER_HF, FILTER_VHF, FILTER_UHF, FILTER_800};
const char *FILTER_NAMES[] = {"HF", "VHF", "UHF", "800"};
uint8_t filterIndex = 0;

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
#define MON_SETTLE_MS 8
static uint32_t s_lastMon;

/* ── План диапазонов: автонастройка модуляции и шага ───────────
 * При вводе частоты и при переходе через границу диапазона шагами
 * VFO получает модуляцию и шаг по умолчанию для диапазона. Первое
 * совпадение выигрывает — узкие диапазоны стоят раньше общих.
 * Границы в кГц: начало включительно, конец исключительно. */
/* Группы диапазонов: SIDE1/SIDE2 перебирают диапазоны своей группы.
 * BC — вещание (AM/WFM), HAM — любительские SSB, SVC — службы (CB, авиа,
 * 2 м/70 см, морской, PMR). Общие диапазоны вне групп. */
typedef enum { CAT_BC, CAT_HAM, CAT_SVC, CAT_COUNT, CAT_NONE = 0xFF } BandCat;

typedef struct {
  uint32_t startKhz;
  uint32_t endKhz;
  uint8_t mod;
  uint8_t step;
  const char *name;
  uint8_t cat;
  bool loop; /* шаг UP/DOWN зацикливается внутри диапазона; выбирается SIDE1/2 */
} BandPlan;

#define BC(a, b, m, st, nm) {a, b, m, st, nm, CAT_BC, true}
#define HAM(a, b, m, st, nm) {a, b, m, st, nm, CAT_HAM, true}
#define SVC(a, b, m, st, nm) {a, b, m, st, nm, CAT_SVC, true}

/* Первое совпадение выигрывает; общие диапазоны (loop=false) — в конце.
 * V/UHF: пока везде FM и шаг 25 кГц (авиа — AM). */
static const BandPlan PLAN[] = {
    /* вещание: ДВ/СВ и все КВ-вещательные */
    BC(153, 279, MOD_AM, STEP_9_0kHz, "LW"),
    BC(520, 1710, MOD_AM, STEP_9_0kHz, "MW"),
    BC(2300, 2495, MOD_AM, STEP_5_0kHz, "120m BC"),
    BC(3200, 3400, MOD_AM, STEP_5_0kHz, "90m BC"),
    BC(3900, 4000, MOD_AM, STEP_5_0kHz, "75m BC"),
    BC(4750, 5060, MOD_AM, STEP_5_0kHz, "60m BC"),
    BC(5900, 6200, MOD_AM, STEP_5_0kHz, "49m BC"),
    BC(7200, 7450, MOD_AM, STEP_5_0kHz, "41m BC"),
    BC(9400, 9900, MOD_AM, STEP_5_0kHz, "31m BC"),
    BC(11600, 12100, MOD_AM, STEP_5_0kHz, "25m BC"),
    BC(13570, 13870, MOD_AM, STEP_5_0kHz, "22m BC"),
    BC(15100, 15800, MOD_AM, STEP_5_0kHz, "19m BC"),
    BC(17480, 17900, MOD_AM, STEP_5_0kHz, "16m BC"),
    BC(18900, 19020, MOD_AM, STEP_5_0kHz, "15m BC"),
    BC(21450, 21850, MOD_AM, STEP_5_0kHz, "13m BC"),
    BC(25670, 26100, MOD_AM, STEP_5_0kHz, "11m BC"),
    BC(64000, 108000, MOD_WFM, STEP_100_0kHz, "FM BC"), /* на SI4732 */
    /* любительские КВ (SSB): ниже 10 МГц LSB, выше USB */
    HAM(1800, 2000, MOD_LSB, STEP_1_0kHz, "160m"),
    HAM(3500, 3800, MOD_LSB, STEP_1_0kHz, "80m"),
    HAM(5300, 5410, MOD_USB, STEP_1_0kHz, "60m"),
    HAM(7000, 7200, MOD_LSB, STEP_1_0kHz, "40m"),
    HAM(10100, 10150, MOD_USB, STEP_1_0kHz, "30m"),
    HAM(14000, 14350, MOD_USB, STEP_1_0kHz, "20m"),
    HAM(18068, 18168, MOD_USB, STEP_1_0kHz, "17m"),
    HAM(21000, 21450, MOD_USB, STEP_1_0kHz, "15m"),
    HAM(24890, 24990, MOD_USB, STEP_1_0kHz, "12m"),
    HAM(28000, 29700, MOD_USB, STEP_1_0kHz, "10m"),
    /* службы */
    SVC(26965, 27405, MOD_AM, STEP_10_0kHz, "CB"),
    SVC(118000, 136000, MOD_AM, STEP_25_0kHz, "AIR"),
    SVC(144000, 146000, MOD_FM, STEP_25_0kHz, "2m"),
    SVC(156000, 163000, MOD_FM, STEP_25_0kHz, "MARINE"),
    SVC(430000, 440000, MOD_FM, STEP_25_0kHz, "70cm"),
    SVC(446000, 446200, MOD_FM, STEP_25_0kHz, "PMR446"),
    /* общие: без зацикливания и без группы */
    {100, 30000, MOD_AM, STEP_5_0kHz, "HF", CAT_NONE, false},
    {30000, 1340000, MOD_FM, STEP_25_0kHz, "V/UHF", CAT_NONE, false},
};
#define PLAN_N (sizeof(PLAN) / sizeof(PLAN[0]))

/* последняя частота в каждом диапазоне — при возврате SIDE1/2 */
static uint32_t s_bandMem[PLAN_N];
/* последний диапазон, где были в группе — при переключении группы */
static int8_t s_groupLast[CAT_COUNT] = {-1, -1, -1};

static int8_t findPlanIdx(uint32_t f) {
  uint32_t khz = f / 100; /* 10 Гц → кГц */
  for (uint8_t i = 0; i < PLAN_N; i++)
    if (khz >= PLAN[i].startKhz && khz < PLAN[i].endKhz)
      return (int8_t)i;
  return -1;
}

static const BandPlan *findPlan(uint32_t f) {
  int8_t i = findPlanIdx(f);
  return i < 0 ? nullptr : &PLAN[i];
}

const char *Radio_BandName(uint32_t f) {
  const BandPlan *b = findPlan(f);
  return b ? b->name : "";
}

/* Выставить модуляцию и шаг активного VFO по плану. false — вне плана. */
bool Radio_ApplyBandPlan(uint32_t f) {
  const BandPlan *b = findPlan(f);
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

/* Разбудить/усыпить B на время теста (ALARM в сканере). */
void Radio_BkBWake(bool on) {
  BK4819_SelectChip(1);
  if (on) {
    BK4819_RX_TurnOn();
    BK4819_ToggleAFDAC(false); /* звук B не нужен — только RSSI */
  } else {
    bkSleepB();
  }
  BK4819_SelectChip(0); /* активный приёмник — всегда A */
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
  if (f < 1800000) /* ниже 18 МГц BK4819 не принимает */
    return RADIO_SI4732;
  if (f < 3000000 && (mod == MOD_AM || mod == MOD_LSB || mod == MOD_USB))
    return RADIO_SI4732;
  return RADIO_BK4819A; /* 18–30 МГц FM/прочее (CB 27 МГц) — тоже BK A */
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

/* precise=true: полное применение (смена VFO, ручной ввод).
   precise=false: только частота (сканирование, мониторинг). */
static void applyVfo(bool precise) {
  VFO *v = &vfos[currentVfo];
  v->radio = Radio_PickDevice(v->rxF, v->modulation);

  if (v->radio == RADIO_SI4732) {
    siEnsure(v); /* может откатить v->radio на BK */
    if (v->radio == RADIO_SI4732) {
      SI47XX_TuneTo(v->rxF); /* единицы 10 Гц — как и раньше */
      SI47XX_SetAutomaticGainControl(modToSi(v->modulation) == SI47XX_FM ? 0 : 1, 0);
    }
  }

  if (v->radio != RADIO_SI4732) {
    uint8_t chip = vfoChip(currentVfo);
    BK4819_SelectChip(chip);
    BK4819_SelectFilterByFrequency(v->rxF);
    BK4819_TuneTo(v->rxF, precise);
    if (precise) {
      /* WFM на BK — широкополосный FM (BW до 18k вместо 12k) */
      ModulationType m = (v->modulation == MOD_WFM) ? MOD_FM
                         : static_cast<ModulationType>(v->modulation);
      BK4819_SetModulation(m);
      BK4819_SetFilterBandwidth(
          static_cast<BK4819_FilterBandwidth_t>(v->bw));
      BK4819_SetupSquelch(GetSql(squelchLevel), 1, 1);
      audio_path_set(chip);
    }
  }

  if (precise)
    applyMute(!s_bootMute);
}

/* ── Мониторинг: один BK крутится по закреплённым VFO ──────────
 * Кандидаты — VFO с флагом monitor, НЕ активные, на BK и на кристалле,
 * отличном от активного (один кристалл = один частотный тракт).
 * По открывшемуся шумодаву монитор становится активным VFO. */
static void monitorTick(void) {
  uint32_t now = millis();
  if (now - s_lastMon < MON_DWELL_MS)
    return;
  s_lastMon = now;

  Radio ar = vfos[currentVfo].radio;
  uint8_t cand[DEVICE_VFO_COUNT];
  uint8_t n = 0;
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    if (i == currentVfo || !vfos[i].monitor)
      continue;
    vfos[i].radio = Radio_PickDevice(vfos[i].rxF, vfos[i].modulation);
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
  BK4819_TuneTo(m->rxF, false);
  BK4819_SetupSquelch(GetSql(squelchLevel), 1, 1);
  delay_ms(MON_SETTLE_MS);
  bool open = BK4819_IsSquelchOpen();
  if (bkDacOn[chip]) { /* монитор не должен звучать */
    BK4819_ToggleAFDAC(false);
    bkDacOn[chip] = false;
  }

  if (open) {
    Log("mon: vfo %u sq open", i);
    currentVfo = i;
    applyVfo(true);
  }
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
  applyVfo(true);
  s_bootMute = false;
}

/* Каждый update-цикл: шумодав активного VFO + мониторинг. */
void Radio_Update(void) {
  Radio ar = vfos[currentVfo].radio;
  bool sqOpen = true;
  if (ar != RADIO_SI4732) {
    BK4819_SelectChip(vfoChip(currentVfo));
    sqOpen = (squelchLevel == 0) || BK4819_IsSquelchOpen();
  }
  applyMute(sqOpen);

  monitorTick();

  /* monitorTick мог переключить кристаллы — вернём контекст активного */
  if (vfos[currentVfo].radio != RADIO_SI4732)
    BK4819_SelectChip(vfoChip(currentVfo));
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

void Radio_SetSquelch(uint8_t level) {
  squelchLevel = level;
  for (uint8_t c = 0; c < BK_RX_CHIPS; c++) {
    BK4819_SelectChip(c);
    BK4819_SetupSquelch(GetSql(level), 1, 1);
  }
}

void Radio_TuneTo(uint32_t f, bool precise) {
  vfos[currentVfo].rxF = f;
  applyVfo(precise);
}

void Radio_TuneToAuto(uint32_t f) {
  if (!Radio_ApplyBandPlan(f))
    vfos[currentVfo].modulation = MOD_FM;
  vfos[currentVfo].rxF = f;
  applyVfo(true);
}

void Radio_TuneStep(int8_t dir) {
  VFO *v = &vfos[currentVfo];
  int64_t step = StepFrequencyTable[v->step];
  int64_t nf = (int64_t)v->rxF + step * dir;
  const BandPlan *a = findPlan(v->rxF);

  /* внутри диапазона шаг зацикливается: вверх за край — на начало,
   * вниз за начало — на последнюю точку сетки у края */
  if (a && a->loop) {
    int64_t lo = (int64_t)a->startKhz * 100, hi = (int64_t)a->endKhz * 100;
    if (nf >= hi)
      nf = lo;
    else if (nf < lo)
      nf = lo + ((hi - 1 - lo) / step) * step;
    Radio_TuneTo((uint32_t)nf, false);
    return;
  }

  /* вне «петли» (общие диапазоны): вошли в другой — берём его модуляцию и шаг */
  if (nf < 0)
    nf = 0;
  const BandPlan *b = findPlan((uint32_t)nf);
  bool changed = b && a != b;
  if (changed) {
    v->modulation = b->mod;
    v->step = static_cast<Step>(b->step);
  }
  Radio_TuneTo((uint32_t)nf, changed); /* смена модуляции — только precise */
}

/* Группа, в которой сейчас VFO: по диапазону, а вне диапазонов — по
 * модуляции (SSB — любительские, AM/WFM — вещание, FM — службы). */
static uint8_t currentCat(const VFO *v) {
  int8_t i = findPlanIdx(v->rxF);
  if (i >= 0 && PLAN[i].cat != CAT_NONE)
    return PLAN[i].cat;
  if (v->modulation == MOD_LSB || v->modulation == MOD_USB)
    return CAT_HAM;
  if (v->modulation == MOD_AM || v->modulation == MOD_WFM)
    return CAT_BC;
  return CAT_SVC;
}

static void gotoBand(int8_t t) {
  uint32_t f = s_bandMem[t] ? s_bandMem[t] : PLAN[t].startKhz * 100;
  s_groupLast[PLAN[t].cat] = t;
  Radio_TuneToAuto(f); /* модуляция и шаг диапазона */
}

/* Выбор диапазона: следующий/предыдущий по частоте В СВОЕЙ ГРУППЕ
 * (вещание — вещание, SSB — SSB-диапазоны). Частота возвращается к
 * последней, на которой были в этом диапазоне. */
void Radio_NextBand(int8_t dir) {
  VFO *v = &vfos[currentVfo];
  int8_t cur = findPlanIdx(v->rxF);
  if (cur >= 0)
    s_bandMem[cur] = v->rxF;
  uint8_t cat = currentCat(v);

  uint32_t key = (cur >= 0 && PLAN[cur].loop && PLAN[cur].cat == cat)
                     ? PLAN[cur].startKhz
                     : v->rxF / 100;
  int8_t best = -1, edge = -1; /* ближайший по направлению / крайний (оборот) */
  for (uint8_t i = 0; i < PLAN_N; i++) {
    if (!PLAN[i].loop || PLAN[i].cat != cat)
      continue;
    uint32_t s = PLAN[i].startKhz;
    if (dir > 0) {
      if (s > key && (best < 0 || s < PLAN[best].startKhz))
        best = i;
      if (edge < 0 || s < PLAN[edge].startKhz)
        edge = i;
    } else {
      if (s < key && (best < 0 || s > PLAN[best].startKhz))
        best = i;
      if (edge < 0 || s > PLAN[edge].startKhz)
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
  int8_t cur = findPlanIdx(v->rxF);
  if (cur >= 0)
    s_bandMem[cur] = v->rxF;
  uint8_t ng = (currentCat(v) + 1) % CAT_COUNT;

  int8_t t = s_groupLast[ng];
  if (t < 0) {
    for (uint8_t i = 0; i < PLAN_N; i++)
      if (PLAN[i].loop && PLAN[i].cat == ng &&
          (t < 0 || PLAN[i].startKhz < PLAN[t].startKhz))
        t = i;
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
  if (s_pendingApply) {
    s_pendingApply = false;
    applyVfo(true);
  }
}

void Radio_ToggleMonitor(uint8_t idx) {
  if (idx == currentVfo)
    return; /* активный не мониторится — он и так на своём приёмнике */
  vfos[idx].monitor = !vfos[idx].monitor;
  Log("mon: vfo %u %s", idx, vfos[idx].monitor ? "on" : "off");
}

void Radio_NextStep(void) {
  VFO *v = &vfos[currentVfo];
  v->step = static_cast<Step>((v->step + 1) % STEP_COUNT);
}

void Radio_PrevStep(void) {
  VFO *v = &vfos[currentVfo];
  v->step = static_cast<Step>((v->step + STEP_COUNT - 1) % STEP_COUNT);
}

void Radio_NextFilter(void) { filterIndex = (filterIndex + 1) % 4; }

void Radio_ToggleFilter(void) {
  BK4819_ToggleFilter(filters[filterIndex],
                      !(BK4819_GetFilter() & filters[filterIndex]));
}
