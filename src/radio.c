#include "radio.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/si473x.h"
#include "helper/log.h"
#include "helper/measurements.h"
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
    "20Hz", "50Hz", "500Hz", "1k",  "2.5k", "5k",   "6.25k", "8.33k",
    "9k",   "10k",  "12.5k", "25k", "50k",  "100k", "500k",
};

uint8_t squelchLevel = 4;

/* Состояние AFDAC и SI4732 — пишем только при изменении */
static bool bk0DacOn = false;
static bool siUnmuted = false;
static bool afMuteOn = false; // PIN_AF_MUTE last state

/* Применить mute-состояние. Пишет в железо только при изменении. */
static SI47XX_MODE modToSiMode(ModulationType mod) {
  switch (mod) {
    case MOD_AM: return SI47XX_AM;
    case MOD_LSB: return SI47XX_LSB;
    case MOD_USB: return SI47XX_USB;
    default: return SI47XX_FM;
  }
}

static ModulationType Radio_AutoModulation(uint32_t f) {
  if (f >= 150000 && f < 2000000) return MOD_AM;
  if (f >= 2000000 && f <= 30000000) return MOD_AM;
  if (f > 30000000 && f < 80000000)  return MOD_FM;
  if (f >= 88000000 && f <= 108000000) return MOD_WFM;
  return MOD_FM;
}

static void applyMute(bool sqOpen) {
  bool wantBk0 = (vfos[currentVfo].radio == RADIO_BK4819A) && sqOpen;
  if (wantBk0 != bk0DacOn) {
    BK4819_ToggleAFDAC(wantBk0);
    bk0DacOn = wantBk0;
  }

  /* chip 1 — TX only, пока не трогаем */

  bool wantSi = (vfos[currentVfo].radio == RADIO_SI4732);
  if (wantSi != siUnmuted) {
    SI47XX_SetVolume(wantSi ? 63 : 0);
    siUnmuted = wantSi;
  }

  if (sqOpen != afMuteOn) {
    gpio_pin_write(&PIN_AF_MUTE, sqOpen);
    afMuteOn = sqOpen;
  }
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
    SI47XX_SetAutomaticGainControl(0, 0);
  } else {
    BK4819_SelectFilterByFrequency(f);
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
  SQL sq = GetSql(squelchLevel);
  BK4819_SetupSquelch(sq, 1, 1);

  SI47XX_Init();
  si4732mode = modToSiMode(vfos[2].modulation);
  SI47XX_PowerUp();
  SI47XX_TuneTo(vfos[2].rxF);

  /* Начальное состояние: активен VFO 0 (BK4819A) */
  bk0DacOn = false; // форсируем запись
  siUnmuted = true; // форсируем запись SetVolume(0)
  applyMute(true);
}

/* Вызывается каждый update-цикл. Опрашивает шумодав и обновляет AFDAC
    только если состояние изменилось — SPI не пишется вхолостую. */
void Radio_Update(void) {
  if (vfos[currentVfo].radio == RADIO_SI4732)
    return;

  bool sqOpen = (squelchLevel == 0) || BK4819_IsSquelchOpen();
  applyMute(sqOpen);
}

void Radio_SetSquelch(uint8_t level) {
  squelchLevel = level;
  SQL sq = GetSql(level);
  BK4819_SetupSquelch(sq, 1, 1);
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
