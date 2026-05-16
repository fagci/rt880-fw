#include "radio.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/si473x.h"
#include "helper/log.h"
#include "ui/graphics.h"

extern SI47XX_MODE si4732mode;

VFO vfos[DEVICE_VFO_COUNT] = {
    {.radio = RADIO_BK4819A,
     .rxF = 17230000,
     .step = STEP_25_0kHz,
     .modulation = MOD_FM,
     .bw = BK4819_FILTER_BW_12k},
    {.radio = RADIO_BK4819A,
     .rxF = 43400000,
     .step = STEP_25_0kHz,
     .modulation = MOD_FM,
     .bw = BK4819_FILTER_BW_12k},
    {.radio = RADIO_SI4732,
     .rxF = 10480000,
     .step = STEP_100_0kHz,
     .modulation = MOD_WFM,
     .bw = BK4819_FILTER_BW_12k},
};

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
    "20Hz",  "50Hz",   "500Hz", "1k",   "2.5k",
    "5k",    "6.25k",  "8.33k", "9k",   "10k",
    "12.5k", "25k",    "50k",   "100k", "500k",
};

uint8_t squelchLevel = 4;

/* Состояние AFDAC и SI4732 — пишем только при изменении */
static bool bk0DacOn  = false;
static bool siUnmuted = false;

/* ── Band plan ──────────────────────────────────────────────── */

#define F_KHZ(x) ((uint32_t)(x) * 100U)
#define F_MHZ(x) ((uint32_t)(x) * 100000U)

typedef struct {
  uint32_t lo;
  uint32_t hi;
  ModulationType mod;
} BandEntry_t;

static const BandEntry_t BAND_PLAN[] = {
    {F_KHZ(148),  F_KHZ(283),  MOD_AM },  // LW
    {F_KHZ(530),  F_KHZ(1700), MOD_AM },  // MW
    {F_KHZ(1800), F_KHZ(2000), MOD_LSB},  // HAM 160m
    {F_KHZ(2300), F_KHZ(2495), MOD_AM },  // SW 120m
    {F_KHZ(3200), F_KHZ(3400), MOD_AM },  // SW 90m
    {F_KHZ(3500), F_KHZ(3800), MOD_LSB},  // HAM 80m
    {F_KHZ(3900), F_KHZ(4000), MOD_AM },  // SW 75m
    {F_KHZ(4750), F_KHZ(5060), MOD_AM },  // SW 60m
    {F_KHZ(5900), F_KHZ(6200), MOD_AM },  // SW 49m
    {F_KHZ(7000), F_KHZ(7200), MOD_LSB},  // HAM 40m
    {F_KHZ(7200), F_KHZ(7450), MOD_AM },  // SW 41m
    {F_KHZ(9400), F_KHZ(9900), MOD_AM },  // SW 31m
    {F_KHZ(11600),F_KHZ(12100),MOD_AM },  // SW 25m
    {F_KHZ(13570),F_KHZ(13870),MOD_AM },  // SW 22m
    {F_KHZ(14000),F_KHZ(14350),MOD_USB},  // HAM 20m
    {F_KHZ(15100),F_KHZ(15800),MOD_AM },  // SW 19m
    {F_KHZ(17480),F_KHZ(17900),MOD_AM },  // SW 16m
    {F_KHZ(18068),F_KHZ(18168),MOD_USB},  // HAM 17m
    {F_KHZ(18900),F_KHZ(19020),MOD_AM },  // SW 15m вещ.
    {F_KHZ(21000),F_KHZ(21450),MOD_USB},  // HAM 15m
    {F_KHZ(21450),F_KHZ(21850),MOD_AM },  // SW 13m
    {F_KHZ(24890),F_KHZ(24990),MOD_USB},  // HAM 12m
    {F_KHZ(25600),F_KHZ(26100),MOD_AM },  // SW 11m
    {F_KHZ(28000),F_KHZ(29700),MOD_USB},  // HAM 10m
    {F_MHZ(76),   F_MHZ(108),  MOD_WFM},  // FM вещалки
    {F_MHZ(144),  F_MHZ(148),  MOD_FM },  // HAM 2m
    {F_MHZ(430),  F_MHZ(440),  MOD_FM },  // HAM 70cm
};

ModulationType Radio_AutoModulation(uint32_t f) {
  for (uint8_t i = 0; i < sizeof(BAND_PLAN) / sizeof(BAND_PLAN[0]); i++) {
    if (f >= BAND_PLAN[i].lo && f < BAND_PLAN[i].hi)
      return BAND_PLAN[i].mod;
  }
  if (f < F_MHZ(30))  return MOD_AM;
  if (f < F_MHZ(76))  return MOD_FM;
  if (f < F_MHZ(108)) return MOD_WFM;
  return MOD_FM;
}

/* ── Helpers ────────────────────────────────────────────────── */

static SI47XX_MODE modToSiMode(ModulationType mod) {
  switch (mod) {
  case MOD_USB: return SI47XX_USB;
  case MOD_LSB: return SI47XX_LSB;
  case MOD_FM:
  case MOD_WFM: return SI47XX_FM;
  default:      return SI47XX_AM;
  }
}

/* Применить mute-состояние. Пишет в железо только при изменении.
   PIN_AF_MUTE выставляется один раз в Radio_Init и больше не трогается —
   дёрганье этой линии каждый кадр даёт треск. */
static void applyMute(bool sqOpen) {
  bool wantBk0 = (vfos[currentVfo].radio == RADIO_BK4819A) && sqOpen;
  if (wantBk0 != bk0DacOn) {
    BK4819_SelectChip(0);
    BK4819_ToggleAFDAC(wantBk0);
    bk0DacOn = wantBk0;
  }

  /* chip 1 — TX only, пока не трогаем */

  bool wantSi = (vfos[currentVfo].radio == RADIO_SI4732);
  if (wantSi != siUnmuted) {
    SI47XX_SetVolume(wantSi ? 63 : 0);
    siUnmuted = wantSi;
  }

  /* восстанавливаем активный чип */
  if (vfos[currentVfo].radio == RADIO_BK4819A)
    BK4819_SelectChip(0);
}

/* precise=true: полное применение (смена VFO, ручной ввод).
   precise=false: только частота (сканирование) — пропускаем SetModulation
                  и applyMute, чтобы не тормозить. */
static void applyVfo(bool precise) {
  VFO *v = &vfos[currentVfo];
  uint32_t f = v->rxF;

  if (v->radio == RADIO_SI4732) {
    if (precise)
      SI47XX_SwitchMode(modToSiMode(v->modulation));
    SI47XX_TuneTo(f);
  } else {
    BK4819_SelectChip(0);
    BK4819_SelectFilterByFrequency(f);
    if (precise)
      BK4819_SetModulation(v->modulation);
    BK4819_TuneTo(f, precise);
  }

  if (precise)
    applyMute(true);
}

/* ── Public API ─────────────────────────────────────────────── */

void Radio_Init(void) {
  BK4819_SelectChip(0);
  BK4819_Init();
  BK4819_RX_TurnOn();
  BK4819_SetAFC(0);
  BK4819_SetAGC(true, 1);
  BK4819_SetFilterBandwidth(vfos[0].bw);
  BK4819_TuneTo(vfos[0].rxF, true);
  BK4819_SetModulation(vfos[0].modulation);
  BK4819_Squelch(squelchLevel, 1, 1);

  SI47XX_Init();
  si4732mode = modToSiMode(vfos[2].modulation);
  SI47XX_PowerUp();
  SI47XX_TuneTo(vfos[2].rxF);

  /* Открываем аппаратный мьют один раз и больше не трогаем */
  gpio_pin_set(&PIN_AF_MUTE);

  /* Начальное состояние: активен VFO 0 (BK4819A) */
  bk0DacOn  = false; // форсируем запись
  siUnmuted = true;  // форсируем запись SetVolume(0)
  applyMute(true);
}

/* Вызывается каждый update-цикл. Опрашивает шумодав и обновляет AFDAC
   только если состояние изменилось — SPI не пишется вхолостую. */
void Radio_Update(void) {
  if (vfos[currentVfo].radio == RADIO_SI4732)
    return;

  BK4819_SelectChip(0);
  bool sqOpen = (squelchLevel == 0) || BK4819_IsSquelchOpen();
  applyMute(sqOpen);
}

void Radio_SetSquelch(uint8_t level) {
  squelchLevel = level;
  BK4819_SelectChip(0);
  BK4819_Squelch(level, 1, 1);
}

void Radio_TuneTo(uint32_t f, bool precise) {
  vfos[currentVfo].rxF = f;
  applyVfo(precise);
}

void Radio_TuneToAuto(uint32_t f) {
  vfos[currentVfo].modulation = Radio_AutoModulation(f);
  vfos[currentVfo].rxF = f;
  applyVfo(true);
}

void Radio_TuneStep(int8_t dir) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  Radio_TuneTo(vfos[currentVfo].rxF + step * dir, false);
}

void Radio_NextVfo(void) {
  currentVfo = (currentVfo + 1) % DEVICE_VFO_COUNT;
  applyVfo(true);
}

void Radio_NextStep(void) {
  VFO *v = &vfos[currentVfo];
  v->step = (v->step + 1) % STEP_COUNT;
}

void Radio_PrevStep(void) {
  VFO *v = &vfos[currentVfo];
  v->step = (v->step + STEP_COUNT - 1) % STEP_COUNT;
}

void Radio_NextFilter(void) { filterIndex = (filterIndex + 1) % 4; }

void Radio_ToggleFilter(void) {
  BK4819_ToggleFilter(filters[filterIndex],
                      !(BK4819_GetFilter() & filters[filterIndex]));
}
