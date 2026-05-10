#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "driver/st7789.h"
#include "helper/measurements.h"
#include "misc.h"
#include "ui/graphics.h"
#include "ui/spectrum.h"

typedef enum {
  RADIO_BK4819A,
  RADIO_BK4819B,
  RADIO_SI4732,
} Radio;

typedef enum {
  OFFSET_NONE,
  OFFSET_PLUS,
  OFFSET_MINUS,
  OFFSET_FREQ,
} OffsetDirection;

typedef enum {
  TX_POW_ULOW,
  TX_POW_LOW,
  TX_POW_MID,
  TX_POW_HIGH,
} TXOutputPower;

typedef enum {
  STEP_0_02kHz,
  STEP_0_05kHz,
  STEP_0_5kHz,
  STEP_1_0kHz,

  STEP_2_5kHz,
  STEP_5_0kHz,
  STEP_6_25kHz,
  STEP_8_33kHz,
  STEP_9_0kHz,
  STEP_10_0kHz,
  STEP_12_5kHz,
  STEP_25_0kHz,
  STEP_50_0kHz,
  STEP_100_0kHz,
  STEP_500_0kHz,

  STEP_COUNT,
} Step;

typedef struct {
  uint8_t value;
  uint8_t type : 4;
} Code;

typedef struct {
  Code rx;
  Code tx;
} CodeRXTX;

typedef struct {
  uint8_t value : 4;
  uint8_t type : 2;
} Squelch;

typedef struct {
  uint16_t scanlists;
  uint16_t channel;
  char name[10];
  uint32_t rxF : 27;
  int32_t ppm : 5;
  uint32_t txF : 27;
  OffsetDirection offsetDir : 2;
  bool allowTx : 1;
  uint8_t reserved2 : 2;
  Step step : 4;
  uint8_t modulation : 4;
  uint8_t bw : 4;
  Radio radio : 2;
  TXOutputPower power : 2;
  uint8_t mic : 4; // Mic gain (0-15)
  uint8_t scrambler : 4;
  Squelch squelch;
  CodeRXTX code;
  bool fixedBoundsMode : 1;
  bool isChMode : 1;
  uint8_t gainIndex : 5;
  uint8_t deviation; // Deviation setting (0-255, stored as value*10 when used)
  uint32_t upconverter; // Upconverter frequency shift (full 32-bit)
} __attribute__((packed)) VFO;

VFO vfos[3] = {
    (VFO){
        .radio = RADIO_BK4819A,
        .rxF = 17230000,
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
    },
    (VFO){
        .radio = RADIO_BK4819B,
        .rxF = 43392500,
        .step = STEP_25_0kHz,
        .modulation = MOD_FM,
        .bw = BK4819_FILTER_BW_12k,
    },
    (VFO){
        .radio = RADIO_SI4732,
        .rxF = 10480000,
        .step = STEP_100_0kHz,
        .modulation = MOD_WFM,
        .bw = BK4819_FILTER_BW_12k,
    },
};

static const uint8_t VFO_H = 64;
void renderVFO(VFO *vfo, uint8_t i) {
  uint16_t sy = 24 + i * VFO_H;

  FillRect(0, sy, LCD_WIDTH, VFO_H, C_BLACK);
  DrawRect(0, sy, LCD_WIDTH, VFO_H, C_GRAY);
  PrintfEx(8, sy + 4 + 7, POS_L, C_YELLOW, C_BLACK, F_SM, "VFO%u", i + 1);
  PrintfEx(8, sy + 4 + 7 + 2 + 18, POS_L, C_YELLOW, C_BLACK, F_NORM, "%u.%03u",
           vfo->rxF / MHZ, vfo->rxF % MHZ / KHZ);
}

int main(void) {
  board_init();

  st7789_init();
  st7789_backlight_on();
  gpio_pin_set(&PIN_KEYBOARD_BACKLIGHT);

  BK4819_SelectChip(0);
  BK4819_Init();
  BK4819_RX_TurnOn();
  BK4819_SetAFC(0);
  // BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, 1);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_TuneTo(17230000, true);

  gpio_pin_set(&PIN_AF_MUTE);

  for (uint8_t i = 0; i < 3; ++i) {
    VFO *vfo = &vfos[i];
    renderVFO(vfo, i);
  }
}
