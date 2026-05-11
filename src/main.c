#include "driver/bk4819-regs.h"
#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "driver/st7789.h"
#include "helper/measurements.h"
#include "misc.h"
#include "ui/finput.h"
#include "ui/graphics.h"
#include "ui/numval.h"
#include "ui/spectrum.h"
#include "ui/statusline.h"
#include <stdint.h>

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

NumVal_t numVal[3];

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

uint8_t currentVfo = 0;

const uint16_t StepFrequencyTable[15] = {
    2,   5,   50,  100,

    250, 500, 625, 833, 900, 1000, 1250, 2500, 5000, 10000, 50000,
};

#define LOG_LINES 8
#define LOG_LINE_W 32

static char logBuf[LOG_LINES][LOG_LINE_W];
static uint8_t logHead = 0; // индекс следующей строки для записи

void Log(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(logBuf[logHead % LOG_LINES], LOG_LINE_W, fmt, args);
  va_end(args);
  logHead++;
}

void LogRender(uint16_t x, uint16_t y, uint8_t lineH) {
  for (uint8_t i = 0; i < LOG_LINES; i++) {
    uint8_t idx = (logHead + i) % LOG_LINES;
    PrintfEx(x, y + i * lineH, POS_L, C_YELLOW, C_BLACK, F_SS, "%s",
             logBuf[idx]);
  }
}

void applyVFO(bool precise) {
  uint32_t f = vfos[currentVfo].rxF;
  if (currentVfo != 2) {
    BK4819_SelectChip(currentVfo);
    BK4819_TuneTo(f, precise);

    BK4819_SelectChip(0);
    if (f >= 240 * MHZ) {
      BK4819_ToggleGpioOut(BK4819_GPIO2_VHF_RX, false);
      BK4819_ToggleGpioOut(BK4819_GPIO1_UHF_RX, true);
    } else {
      BK4819_ToggleGpioOut(BK4819_GPIO2_VHF_RX, true);
      BK4819_ToggleGpioOut(BK4819_GPIO1_UHF_RX, false);
    }
  }

  Log("radio=%u, rxF=%lu, UHF? %u", currentVfo, f, f >= 240 * MHZ);
  LogRender(0, LCD_HEIGHT - LOG_LINES * 6, 6);
}

void tuneTo(uint32_t f, uint32_t f2) {
  (void)f2;
  vfos[currentVfo].rxF = f;
  numVal[currentVfo].value = f;
  NumVal_Render(&numVal[currentVfo]);
  applyVFO(true);
}

void tuneToSoft(uint32_t f) {
  vfos[currentVfo].rxF = f;
  numVal[currentVfo].value = f;
  NumVal_Render(&numVal[currentVfo]);
  applyVFO(false);
}

static const uint8_t VFO_H = 64;
static const uint8_t STATUS_H = 24;

void renderVFO(VFO *vfo, uint8_t i) {
  uint16_t sy = STATUS_H + i * VFO_H;

  DrawRect(0, sy, LCD_WIDTH, VFO_H, currentVfo == i ? C_GRAY : C_BLACK);
  PrintfEx(8, sy + 4 + 13, POS_L, C_GRAY, C_BLACK, F_SM, "VFO%u", i + 1);
}

void initVFOs() {
  for (uint8_t i = 0; i < 3; ++i) {
    uint16_t sy = STATUS_H + i * VFO_H;
    NumVal_Init(&numVal[i], 8, sy + 4 + 13 + 2 + 23, F_MONO_LG, 21, 23,
                UNIT_MHZ, vfos[i].rxF, 0, 1340 * MHZ, tuneTo);
  }
}

void renderVFOs() {
  for (uint8_t i = 0; i < 3; ++i) {
    VFO *vfo = &vfos[i];
    renderVFO(vfo, i);
  }
}

bool onKey(key_id_t key, key_evt_type_t state) {
  if (NumVal_Key(&numVal[currentVfo], key, state)) {
    return true;
  }
  if (state == KEY_EVT_RELEASE) {

    switch (key) {
    case KEY_EXIT:
      currentVfo = (currentVfo + 1) % 3;
      return true;
    case KEY_UP:
    case KEY_DOWN:
      tuneToSoft(vfos[currentVfo].rxF +
                 StepFrequencyTable[vfos[currentVfo].step] *
                     (key == KEY_UP ? 1 : -1));

      return true;
    }
  }
  return false;
}

void render() {
  STATUSLINE_render();
  renderVFOs();
  if (gFInputActive) {
    FINPUT_render();
  }
}

int main(void) {
  board_init();

  st7789_init();
  st7789_backlight_on();
  gpio_pin_set(&PIN_KEYBOARD_BACKLIGHT);

  UI_ClearScreen(C_BLACK);

  BK4819_SelectChip(0);
  BK4819_Init();
  BK4819_RX_TurnOn();
  BK4819_SetAFC(0);
  // BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, 1);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_TuneTo(17230000, true);

  gpio_pin_set(&PIN_AF_MUTE);

  UI_ClearScreen(C_BLACK);

  initVFOs();
  render();

  for (;;) {
    key_event_t evt;
    while (keyboard_get_event(&evt)) {
      if (onKey(evt.key, evt.type)) {
        render();
      }
    }
    NumVal_Update(&numVal[currentVfo]);
    if (gRedrawScreen) {
      render();
      gRedrawScreen = false;
    }
    for (uint8_t i = 0; i < 3; ++i) {
      NumVal_Render(&numVal[i]);
    }
  }
}
