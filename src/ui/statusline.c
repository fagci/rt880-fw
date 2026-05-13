#include "statusline.h"
// #include "../apps/apps.h"
// #include "../apps/messenger.h"
// #include "../driver/battery.h"
// #include "../driver/py25q16.h"
// #include "../driver/si473x.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/st7789.h"
#include "graphics.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint8_t previousBatteryLevel = 255;
static bool showBattery = true;

static uint32_t lastEepromWrite = 0;
static uint32_t lastTickerUpdate = 0;

static char statuslineText[32] = {0};
static char statuslineTicker[32] = {0};

void STATUSLINE_SetText(const char *pattern, ...) {
  char statuslineTextNew[32] = {0};
  va_list args;
  va_start(args, pattern);
  vsnprintf(statuslineTextNew, 31, pattern, args);
  va_end(args);
  if (strcmp(statuslineText, statuslineTextNew)) {
    strcpy(statuslineText, statuslineTextNew);
    // gRedrawScreen = true;
  }
}

void STATUSLINE_SetTickerText(const char *pattern, ...) {
  char statuslineTextNew[32] = {0};
  va_list args;
  va_start(args, pattern);
  vsnprintf(statuslineTextNew, 31, pattern, args);
  va_end(args);
  if (strcmp(statuslineTicker, statuslineTextNew)) {
    strcpy(statuslineTicker, statuslineTextNew);
    // gRedrawScreen = true;
  }
  lastTickerUpdate = millis();
}

void STATUSLINE_update(void) { const uint32_t now = millis(); }

void STATUSLINE_render(void) {
  static uint16_t oldFlt;
  static bool oldRxTs;

  uint16_t flt = BK4819_GetFilter();
  bool rxTs = gpio_pin_read(&PIN_RX_TS);

  bool needRender = flt != oldFlt || rxTs != oldRxTs;

  if (needRender) {
    FillRect(0, 0, LCD_WIDTH, 24, C_BLACK);
    DrawHLine(0, 24, LCD_WIDTH, C_GRAY);
    PrintfEx(LCD_XCENTER - 10, 12, POS_C, flt & FILTER_HF ? C_WHITE : C_DARK,
             C_BLACK, F_SS, "HF");
    PrintfEx(LCD_XCENTER - 10, 20, POS_C, flt & FILTER_VHF ? C_WHITE : C_DARK,
             C_BLACK, F_SS, "VHF");
    PrintfEx(LCD_XCENTER + 10, 12, POS_C, flt & FILTER_UHF ? C_WHITE : C_DARK,
             C_BLACK, F_SS, "UHF");
    PrintfEx(LCD_XCENTER + 10, 20, POS_C, flt & FILTER_800 ? C_WHITE : C_DARK,
             C_BLACK, F_SS, "800");
    PrintfEx(0, 22, POS_L, C_WHITE, C_BLACK, F_SS, "RX TS: %u", rxTs);

    // HERE update all old vals
    oldFlt = flt;
    oldRxTs = rxTs;
  }
}

/* void STATUSLINE_RenderRadioSettings() {
  char gain[8];
  char bandwidth[8];
  char squelch_type[8];
  char squelch_value[8];
  char modulation[8];

  strcpy(gain, RADIO_GetParamValueString(ctx, PARAM_GAIN));
  strcpy(bandwidth, RADIO_GetParamValueString(ctx, PARAM_BANDWIDTH));
  strcpy(squelch_type, RADIO_GetParamValueString(ctx, PARAM_SQUELCH_TYPE));
  strcpy(squelch_value, RADIO_GetParamValueString(ctx, PARAM_SQUELCH_VALUE));
  strcpy(modulation, RADIO_GetParamValueString(ctx, PARAM_MODULATION));

  STATUSLINE_SetText("%s %s %s %s %s", gain, bandwidth, squelch_type,
                     squelch_value, modulation);
} */
