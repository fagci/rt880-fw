#include "vfo.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/measurements.h"
#include "../helper/log.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/numval.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "mode.h"
#include "radio.h"
#include "scanner.h"
#include "fc.h"
#include "analyser.h"
#include "printf.h"
#include <string.h>

#define VFO_H 64
#define STATUS_H 24

#define VFO_LABEL_Y 17
#define VFO_FREQ_Y 30
#define VFO_STEP_Y 48
#define VFO_RSSI_Y (VFO_STEP_Y - 8)

#define EXTRA_W (11 * 2 + 8)
#define EXTRA_X (LCD_WIDTH - 8 - EXTRA_W)
#define EXTRA_COL_W 32

#define VFO_LABEL_W 30
#define VFO_RSSI_W (LCD_WIDTH - LCD_WIDTH / 3)

struct {
  NumVal_t numVal[DEVICE_VFO_COUNT];
  uint8_t prevActive;
  uint8_t prevMod[DEVICE_VFO_COUNT];
  uint8_t prevStep;
  uint8_t prevRssi[DEVICE_VFO_COUNT];
  bool inited;
} vfoCtx;

static bool secondReceiver;

static const char *MOD_NAMES[] = {
    [MOD_FM] = "FM",   [MOD_AM] = "AM",   [MOD_LSB] = "LSB", [MOD_USB] = "USB",
    [MOD_BYP] = "BYP", [MOD_RAW] = "RAW", [MOD_WFM] = "WFM",
};

static void onFreqEntered(uint32_t f) {
  Radio_TuneToAuto(f);
  Log("tune vfo=%u f=%lu mod=%u", currentVfo, f, vfos[currentVfo].modulation);
}

static void initNumVals() {
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    uint16_t sy = STATUS_H + i * VFO_H;
    NumVal_Init(&vfoCtx.numVal[i], LCD_WIDTH - 8 - EXTRA_W - 4, sy + VFO_FREQ_Y,
                F_MONO_LG, 21, 23, UNIT_MHZ, vfos[i].rxF, 0, 1340 * MHZ,
                onFreqEntered, POS_R, F_SM, 11, 12);
  }
}

static void renderVfoLabel(uint8_t i, uint16_t sy, bool active) {
  Color clr = active ? C_GREEN : C_GRAY;

  FillRect(EXTRA_X, sy + VFO_LABEL_Y - 10, EXTRA_COL_W, 12, BG());
  PrintfT(LCD_WIDTH - 8, sy + VFO_LABEL_Y,
          text_style(clr, C_BLACK, POS_R, F_SM), "%3s",
          MOD_NAMES[vfos[i].modulation]);
}

static void renderVfoStep(uint8_t i, uint16_t sy) {
  uint32_t step = StepFrequencyTable[vfos[i].step];
  PrintfT(LCD_WIDTH - 8, sy + VFO_STEP_Y,
          text_style(C_YELLOW, C_BLACK, POS_R, F_SM), "%3u.%02uk", step / 100,
          step % 100);
}

static void renderVfoRssi(uint8_t i, uint16_t sy, uint8_t w) {
  (void)i;
  if (w)
    FillRect(8, sy + VFO_RSSI_Y, w, 8, C_GREEN);
  FillRect(8 + w, sy + VFO_RSSI_Y, VFO_RSSI_W - w, 8, BG());
}

static void renderVfo(uint8_t i) {
  uint16_t sy = STATUS_H + i * VFO_H;
  bool active = (i == currentVfo);
  bool wasActive = (vfoCtx.prevActive == i);
  bool firstRender = !vfoCtx.inited;

  if (firstRender || active != wasActive ||
      vfos[i].modulation != vfoCtx.prevMod[i]) {
    renderVfoLabel(i, sy, active);
  }

  NumVal_Render(&vfoCtx.numVal[i]);

  if (active) {
    if (firstRender || vfos[i].step != vfoCtx.prevStep || !wasActive) {
      renderVfoStep(i, sy);
    }
  } else {
    if (wasActive) {
    }
  }

  if (vfos[i].radio != RADIO_SI4732) {
    uint8_t w = 0;
    if (active) {
      w = ConvertDomain(BK4819_GetRSSI(), 0, 255, 0, VFO_RSSI_W);
    }
    if (firstRender || w != vfoCtx.prevRssi[i] || active != wasActive) {
      renderVfoRssi(i, sy, w);
      vfoCtx.prevRssi[i] = w;
    }
  }
}

static void renderFilterList() {
  uint16_t baseY = VFO_H * DEVICE_VFO_COUNT + STATUS_H;
  for (uint8_t i = 0; i < 4; i++) {
    uint16_t y = baseY + i * 14;
    char marker = filterIndex == i ? '>' : ' ';
    FillRect(2, y - 10, 14, 12, BG());
    PrintfT(2, y, text_style(C_GREEN, C_BLACK, POS_L, F_SM), "%c %s", marker,
            FILTER_NAMES[i]);
  }
}

class VfoMode : public Mode {
public:
  void enter() override {
    UI_ClearScreen(C_BLACK);
    memset(&vfoCtx, 0, sizeof(vfoCtx));
    initNumVals();
    Radio_Init();
  }

  void update() override {
    Radio_Update();

    for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
      if (vfos[i].rxF != NumVal_GetValue(&vfoCtx.numVal[i]))
        NumVal_SetValue(&vfoCtx.numVal[i], vfos[i].rxF);
      NumVal_Update(&vfoCtx.numVal[i]);
    }
  }

  void render() override {
    STATUSLINE_render();

    for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++)
      renderVfo(i);

    vfoCtx.prevActive = currentVfo;
    vfoCtx.prevStep = vfos[currentVfo].step;
    for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++)
      vfoCtx.prevMod[i] = vfos[i].modulation;
    vfoCtx.inited = true;

    renderFilterList();

    Log_Render(0, LCD_HEIGHT - 8 * 6, 6);
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (NumVal_Key(&vfoCtx.numVal[currentVfo], k, state))
      return true;

    if (state == KEY_LONG_PRESSED_CONT) {
      switch (k) {
      case KEY_UP:
        Radio_TuneStep(+1);
        return true;
      case KEY_DOWN:
        Radio_TuneStep(-1);
        return true;
      default:
        break;
      }
    }

    if (state == KEY_EVT_LONG_PRESS) {
      switch (k) {
      case KEY_3:
        Radio_NextStep();
        return true;
      case KEY_9:
        Radio_PrevStep();
        return true;
case KEY_4:
         Mode_Switch(MODE_FC);
         return true;
       case KEY_5:
         Mode_Switch(MODE_ANALYSER);
         return true;
       case KEY_0:
         Mode_Switch(MODE_RADIO);
         return true;
      case KEY_6:
        vfos[currentVfo].modulation =
            static_cast<ModulationType>((vfos[currentVfo].modulation + 1) % (MOD_WFM + 1));
        Radio_TuneTo(vfos[currentVfo].rxF, true);
        return true;
      case KEY_SIDE2:
        gInverted = !gInverted;
        FillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, BG());
        return true;
      default:
        break;
      }
    }

    if (state != KEY_EVT_RELEASE)
      return false;

    switch (k) {
case KEY_MENU:
       Mode_Switch(MODE_SCANNER);
       return true;
    case KEY_EXIT:
      Radio_NextVfo();
      initNumVals();
      return true;
    case KEY_UP:
      Radio_TuneStep(+1);
      return true;
    case KEY_DOWN:
      Radio_TuneStep(-1);
      return true;
    case KEY_SIDE1:
      Radio_NextFilter();
      return true;
    case KEY_SIDE2:
      Radio_ToggleFilter();
      return true;
    case KEY_ALARM:
      secondReceiver = !secondReceiver;
      BK4819_SelectB(secondReceiver);
      Log("BK: %c", secondReceiver ? 'B' : 'A');
      return true;
    default:
      break;
    }
    return false;
  }
};

VfoMode vfoModeInstance;
Mode &MODE_VFO = vfoModeInstance;
