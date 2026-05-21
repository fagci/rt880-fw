#include "fc.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/log.h"
#include "../helper/measurements.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "mode.h"
#include "vfo.h"
#include "printf.h"
#include <stdlib.h>

#define FC_BOUND 150 * MHZ
#define FC_STATUS_H 24

#define REQUIRED_HITS 2
#define FILTER_SWITCH_INTERVAL 2

struct {
  uint8_t scanSpeed;
  uint16_t hz;
  Filter filter;
  bool bandAutoSwitch;
  bool isScanning;
  uint32_t currentFrequency;
  uint32_t lastDetectedFrequency;
  uint8_t frequencyHits;
  uint8_t filterSwitchCounter;
  uint32_t lastScanTime;
  uint32_t stopFreq;
  bool stopped;
  uint8_t savedVfo;
} fcCtx;

static void enableScan() {
  BK4819_EnableFrequencyScanEx2(
      (FreqScanTime)(fcCtx.scanSpeed > F_SC_T_COUNT - 1
                         ? 0
                         : fcCtx.scanSpeed),
      fcCtx.hz);
  fcCtx.isScanning = true;
  fcCtx.lastScanTime = millis();
}

static void disableScan() {
  BK4819_DisableFrequencyScan();
  BK4819_RX_TurnOn();
  fcCtx.isScanning = false;
}

static void switchFilter() {
  fcCtx.filter = (fcCtx.filter == FILTER_VHF) ? FILTER_UHF : FILTER_VHF;
  BK4819_SelectFilterEx(fcCtx.filter);
  fcCtx.filterSwitchCounter = 0;
}

class FcMode : public Mode {
public:
  void enter() override {
    Log("FC enter");
    fcCtx.savedVfo = currentVfo;
    if (vfos[currentVfo].radio != RADIO_BK4819A)
      currentVfo = 0;

    Radio_Init();
    fcCtx.filter = FILTER_VHF;
    BK4819_SelectFilterEx(fcCtx.filter);
    fcCtx.scanSpeed = 0;
    fcCtx.hz = 0x244;
    fcCtx.bandAutoSwitch = true;
    fcCtx.isScanning = false;
    fcCtx.currentFrequency = 0;
    fcCtx.lastDetectedFrequency = 0;
    fcCtx.frequencyHits = 0;
    fcCtx.filterSwitchCounter = 0;
    fcCtx.stopped = false;
    fcCtx.stopFreq = 0;
    enableScan();
  }

  void exit() override {
    disableScan();
    currentVfo = fcCtx.savedVfo;
  }

  void update() override {
    if (fcCtx.isScanning) {
      uint32_t freq;
      if (BK4819_GetFrequencyScanResult(&freq)) {
        fcCtx.currentFrequency = freq;
        bool freqOk = true;

        if (freq >= 88 * MHZ && freq <= 108 * MHZ)
          freqOk = false;

        if (fcCtx.filter == FILTER_VHF && freq >= FC_BOUND)
          freqOk = false;

        if (fcCtx.filter == FILTER_UHF && freq < FC_BOUND)
          freqOk = false;

        if (freqOk && DeltaF(freq, fcCtx.lastDetectedFrequency) < 300) {
          fcCtx.frequencyHits++;
        } else {
          fcCtx.frequencyHits = 1;
        }

        fcCtx.lastDetectedFrequency = freq;

        if (fcCtx.frequencyHits >= REQUIRED_HITS) {
          Log("FC hit %lu", freq);
          disableScan();
          Radio_TuneTo(freq, true);
          fcCtx.stopped = true;
          fcCtx.stopFreq = freq;
          fcCtx.frequencyHits = 0;
        } else {
          disableScan();
          enableScan();
        }
      }

      if (fcCtx.bandAutoSwitch && fcCtx.filterSwitchCounter >= FILTER_SWITCH_INTERVAL) {
        switchFilter();
      }

      if (millis() - fcCtx.lastScanTime >= (200u << fcCtx.scanSpeed)) {
        fcCtx.filterSwitchCounter++;
        fcCtx.lastScanTime = millis();
      }
    } else {
      if (!fcCtx.stopped) {
        enableScan();
      } else {
        bool sqOpen = (squelchLevel == 0) || BK4819_IsSquelchOpen();
        if (!sqOpen) {
          delay_ms(55);
          enableScan();
          fcCtx.stopped = false;
          fcCtx.stopFreq = 0;
        }
      }
    }
  }

  void render() override {
    STATUSLINE_render();

    {
      char status[48];
      snprintf(status, sizeof(status), "%s %ums HZ %u SQ %u %s",
               (fcCtx.filter & FILTER_VHF ? "VHF" : "UHF"), 200 << fcCtx.scanSpeed, fcCtx.hz,
               squelchLevel, fcCtx.bandAutoSwitch ? "[A]" : "");
      PrintfEx(0, FC_STATUS_H - 10, POS_L, C_WHITE, BG(), F_SM, "%s", status);
    }

    {
      char freqStr[16];
      snprintf(freqStr, sizeof(freqStr), "%lu.%03lu",
               fcCtx.currentFrequency / MHZ,
               (fcCtx.currentFrequency % MHZ) / 1000);
      FillRect(4, FC_STATUS_H + 2, LCD_WIDTH - 8, 36, BG());
      PrintfEx(LCD_XCENTER, FC_STATUS_H + 12, POS_C,
               fcCtx.stopped ? C_GREEN : C_YELLOW, BG(), F_MONO_LG, "%s", freqStr);
    }

    if (fcCtx.stopped) {
      FillRect(4, FC_STATUS_H + 44, LCD_WIDTH - 8, 16, BG());
      PrintfEx(LCD_XCENTER, FC_STATUS_H + 48, POS_C, C_GREEN, BG(), F_SM,
               "Squelch: %s", BK4819_IsSquelchOpen() ? "OPEN" : "CLOSED");
    }

    {
      char hint[64];
      snprintf(hint, sizeof(hint),
               "1/7:Speed 3/9:SQ 5:HZ F:Filter 0:Auto *:Exit PTT:VFO");
      PrintfEx(0, LCD_HEIGHT - 12, POS_L, C_GRAY, BG(), F_SS, "%s", hint);
    }

    Log_Render(0, LCD_HEIGHT - 24, 6);
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (state == KEY_EVT_RELEASE) {
      switch (k) {
      case KEY_1:
      case KEY_7:
        fcCtx.scanSpeed = IncDecU(fcCtx.scanSpeed, 0, 3, k == KEY_1);
        if (fcCtx.isScanning) {
          disableScan();
          enableScan();
        }
        return true;
      case KEY_3:
      case KEY_9:
        squelchLevel = IncDecU(squelchLevel, 0, 10, k == KEY_3);
        Radio_SetSquelch(squelchLevel);
        return true;
      case KEY_5:
        fcCtx.hz = fcCtx.hz == 0x244 ? 0x580 : 0x244;
        if (fcCtx.isScanning) {
          disableScan();
          enableScan();
        }
        return true;
      case KEY_HASH:
        switchFilter();
        return true;
      case KEY_0:
        fcCtx.bandAutoSwitch = !fcCtx.bandAutoSwitch;
        return true;
      case KEY_STAR:
      case KEY_PTT:
        if (fcCtx.stopped && fcCtx.stopFreq) {
          Radio_TuneTo(fcCtx.stopFreq, true);
        }
        Mode_Switch(MODE_VFO);
        return true;
      default:
        break;
      }
    }
    return false;
  }
};

FcMode fcModeInstance;
Mode &MODE_FC = fcModeInstance;
