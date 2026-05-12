#include "radio.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "helper/log.h"
#include "ui/graphics.h"

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

static void applyVfo(bool precise) {
  uint32_t f = vfos[currentVfo].rxF;
  BK4819_TuneTo(f, precise);

  if (f >= 240 * MHZ) {
    BK4819_ToggleFilter(FILTER_VHF, false);
    BK4819_ToggleFilter(FILTER_UHF, true);
  } else {
    BK4819_ToggleFilter(FILTER_VHF, true);
    BK4819_ToggleFilter(FILTER_UHF, false);
  }
}

void Radio_Init(void) {
  BK4819_SelectChip(0);
  BK4819_Init();
  BK4819_RX_TurnOn();
  BK4819_SetAFC(0);
  BK4819_SetAGC(true, 1);
  BK4819_SetFilterBandwidth(vfos[0].bw);
  BK4819_TuneTo(vfos[0].rxF, true);
  BK4819_SetModulation(vfos[0].modulation);

  gpio_pin_set(&PIN_AF_MUTE);
}

void Radio_TuneTo(uint32_t f, bool precise) {
  vfos[currentVfo].rxF = f;
  applyVfo(precise);
}

void Radio_TuneStep(int8_t dir) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  Radio_TuneTo(vfos[currentVfo].rxF + step * dir, false);
}

void Radio_NextVfo(void) {
  currentVfo = (currentVfo + 1) % DEVICE_VFO_COUNT;
  applyVfo(true);

  /* if (currentVfo == 0) {
    gpio_pin_clr(&PIN_RX_TS);
  } else if (currentVfo == 1) {
    gpio_pin_set(&PIN_RX_TS);
  } */
}

void Radio_NextFilter(void) { filterIndex = (filterIndex + 1) % 4; }

void Radio_ToggleFilter(void) {
  BK4819_ToggleFilter(filters[filterIndex],
                      !(BK4819_GetFilter() & filters[filterIndex]));
}
