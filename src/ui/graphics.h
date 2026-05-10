#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "gfxfont.h"
#include <stdarg.h>
#include <stdint.h>

#define LCD_WIDTH ST7789_WIDTH
#define LCD_HEIGHT ST7789_HEIGHT

#include "fonts/FreeSans12pt7b.h"
#include "fonts/FreeSans18pt7b.h"
#include "fonts/FreeSans9pt7b.h"
#include "fonts/TomThumb.h"

#define F_SS (&TomThumb)
#define F_SM (&FreeSans9pt7b)
#define F_NORM (&FreeSans12pt7b)

typedef enum { POS_L, POS_C, POS_R } TextPos;
typedef uint16_t Color;

void UI_ClearScreen(Color bg);

void PutPixel(int16_t x, int16_t y, Color c);
void DrawVLine(int16_t x, int16_t y, int16_t h, Color c);
void DrawHLine(int16_t x, int16_t y, int16_t w, Color c);
void DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c);
void DrawRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c);
void FillRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c);
void FillCircle(int16_t x, int16_t y, int16_t r, Color c);

void Printf(uint8_t x, uint8_t y, const char *fmt, ...);
void PrintfEx(uint8_t x, uint8_t y, TextPos align, Color col, Color bg,
              const GFXfont *font, const char *fmt, ...);

#endif
