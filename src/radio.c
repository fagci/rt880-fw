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
    {.radio = RADIO_BK4819B,
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

const uint16_t StepFrequencyTable[15] = {
    2,   5,    50,   100,  250,  500,   625,   833,
    900, 1000, 1250, 2500, 5000, 10000, 50000,
};

static uint8_t squelchLevel = 10;
static bool siUnmuted = false;

/* ── Band plan ──────────────────────────────────────────────── */

/* Единица частоты везде — 10 Гц (как в rxF).
   KHZ = 100, MHZ = 100000 */
#define F_KHZ(x) ((uint32_t)(x) * 100U)
#define F_MHZ(x) ((uint32_t)(x) * 100000U)

typedef struct {
  uint32_t lo;
  uint32_t hi;
  ModulationType mod;
} BandEntry_t;

/* Таблица просматривается сверху вниз, возвращается первое совпадение.
   HAM-диапазоны идут раньше смежных вещательных. */
static const BandEntry_t BAND_PLAN[] = {
    /* LW вещалки */
    {F_KHZ(148), F_KHZ(283), MOD_AM},

    /* MW вещалки */
    {F_KHZ(530), F_KHZ(1700), MOD_AM},

    /* HAM 160m */
    {F_KHZ(1800), F_KHZ(2000), MOD_LSB},

    /* SW 120m / 90m / 75m вещалки */
    {F_KHZ(2300), F_KHZ(2495), MOD_AM},
    {F_KHZ(3200), F_KHZ(3400), MOD_AM},
    {F_KHZ(3900), F_KHZ(4000), MOD_AM},

    /* HAM 80m — до вещательного 90m нет пересечения */
    {F_KHZ(3500), F_KHZ(3800), MOD_LSB},

    /* SW 60m / 49m */
    {F_KHZ(4750), F_KHZ(5060), MOD_AM},
    {F_KHZ(5900), F_KHZ(6200), MOD_AM},

    /* HAM 40m */
    {F_KHZ(7000), F_KHZ(7200), MOD_LSB},

    /* SW 41m (7.2–7.45 МГц) */
    {F_KHZ(7200), F_KHZ(7450), MOD_AM},

    /* SW 31m / 25m / 22m */
    {F_KHZ(9400), F_KHZ(9900), MOD_AM},
    {F_KHZ(11600), F_KHZ(12100), MOD_AM},
    {F_KHZ(13570), F_KHZ(13870), MOD_AM},

    /* HAM 20m */
    {F_KHZ(14000), F_KHZ(14350), MOD_USB},

    /* SW 19m / 16m */
    {F_KHZ(15100), F_KHZ(15800), MOD_AM},
    {F_KHZ(17480), F_KHZ(17900), MOD_AM},

    /* HAM 17m */
    {F_KHZ(18068), F_KHZ(18168), MOD_USB},

    /* SW 15m */
    {F_KHZ(18900), F_KHZ(19020), MOD_AM},

    /* HAM 15m */
    {F_KHZ(21000), F_KHZ(21450), MOD_USB},

    /* SW 13m */
    {F_KHZ(21450), F_KHZ(21850), MOD_AM},

    /* HAM 12m */
    {F_KHZ(24890), F_KHZ(24990), MOD_USB},

    /* SW 11m */
    {F_KHZ(25600), F_KHZ(26100), MOD_AM},

    /* HAM 10m */
    {F_KHZ(28000), F_KHZ(29700), MOD_USB},

    /* FM вещалки */
    {F_MHZ(76), F_MHZ(108), MOD_WFM},

    /* HAM VHF (2m) */
    {F_MHZ(144), F_MHZ(148), MOD_FM},

    /* HAM UHF (70cm) */
    {F_MHZ(430), F_MHZ(440), MOD_FM},
};

/* Вернуть предпочтительную модуляцию для частоты f (в единицах 10 Гц).
   Если частота не попала ни в один диапазон — эвристика по полосе. */
ModulationType Radio_AutoModulation(uint32_t f) {
  for (uint8_t i = 0; i < sizeof(BAND_PLAN) / sizeof(BAND_PLAN[0]); i++) {
    if (f >= BAND_PLAN[i].lo && f < BAND_PLAN[i].hi)
      return BAND_PLAN[i].mod;
  }

  /* fallback по полосе */
  if (f < F_MHZ(30))
    return MOD_AM; // прочий HF
  if (f < F_MHZ(76))
    return MOD_FM; // VHF low (6m HAM и др.)
  if (f < F_MHZ(108))
    return MOD_WFM; // FM вещалки (перекрытие с таблицей)
  return MOD_FM;    // VHF/UHF
}

/* ── Helpers ────────────────────────────────────────────────── */

static SI47XX_MODE modToSiMode(ModulationType mod) {
  switch (mod) {
  case MOD_USB:
    return SI47XX_USB;
  case MOD_LSB:
    return SI47XX_LSB;
  case MOD_FM:
  case MOD_WFM:
    return SI47XX_FM;
  default:
    return SI47XX_AM;
  }
}

static uint8_t chipForVfo(uint8_t i) {
  return vfos[i].radio == RADIO_BK4819B ? 1 : 0;
}

static void applyMute(bool sqOpen) {
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    bool active = (i == currentVfo);
    if (vfos[i].radio == RADIO_SI4732) {
      bool want = active;
      if (want != siUnmuted) {
        SI47XX_SetVolume(want ? 63 : 0);
        siUnmuted = want;
      }
    } else {
      BK4819_SelectChip(chipForVfo(i));
      BK4819_ToggleAFDAC(active && sqOpen);
    }
  }
  if (vfos[currentVfo].radio != RADIO_SI4732)
    BK4819_SelectChip(chipForVfo(currentVfo));
}

static void applyVfo(bool precise) {
  VFO *v = &vfos[currentVfo];
  uint32_t f = v->rxF;

  if (v->radio == RADIO_SI4732) {
    SI47XX_SwitchMode(modToSiMode(v->modulation));
    SI47XX_TuneTo(f);
  } else {
    BK4819_SelectChip(chipForVfo(currentVfo));
    BK4819_SelectFilterByFrequency(f);
    BK4819_SetModulation(v->modulation); // синхронизируем модуляцию с VFO
    BK4819_TuneTo(f, precise);
  }

  applyMute(true);
}

static void initBkChip(uint8_t chip) {
  BK4819_SelectChip(chip);
  BK4819_Init();
  BK4819_RX_TurnOn();
  BK4819_SetAFC(0);
  BK4819_SetAGC(true, 1);
  BK4819_SetFilterBandwidth(vfos[chip].bw);
  BK4819_TuneTo(vfos[chip].rxF, true);
  BK4819_SetModulation(vfos[chip].modulation);
  BK4819_Squelch(squelchLevel, 1, 1);
}

/* ── Public API ─────────────────────────────────────────────── */

void Radio_Init(void) {
  initBkChip(0);
  initBkChip(1);

  SI47XX_Init();
  si4732mode = modToSiMode(vfos[2].modulation);
  SI47XX_PowerUp();
  SI47XX_TuneTo(vfos[2].rxF);

  siUnmuted = false;
  applyMute(true);

  gpio_pin_set(&PIN_AF_MUTE);
}

void Radio_Update(void) {
  if (vfos[currentVfo].radio == RADIO_SI4732)
    return;

  BK4819_SelectChip(chipForVfo(currentVfo));
  bool sqOpen = (squelchLevel == 0) || BK4819_IsSquelchOpen();
  applyMute(sqOpen);
}

void Radio_SetSquelch(uint8_t level) {
  squelchLevel = level;
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    if (vfos[i].radio == RADIO_SI4732)
      continue;
    BK4819_SelectChip(chipForVfo(i));
    BK4819_Squelch(level, 1, 1);
  }
  if (vfos[currentVfo].radio != RADIO_SI4732)
    BK4819_SelectChip(chipForVfo(currentVfo));
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

void Radio_NextFilter(void) { filterIndex = (filterIndex + 1) % 4; }

void Radio_ToggleFilter(void) {
  BK4819_ToggleFilter(filters[filterIndex],
                      !(BK4819_GetFilter() & filters[filterIndex]));
}
