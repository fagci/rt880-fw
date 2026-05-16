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
#include <stdio.h>
#include <stdlib.h>

#define FC_BOUND 150 * MHZ
#define FC_STATUS_H 24

#define REQUIRED_HITS 2
#define FILTER_SWITCH_INTERVAL 2

static const char *FC_FILTER_NAMES[] = {
    [FILTER_VHF] = "VHF",
    [FILTER_UHF] = "UHF",
};

typedef struct {
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
} FC_Ctx;

static FC_Ctx ctx;

static void enableScan(void) {
  BK4819_EnableFrequencyScanEx2(
      (FreqScanTime)(ctx.scanSpeed > F_SC_T_COUNT - 1
                         ? 0
                         : ctx.scanSpeed),
      ctx.hz);
  ctx.isScanning = true;
  ctx.lastScanTime = millis();
}

static void disableScan(void) {
  BK4819_DisableFrequencyScan();
  BK4819_RX_TurnOn();
  ctx.isScanning = false;
}

static void switchFilter(void) {
  ctx.filter = (ctx.filter == FILTER_VHF) ? FILTER_UHF : FILTER_VHF;
  BK4819_SelectFilterEx(ctx.filter);
  ctx.filterSwitchCounter = 0;
}

static void enter(Mode_t *self) {
  (void)self;
  Log("FC enter");
  ctx.savedVfo = currentVfo;
  if (vfos[currentVfo].radio != RADIO_BK4819A)
    currentVfo = 0;

  Radio_Init();
  ctx.filter = FILTER_VHF;
  BK4819_SelectFilterEx(ctx.filter);
  ctx.scanSpeed = 0;
  ctx.hz = 0x244;
  ctx.bandAutoSwitch = true;
  ctx.isScanning = false;
  ctx.currentFrequency = 0;
  ctx.lastDetectedFrequency = 0;
  ctx.frequencyHits = 0;
  ctx.filterSwitchCounter = 0;
  ctx.stopped = false;
  ctx.stopFreq = 0;
  enableScan();
}

static void exitMode(Mode_t *self) {
  (void)self;
  disableScan();
  currentVfo = ctx.savedVfo;
}

static void update(Mode_t *self) {
  (void)self;

  if (ctx.isScanning) {
    uint32_t freq;
    if (BK4819_GetFrequencyScanResult(&freq)) {
      ctx.currentFrequency = freq;
      bool freqOk = true;

      if (freq >= 88 * MHZ && freq <= 108 * MHZ)
        freqOk = false;

      if (ctx.filter == FILTER_VHF && freq >= FC_BOUND)
        freqOk = false;

      if (ctx.filter == FILTER_UHF && freq < FC_BOUND)
        freqOk = false;

      if (freqOk && DeltaF(freq, ctx.lastDetectedFrequency) < 300) {
        ctx.frequencyHits++;
      } else {
        ctx.frequencyHits = 1;
      }

      ctx.lastDetectedFrequency = freq;

      if (ctx.frequencyHits >= REQUIRED_HITS) {
        Log("FC hit %lu", freq);
        disableScan();
        Radio_TuneTo(freq, true);
        ctx.stopped = true;
        ctx.stopFreq = freq;
        ctx.frequencyHits = 0;
      } else {
        disableScan();
        enableScan();
      }
    }

    if (ctx.bandAutoSwitch && ctx.filterSwitchCounter >= FILTER_SWITCH_INTERVAL) {
      switchFilter();
    }

    if (millis() - ctx.lastScanTime >= (200u << ctx.scanSpeed)) {
      ctx.filterSwitchCounter++;
      ctx.lastScanTime = millis();
    }
  } else {
    if (!ctx.stopped) {
      enableScan();
    } else {
      bool sqOpen = (squelchLevel == 0) || BK4819_IsSquelchOpen();
      if (!sqOpen) {
        delay_ms(55);
        enableScan();
        ctx.stopped = false;
        ctx.stopFreq = 0;
      }
    }
  }
}

static void render(Mode_t *self) {
  (void)self;
  STATUSLINE_render();

  {
    char status[48];
    snprintf(status, sizeof(status), "%s %ums HZ %u SQ %u %s",
             FC_FILTER_NAMES[ctx.filter], 200 << ctx.scanSpeed, ctx.hz,
             squelchLevel, ctx.bandAutoSwitch ? "[A]" : "");
    PrintfEx(0, FC_STATUS_H - 10, POS_L, C_WHITE, BG(), F_SM, "%s", status);
  }

  {
    char freqStr[16];
    snprintf(freqStr, sizeof(freqStr), "%lu.%03lu",
             ctx.currentFrequency / MHZ,
             (ctx.currentFrequency % MHZ) / 1000);
    FillRect(4, FC_STATUS_H + 2, LCD_WIDTH - 8, 36, BG());
    PrintfEx(LCD_XCENTER, FC_STATUS_H + 12, POS_C,
             ctx.stopped ? C_GREEN : C_YELLOW, BG(), F_MONO_LG, "%s", freqStr);
  }

  if (ctx.stopped) {
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

static bool key(Mode_t *self, key_id_t k, key_evt_type_t state) {
  (void)self;

  if (state == KEY_EVT_RELEASE) {
    switch (k) {
    case KEY_1:
    case KEY_7:
      ctx.scanSpeed = IncDecU(ctx.scanSpeed, 0, 3, k == KEY_1);
      if (ctx.isScanning) {
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
      ctx.hz = ctx.hz == 0x244 ? 0x580 : 0x244;
      if (ctx.isScanning) {
        disableScan();
        enableScan();
      }
      return true;
    case KEY_HASH:
      switchFilter();
      return true;
    case KEY_0:
      ctx.bandAutoSwitch = !ctx.bandAutoSwitch;
      return true;
    case KEY_STAR:
    case KEY_PTT:
      if (ctx.stopped && ctx.stopFreq) {
        Radio_TuneTo(ctx.stopFreq, true);
      }
      Mode_Switch(&MODE_VFO);
      return true;
    default:
      break;
    }
  }
  return false;
}

Mode_t MODE_FC = {
    .enter = enter,
    .exit = exitMode,
    .update = update,
    .render = render,
    .key = key,
    .ctx = &ctx,
};
