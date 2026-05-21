#include "statusline.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/st7789.h"
#include "../helper/measurements.h"
#include "graphics.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "printf.h"

#define SL_H 24
#define BAT_X (LCD_WIDTH - 46)
#define BAT_Y 4
#define BAT_W 42
#define BAT_H 16
#define BAT_MIN 600
#define BAT_MAX 840

static uint16_t prevFlt;
static uint16_t prevBat;

typedef struct {
  uint16_t x, y;
  uint16_t w, h;
  uint16_t mask;
  const char *name;
} FilterLabel;

static const FilterLabel FL[] = {
    {LCD_XCENTER - 10, 2, 11, 10, FILTER_HF, "HF"},
    {LCD_XCENTER - 10, 10, 16, 10, FILTER_VHF, "VHF"},
    {LCD_XCENTER + 10, 2, 16, 10, FILTER_UHF, "UHF"},
    {LCD_XCENTER + 10, 10, 16, 10, FILTER_800, "800"},
};

static TextStyle makeFltStyle(bool on) {
  return text_style(on ? C_WHITE : C_DARK, C_BLACK, POS_C, F_SS);
}

static void draw_battery(void) {
  uint16_t bat = battery_voltage_cv();
  if (bat == prevBat) return;
  prevBat = bat;

  FillRect(BAT_X, BAT_Y, BAT_W, BAT_H, BG());

  uint8_t fill = (uint8_t)ConvertDomain((int)bat, BAT_MIN, BAT_MAX, 0, BAT_W - 4);
  uint16_t fcol;
  if (bat >= 740)      fcol = C_GREEN;
  else if (bat >= 680) fcol = C_YELLOW;
  else                 fcol = C_RED;

  FillRect(BAT_X + 2, BAT_Y + 2, fill, BAT_H - 4, fcol);
  DrawRect(BAT_X, BAT_Y, BAT_W, BAT_H, C_GRAY);

  PrintfEx(BAT_X + BAT_W / 2, BAT_Y + BAT_H / 2, POS_C,
           C_WHITE, fcol, F_SS, "%u.%02u", bat / 100, bat % 100);
}

void STATUSLINE_render(void) {
  uint16_t flt = BK4819_GetFilter();

  if (!prevFlt) {
    FillRect(0, 0, LCD_WIDTH, SL_H, BG());
    DrawHLine(0, SL_H - 1, LCD_WIDTH, C_GRAY);
    for (uint8_t i = 0; i < 4; i++) {
      bool on = flt & FL[i].mask;
      PrintfT(FL[i].x, FL[i].y + FL[i].h, makeFltStyle(on), "%s", FL[i].name);
    }
    prevFlt = flt;
    prevBat = 0;
    return;
  }

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

  draw_battery();
}

void STATUSLINE_SetText(const char *pattern, ...) {
  char statuslineText[32] = {0};
  va_list args;
  va_start(args, pattern);
  vsnprintf(statuslineText, 31, pattern, args);
  va_end(args);
  (void)statuslineText;
}
