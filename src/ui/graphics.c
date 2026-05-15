#include "graphics.h"
#include "driver/st7789.h"
#include "gfxfont.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool gRedrawScreen;
bool gInverted;

#define FB_W ST7789_WIDTH
#define FB_H ST7789_HEIGHT

/* Прямая запись пикселя на дисплей (однопиксельное окно) */
static int16_t clip_x(int16_t x) {
  if (x < 0)
    return 0;
  if (x >= FB_W)
    return FB_W - 1;
  return x;
}
static int16_t clip_y(int16_t y) {
  if (y < 0)
    return 0;
  if (y >= FB_H)
    return FB_H - 1;
  return y;
}

/* Прямая запись пикселя на дисплей (однопиксельное окно) */
void PutPixel(int16_t x, int16_t y, Color c) {
  x = clip_x(x);
  y = clip_y(y);
  st7789_set_addr_window(x, y, 1, 1);
  st7789_cs_low();
  st7789_start_pixels();
  st7789_write_data16(c);
  st7789_cs_high();
}

void UI_ClearScreen(Color bg) { st7789_flush(bg); }

void DrawVLine(int16_t x, int16_t y, int16_t h, Color c) {
  if (x < 0 || x >= FB_W || h <= 0)
    return;
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (y + h > FB_H)
    h = FB_H - y;
  if (h <= 0)
    return;

  st7789_fill_rect_dma((uint16_t)x, (uint16_t)y, 1, (uint16_t)h, c);
}

void DrawHLine(int16_t x, int16_t y, int16_t w, Color c) {
  if (y < 0 || y >= FB_H || w <= 0)
    return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (x + w > FB_W)
    w = FB_W - x;
  if (w <= 0)
    return;

  st7789_fill_rect_dma((uint16_t)x, (uint16_t)y, (uint16_t)w, 1, c);
}

void FillRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > FB_W)
    w = FB_W - x;
  if (y + h > FB_H)
    h = FB_H - y;
  if (w <= 0 || h <= 0)
    return;

  st7789_fill_rect_dma((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, c);
}

static void draw_aline(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                       Color c) {
  int16_t s = abs(y1 - y0) > abs(x1 - x0);
  if (s) {
    int16_t t = x0;
    x0 = y0;
    y0 = t;
    t = x1;
    x1 = y1;
    y1 = t;
  }
  if (x0 > x1) {
    int16_t t = x0;
    x0 = x1;
    x1 = t;
    t = y0;
    y0 = y1;
    y1 = t;
  }
  int16_t dx = x1 - x0, dy = abs(y1 - y0), e = dx >> 1;
  int16_t ys = y0 < y1 ? 1 : -1;
  for (; x0 <= x1; x0++, e -= dy) {
    PutPixel(s ? y0 : x0, s ? x0 : y0, c);
    if (e < 0) {
      y0 += ys;
      e += dx;
    }
  }
}

void DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c) {
  if (x0 == x1) {
    if (y0 > y1) {
      int16_t t = y0;
      y0 = y1;
      y1 = t;
    }
    DrawVLine(x0, y0, y1 - y0 + 1, c);
  } else if (y0 == y1) {
    if (x0 > x1) {
      int16_t t = x0;
      x0 = x1;
      x1 = t;
    }
    DrawHLine(x0, y0, x1 - x0 + 1, c);
  } else {
    draw_aline(x0, y0, x1, y1, c);
  }
}

void DrawRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c) {
  DrawHLine(x, y, w, c);
  DrawHLine(x, y + h - 1, w, c);
  DrawVLine(x, y, h, c);
  DrawVLine(x + w - 1, y, h, c);
}

void FillCircle(int16_t x0, int16_t y0, int16_t r, Color c) {
  int16_t x = 0, y = r, d = 3 - 2 * r;
  while (y >= x) {
    int16_t w = 2 * x + 1;
    DrawHLine(x0 - x, y0 - y, w, c);
    DrawHLine(x0 - x, y0 + y, w, c);
    w = 2 * y + 1;
    DrawHLine(x0 - y, y0 - x, w, c);
    DrawHLine(x0 - y, y0 + x, w, c);
    x++;
    if (d > 0) {
      y--;
      d = d + 4 * (x - y) + 10;
    } else
      d = d + 4 * x + 6;
  }
}

/* ─── Font rendering ─── */
// graphics.c — замена putchar

// Буфер одной строки символа — макс. ширина глифа ~20px
static uint16_t glyph_line_buf[32];

static void putchar_(int16_t *cx, int16_t y, uint8_t c, Color col, Color bg,
                    const GFXfont *f) {
  if (c < f->first || c > f->last)
    return;
  const GFXglyph *g = &f->glyph[c - f->first];
  const uint8_t *b = f->bitmap + g->bitmapOffset;
  uint8_t w = g->width, h = g->height;
  int8_t xo = g->xOffset, yo = g->yOffset;

  // Рисуем построчно: для каждой строки глифа — один DMA-вызов
  uint8_t bits = 0, bit = 0;
  for (uint8_t yy = 0; yy < h; yy++) {
    int16_t screen_y = y + yo + yy;
    if (screen_y < 0 || screen_y >= FB_H) {
      // Всё равно читаем биты чтобы не сломать указатель b
      for (uint8_t xx = 0; xx < w; xx++, bits <<= 1)
        if (!(bit++ & 7))
          bits = *b++;
      continue;
    }
    int16_t screen_x = *cx + xo;
    // Заполняем буфер строки
    for (uint8_t xx = 0; xx < w; xx++, bits <<= 1) {
      if (!(bit++ & 7))
        bits = *b++;
      glyph_line_buf[xx] = (bits & 0x80) ? col : bg;
    }
    // Один setAddrWindow + DMA на всю строку глифа
    if (screen_x >= 0 && screen_x + w <= FB_W) {
      st7789_cs_low();
      st7789_set_addr_window_raw(screen_x, screen_y, w, 1);
      st7789_write_pixels_dma(glyph_line_buf, w);
      st7789_cs_high();
    }
  }
  *cx += g->xAdvance;
}

/* ─── Font rendering ─── */

// ping-pong буферы строки текста (big-endian для SPI)
static uint8_t txt_buf[2][ST7789_WIDTH * 2];

// Заполнить одну строку буфера: bg-заливка + пиксели глифов для screen_row
static void txt_fill_row(uint8_t *buf, int16_t screen_row, const char *s,
                         int16_t sx, int16_t baseline, Color col, Color bg,
                         const GFXfont *f, int16_t x0, int16_t span) {
  uint8_t bgh = bg >> 8, bgl = bg;
  uint8_t clh = col >> 8, cll = col;
  for (int16_t i = 0; i < span; i++) {
    buf[i * 2] = bgh;
    buf[i * 2 + 1] = bgl;
  }
  int16_t cx = sx;
  for (const char *p = s; *p; p++) {
    uint8_t c = (uint8_t)*p;
    if (c < f->first || c > f->last) {
      cx += 0;
      continue;
    }
    const GFXglyph *g = &f->glyph[c - f->first];
    int16_t gy = screen_row - (baseline + g->yOffset);
    if (gy >= 0 && gy < g->height) {
      uint32_t bit0 = (uint32_t)gy * g->width;
      const uint8_t *b = f->bitmap + g->bitmapOffset + bit0 / 8;
      uint8_t bits = *b << (bit0 % 8);
      uint8_t bc = 8 - (bit0 % 8);
      int16_t px = cx + g->xOffset - x0;
      for (uint8_t xx = 0; xx < g->width; xx++, bits <<= 1, bc--) {
        if (!bc) {
          bits = *++b;
          bc = 8;
        }
        if ((bits & 0x80) && px >= 0 && px < span) {
          buf[px * 2] = clh;
          buf[px * 2 + 1] = cll;
        }
        px++;
      }
    }
    cx += g->xAdvance;
  }
}

static int16_t text_width(const char *s, const GFXfont *f) {
  int16_t x = 0;
  for (; *s; s++) {
    if (*s < f->first || *s > f->last)
      continue;
    x += f->glyph[*s - f->first].xAdvance;
  }
  return x;
}

int16_t TextWidth(const char *s, const GFXfont *f) { return text_width(s, f); }

static void printfEx(uint8_t x, uint16_t y, TextPos align, Color col, Color bg,
                       const GFXfont *f, const char *fmt, va_list a) {
  if (gInverted) {
    Color tmp = col;
    col = bg;
    bg = tmp;
  }

  char s[64];
  vsnprintf(s, sizeof(s), fmt, a);

  int16_t sx = x;
  if (align == POS_C)
    sx = x - (text_width(s, f) >> 1);
  else if (align == POS_R)
    sx = x - text_width(s, f);

  // Bounding box по Y
  int16_t y_min = INT16_MAX, y_max = INT16_MIN;
  for (char *p = s; *p; p++) {
    uint8_t c = (uint8_t)*p;
    if (c < f->first || c > f->last)
      continue;
    const GFXglyph *g = &f->glyph[c - f->first];
    int16_t yt = y + g->yOffset;
    int16_t yb = yt + g->height - 1;
    if (yt < y_min)
      y_min = yt;
    if (yb > y_max)
      y_max = yb;
  }
  if (y_min > y_max)
    return;
  if (y_min < 0)
    y_min = 0;
  if (y_max >= FB_H)
    y_max = FB_H - 1;

  // Bounding box по X
  int16_t x0 = sx, x1 = sx + text_width(s, f);
  if (x0 < 0)
    x0 = 0;
  if (x1 > FB_W)
    x1 = FB_W;
  if (x0 >= x1)
    return;
  int16_t span = x1 - x0;

  st7789_cs_low();
  st7789_set_addr_window_raw(x0, y_min, span, y_max - y_min + 1);

  uint8_t cur = 0;
  txt_fill_row(txt_buf[cur], y_min, s, sx, y, col, bg, f, x0, span);
  st7789_dma_start(txt_buf[cur], span * 2);

  for (int16_t row = y_min + 1; row <= y_max; row++) {
    cur ^= 1;
    txt_fill_row(txt_buf[cur], row, s, sx, y, col, bg, f, x0, span);
    st7789_dma_wait();
    st7789_dma_start(txt_buf[cur], span * 2);
  }

  st7789_dma_wait();
  st7789_cs_high();
}

void Printf(uint8_t x, uint16_t y, const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  printfEx(x, y, POS_L, C_WHITE, C_BLACK, F_NORM, fmt, a);
  va_end(a);
}
void PrintfEx(uint8_t x, uint16_t y, TextPos align, Color col, Color bg,
               const GFXfont *f, const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  printfEx(x, y, align, col, bg, f, fmt, a);
  va_end(a);
}

void PrintfT(uint8_t x, uint16_t y, TextStyle st, const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  printfEx(x, y, st.align, st.fg, st.bg, st.font, fmt, a);
  va_end(a);
}
