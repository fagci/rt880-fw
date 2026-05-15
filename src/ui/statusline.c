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
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SL_H 24

/* ── Позиции и размеры лейблов фильтров ─────────────────────────── */
typedef struct {
  uint16_t x, y;
  uint16_t w, h;
  uint16_t mask;
  const char *name;
} FilterLabel;

/* TomThumb ~5px/char: HF=10, VHF/UHF/800=15; +1px запас */
static const FilterLabel FL[] = {
    {LCD_XCENTER - 10, 2,  11, 10, FILTER_HF,  "HF"},
    {LCD_XCENTER - 10, 10, 16, 10, FILTER_VHF, "VHF"},
    {LCD_XCENTER + 10,  2, 16, 10, FILTER_UHF, "UHF"},
    {LCD_XCENTER + 10, 10, 16, 10, FILTER_800, "800"},
};

static uint16_t prevFlt;

/* Стиль для одного лейбла */
static TextStyle makeFltStyle(bool on) {
  return text_style(on ? C_WHITE : C_DARK, C_BLACK, POS_C, F_SS);
}

void STATUSLINE_render(void) {
  uint16_t flt = BK4819_GetFilter();

  if (!prevFlt) {
    /* Первый проход: рисуем фон, разделитель и все лейблы */
    FillRect(0, 0, LCD_WIDTH, SL_H, BG());
    DrawHLine(0, SL_H - 1, LCD_WIDTH, C_GRAY);
    for (uint8_t i = 0; i < 4; i++) {
      bool on = flt & FL[i].mask;
      PrintfT(FL[i].x, FL[i].y + FL[i].h, makeFltStyle(on), "%s", FL[i].name);
    }
    prevFlt = flt;
    return;
  }

  /* Инкрементальный рендер: только изменившиеся биты */
  uint16_t changed = flt ^ prevFlt;
  if (changed) {
    for (uint8_t i = 0; i < 4; i++) {
      if (changed & FL[i].mask) {
        bool on = flt & FL[i].mask;
        PrintfT(FL[i].x, FL[i].y + FL[i].h, makeFltStyle(on), "%s", FL[i].name);
      }
    }
    prevFlt = flt;
  }
}

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
