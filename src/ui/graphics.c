#include "graphics.h"
#include "driver/st7789.h"
#include "fonts/TomThumb.h"
#include <string.h>
#include <stdlib.h>

#define FB_W ST7789_WIDTH
#define FB_H ST7789_HEIGHT

/* Строчный буфер для ClearScreen / FillRect */
static uint16_t line_buf[FB_W];

static int16_t clip_x(int16_t x)
{
    if (x < 0) return 0;
    if (x >= FB_W) return FB_W - 1;
    return x;
}
static int16_t clip_y(int16_t y)
{
    if (y < 0) return 0;
    if (y >= FB_H) return FB_H - 1;
    return y;
}

/* Прямая запись пикселя на дисплей (однопиксельное окно) */
void PutPixel(int16_t x, int16_t y, Color c)
{
    x = clip_x(x); y = clip_y(y);
    st7789_set_addr_window(x, y, 1, 1);
    st7789_start_pixels();
    st7789_write_data16(c);
    st7789_end_pixels();
}

void FB_Clear(Color c)
{
    for (int i = 0; i < FB_W; i++)
        line_buf[i] = c;
}

void FB_Flush(void)
{
    static uint16_t prev[FB_W];
    static int dirty = 1;
    for (int i = 0; i < FB_W; i++)
        if (line_buf[i] != prev[i]) { dirty = 1; break; }
    if (!dirty) return;
    memcpy(prev, line_buf, sizeof(line_buf));
    st7789_set_addr_window(0, 0, FB_W, 1);
    st7789_start_pixels();
    for (int i = 0; i < FB_W; i++)
        st7789_write_data16(line_buf[i]);
    st7789_end_pixels();
}

static void fb_flush_full(void)
{
    for (int y = 0; y < FB_H; y++) {
        st7789_set_addr_window(0, y, FB_W, 1);
        st7789_start_pixels();
        for (int x = 0; x < FB_W; x++)
            st7789_write_data16(line_buf[x]);
        st7789_end_pixels();
    }
}

void UI_ClearScreen(Color bg)
{
    FB_Clear(bg);
    fb_flush_full();
}

void DrawHLine(int16_t x, int16_t y, int16_t w, Color c)
{
    if (y < 0 || y >= FB_H || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (w <= 0) return;
    st7789_set_addr_window(x, y, w, 1);
    st7789_start_pixels();
    for (int i = 0; i < w; i++) st7789_write_data16(c);
    st7789_end_pixels();
}

void DrawVLine(int16_t x, int16_t y, int16_t h, Color c)
{
    if (x < 0 || x >= FB_W || h <= 0) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > FB_H) h = FB_H - y;
    if (h <= 0) return;
    st7789_set_addr_window(x, y, 1, h);
    st7789_start_pixels();
    for (int i = 0; i < h; i++) st7789_write_data16(c);
    st7789_end_pixels();
}

static void draw_aline(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c)
{
    int16_t s = abs(y1 - y0) > abs(x1 - x0);
    if (s) {
        int16_t t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1) {
        int16_t t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    int16_t dx = x1 - x0, dy = abs(y1 - y0), e = dx >> 1;
    int16_t ys = y0 < y1 ? 1 : -1;
    for (; x0 <= x1; x0++, e -= dy) {
        PutPixel(s ? y0 : x0, s ? x0 : y0, c);
        if (e < 0) { y0 += ys; e += dx; }
    }
}

void DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c)
{
    if (x0 == x1) {
        if (y0 > y1) { int16_t t = y0; y0 = y1; y1 = t; }
        DrawVLine(x0, y0, y1 - y0 + 1, c);
    } else if (y0 == y1) {
        if (x0 > x1) { int16_t t = x0; x0 = x1; x1 = t; }
        DrawHLine(x0, y0, x1 - x0 + 1, c);
    } else {
        draw_aline(x0, y0, x1, y1, c);
    }
}

void DrawRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c)
{
    DrawHLine(x, y, w, c);
    DrawHLine(x, y + h - 1, w, c);
    DrawVLine(x, y, h, c);
    DrawVLine(x + w - 1, y, h, c);
}

void FillRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W)  w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    if (w <= 0 || h <= 0) return;
    st7789_set_addr_window(x, y, w, h);
    st7789_start_pixels();
    uint32_t n = (uint32_t)w * h;
    for (uint32_t i = 0; i < n; i++) st7789_write_data16(c);
    st7789_end_pixels();
}

void FillCircle(int16_t x0, int16_t y0, int16_t r, Color c)
{
    int16_t x = 0, y = r, d = 3 - 2 * r;
    while (y >= x) {
        int16_t w = 2 * x + 1;
        DrawHLine(x0 - x, y0 - y, w, c);
        DrawHLine(x0 - x, y0 + y, w, c);
        w = 2 * y + 1;
        DrawHLine(x0 - y, y0 - x, w, c);
        DrawHLine(x0 - y, y0 + x, w, c);
        x++;
        if (d > 0) { y--; d = d + 4 * (x - y) + 10; }
        else d = d + 4 * x + 6;
    }
}

/* ─── Font rendering ─── */
static void putchar(int16_t x, int16_t y, uint8_t c, Color col, const GFXfont *f)
{
    if (c < f->first || c > f->last) return;
    const GFXglyph *g = &f->glyph[c - f->first];
    const uint8_t *b = f->bitmap + g->bitmapOffset;
    uint8_t w = g->width, h = g->height, bits = 0, bit = 0;
    int8_t xo = g->xOffset, yo = g->yOffset;

    for (uint8_t yy = 0; yy < h; yy++) {
        for (uint8_t xx = 0; xx < w; xx++, bits <<= 1) {
            if (!(bit++ & 7)) bits = *b++;
            if (bits & 0x80)
                PutPixel(x + xo + xx, y + yo + yy, col);
        }
    }
}

static int16_t text_width(const char *s, const GFXfont *f)
{
    int16_t x = 0;
    for (; *s; s++) {
        if (*s < f->first || *s > f->last) continue;
        x += f->glyph[*s - f->first].xAdvance;
    }
    return x;
}

void Printf(uint8_t x, uint8_t y, const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    PrintfEx(x, y, POS_L, C_WHITE, fmt);
    va_end(a);
}

void PrintfEx(uint8_t x, uint8_t y, TextPos align, Color c, const char *fmt, ...)
{
    char buf[64];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);

    const GFXfont *f = &TomThumb;
    int16_t sx = x;

    if (align == POS_C) {
        sx = x - (text_width(buf, f) >> 1);
    } else if (align == POS_R) {
        sx = x - text_width(buf, f);
    }

    for (char *p = buf; *p; p++)
        putchar(sx, y, *p, c, f);
}
