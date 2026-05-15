#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "gfxfont.h"
#include "driver/st7789.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#define LCD_WIDTH ST7789_WIDTH
#define LCD_HEIGHT ST7789_HEIGHT
#define LCD_XCENTER (ST7789_WIDTH / 2)

#include "fonts/FreeMono12pt7b.h"
#include "fonts/FreeMono18pt7b.h"
#include "fonts/FreeMono24pt7b.h"
#include "fonts/FreeMono9pt7b.h"
#include "fonts/TomThumb.h"
#include "fonts/symbols.h"

#define F_SS (&TomThumb)

#define F_SYM (&Symbols)

#define F_LG (&FreeMono18pt7b)
// 12*11
#define F_SM (&FreeMono9pt7b)
#define F_NORM (&FreeMono12pt7b)
#define F_MONO_LG (&FreeMono18pt7b)

typedef enum { POS_L, POS_C, POS_R } TextPos;
typedef uint16_t Color;

extern bool gRedrawScreen;
extern bool gInverted;

/* Текущий цвет фона (учитывает инверсию) */
static inline Color BG(void) { return gInverted ? C_WHITE : C_BLACK; }

/* ── Стиль текста — объединяет шрифт, цвета, выравнивание ── */
typedef struct {
  const GFXfont *font;
  Color fg;
  Color bg;
  TextPos align;
} TextStyle;

static inline TextStyle text_style(Color fg, Color bg, TextPos align,
                                    const GFXfont *font) {
  return (TextStyle){.font = font, .fg = fg, .bg = bg, .align = align};
}

/* Пресеты */
#define STYLE_SM_WHITE   text_style(C_WHITE, C_BLACK, POS_L, F_SM)
#define STYLE_SM_YELLOW  text_style(C_YELLOW, C_BLACK, POS_L, F_SM)
#define STYLE_SS_DARK    text_style(C_DARK, C_BLACK, POS_C, F_SS)

void UI_ClearScreen(Color bg);

void PutPixel(int16_t x, int16_t y, Color c);
void DrawVLine(int16_t x, int16_t y, int16_t h, Color c);
void DrawHLine(int16_t x, int16_t y, int16_t w, Color c);
void DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c);
void DrawRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c);
void FillRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c);
void FillCircle(int16_t x, int16_t y, int16_t r, Color c);

/* Ширина текста в пикселях */
int16_t TextWidth(const char *s, const GFXfont *f);

void Printf(uint8_t x, uint16_t y, const char *fmt, ...);
void PrintfEx(uint8_t x, uint16_t y, TextPos align, Color col, Color bg,
              const GFXfont *font, const char *fmt, ...);

/* Версия с TextStyle — 4 параметра вместо 7 */
void PrintfT(uint8_t x, uint16_t y, TextStyle st, const char *fmt, ...);

#endif
