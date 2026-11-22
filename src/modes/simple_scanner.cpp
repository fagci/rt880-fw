#include "simple_scanner.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/measurements.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/numval.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "mode.h"
#include "vfo.h"
#include "printf.h"
#include <stdlib.h>

#define STATUS_H 24
#define SP_H 64
#define SPECTRUM_Y (STATUS_H + 2 + 12)
#define SCAN_STEPS_PER_UPDATE 128

static FRange range = {
    .start = 400 * MHZ,
    .end = 470 * MHZ,
};

struct {
  NumVal_t numVal[2];
} simpleScannerCtx;

static bool spectrumReady;
static bool endFSel;
static uint8_t savedVfo;

static uint32_t stepCount;
static uint32_t lastStepTime;
static uint32_t stepsPerSec;

static void onRangeChanged() {
  SP_Init(&range, StepFrequencyTable[vfos[currentVfo].step],
          range.end - range.start);
  Radio_TuneTo(range.start - StepFrequencyTable[vfos[currentVfo].step], true);
  FillRect(0, STATUS_H, LCD_WIDTH, SPECTRUM_Y + SP_H - STATUS_H, C_BLACK);
  NumVal_Invalidate(&simpleScannerCtx.numVal[0]);
  NumVal_Invalidate(&simpleScannerCtx.numVal[1]);
  stepCount = 0;
  lastStepTime = millis();
  stepsPerSec = 0;
}

static void onStartEntered(uint32_t f) {
  range.start = f;
  onRangeChanged();
}
static void onEndEntered(uint32_t f) {
  range.end = f;
  onRangeChanged();
}

static void initNumVals() {
  NumVal_Init(&simpleScannerCtx.numVal[0], 8, SPECTRUM_Y - 2, F_M, 11, 12, UNIT_MHZ,
              range.start, 0, 1340 * MHZ, onStartEntered, POS_L, nullptr, 0, 0);
  NumVal_Init(&simpleScannerCtx.numVal[1], LCD_WIDTH - 8, SPECTRUM_Y - 2, F_M, 11, 12,
              UNIT_MHZ, range.end, 0, 1340 * MHZ, onEndEntered, POS_R, nullptr, 0,
              0);
}

class SimpleScannerMode : public Mode {
public:
  void enter() override {
    savedVfo = currentVfo;
    if (vfos[currentVfo].radio != RADIO_BK4819A)
      currentVfo = 0;
    initNumVals();
    Radio_Init();

    SQL sql = {
        .ro = 0,
        .rc = 0,
        .no = 46,
        .nc = 50,
        .go = 255,
        .gc = 255,
    };
    BK4819_SetupSquelch(sql, 0, 0);
    BK4819_SquelchType(SQUELCH_RSSI_NOISE_GLITCH);

    onRangeChanged();
    WF_Clear(SPECTRUM_Y + SP_H);
  }

  void update() override {
    if (spectrumReady)
      return;

    for (uint8_t i = 0; i < SCAN_STEPS_PER_UPDATE; i++) {
      Radio_TuneTo(
          vfos[currentVfo].rxF + StepFrequencyTable[vfos[currentVfo].step], true);

      delay_ms(10);

      Measurement m = {
          .f = vfos[currentVfo].rxF,
          .rssi = BK4819_GetRSSI(),
          .noise = BK4819_GetNoise(),
          .open = BK4819_IsSquelchOpen(),
      };
      SP_AddPoint(&m);
      stepCount++;

      if (vfos[currentVfo].rxF >= range.end) {
        spectrumReady = true;
        Radio_TuneTo(range.start - StepFrequencyTable[vfos[currentVfo].step],
                     false);
        break;
      }
    }

    uint32_t now = millis();
    if (now - lastStepTime >= 1000) {
      stepsPerSec = stepCount * 1000 / (now - lastStepTime);
      stepCount = 0;
      lastStepTime = now;
    }
  }

  void render() override {
    STATUSLINE_render();
    NumVal_Render(&simpleScannerCtx.numVal[0]);
    NumVal_Render(&simpleScannerCtx.numVal[1]);

    if (spectrumReady || millis() - lastSpectrumRender >= 500) {
      SP_Render(&range, STATUS_H + 14, SP_H);

      DrawHLine(8, SPECTRUM_Y, 32, endFSel ? C_BLACK : C_WHITE);
      DrawHLine(LCD_WIDTH - 8 - 32, SPECTRUM_Y, 32, !endFSel ? C_BLACK : C_WHITE);

      {
        int16_t tw = TextWidth("9999/s", F_M);
        FillRect(LCD_XCENTER - tw / 2, SPECTRUM_Y - 14, tw, 12, BG());
        PrintfT(LCD_XCENTER, SPECTRUM_Y - 2,
                text_style(C_WHITE, C_BLACK, POS_C, F_M), "%4u/s", stepsPerSec);
      }
      rendered = true;
    }
    if (spectrumReady) {
      WF_Render(SPECTRUM_Y + SP_H, true);
      spectrumReady = false;
      SP_Begin();
      rendered = true;
    }
    if (rendered) {
      lastSpectrumRender = millis();
      rendered = false;
    }
  }

  void exit() override {
    currentVfo = savedVfo;
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (NumVal_Key(&simpleScannerCtx.numVal[endFSel], k, state))
      return true;

    if (state != KEY_EVT_RELEASE)
      return false;

    switch (k) {
case KEY_MENU:
       Mode_Switch(MODE_VFO);
       return true;
    case KEY_EXIT:
      endFSel = !endFSel;
      return true;
    default:
      break;
    }
    return false;
  }

private:
  uint32_t lastSpectrumRender = 0;
  bool rendered = false;
};

SimpleScannerMode simpleScannerModeInstance;
Mode &MODE_SIMPLE_SCANNER = simpleScannerModeInstance;
