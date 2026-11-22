#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "lvgl.h"
#include "driver/st7789.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#define LCD_WIDTH ST7789_WIDTH
#define LCD_HEIGHT ST7789_HEIGHT
#define LCD_XCENTER (ST7789_WIDTH / 2)

/* Шрифты (tools/gen_fonts.sh): S — Cozette 13 px / 1 bpp (пиксельный,
 * ноль перечёркнут); M/L — JetBrains Mono Medium/ExtraBold 14/20 px;
 * XL — JetBrains Mono ExtraBold 38 px. В M/L/XL точка в нуле убрана.
 * Все моноширинные. */
extern const lv_font_t ui_font_s, ui_font_m, ui_font_l, ui_font_xl;
typedef const lv_font_t *Font;
#define F_S (&ui_font_s)
#define F_M (&ui_font_m)
#define F_L (&ui_font_l)
#define F_XL (&ui_font_xl)

typedef enum { POS_L, POS_C, POS_R } TextPos;
typedef uint16_t Color;

/* Тема глобальная (длинный SIDE2 в VFO). Код рисует «тёмными» цветами
 * C_WHITE/C_YELLOW/…; в светлой теме слой graphics отображает их через TC()
 * (белый↔чёрный, насыщенные цвета темнее). Панели спектра/водопада
 * остаются тёмными: на время их рисования gMapColors = false. */
extern bool gThemeLight;
extern bool gMapColors;
Color TC(Color c);
static inline Color BG(void) { return TC(C_BLACK); }

/* Метрики шрифта */
int16_t FontW(Font f);      /* ширина символа (моноширинный) */
int16_t FontH(Font f);      /* высота строки */
int16_t FontAscent(Font f); /* от верха строки до базовой линии */

/* ── Стиль текста — объединяет шрифт, цвета, выравнивание ── */
typedef struct {
  Font font;
  Color fg;
  Color bg;
  TextPos align;
} TextStyle;

static inline TextStyle text_style(Color fg, Color bg, TextPos align,
                                    Font font) {
  return (TextStyle){.font = font, .fg = fg, .bg = bg, .align = align};
}

/* Пресеты */
#define STYLE_SM_WHITE   text_style(C_WHITE, C_BLACK, POS_L, F_M)
#define STYLE_SM_YELLOW  text_style(C_YELLOW, C_BLACK, POS_L, F_M)
#define STYLE_SS_DARK    text_style(C_DARK, C_BLACK, POS_C, F_S)

void UI_ClearScreen(Color bg);
void UI_ReleaseLabels(void); /* удалить лейблы пула Printf* (см. graphics.cpp) */

void PutPixel(int16_t x, int16_t y, Color c);
void DrawVLine(int16_t x, int16_t y, int16_t h, Color c);
void DrawHLine(int16_t x, int16_t y, int16_t w, Color c);
void DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c);
void DrawRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c);
void FillRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c);
/* Заливка без инвалидации перекрытых лейблов: для курсора ввода, который
 * лежит поверх «пустого» пробела поля и не должен затираться его фоном. */
void FillRectNoInv(int16_t x, int16_t y, int16_t w, int16_t h, Color c);
void FillCircle(int16_t x, int16_t y, int16_t r, Color c);

/* Ширина текста в пикселях */
int16_t TextWidth(const char *s, Font f);

void Printf(uint8_t x, uint16_t y, const char *fmt, ...);
void PrintfEx(uint8_t x, uint16_t y, TextPos align, Color col, Color bg,
              Font font, const char *fmt, ...);

/* Версия с TextStyle — 4 параметра вместо 7 */
void PrintfT(uint8_t x, uint16_t y, TextStyle st, const char *fmt, ...);

#endif
