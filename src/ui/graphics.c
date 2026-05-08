#include "graphics.h"
#include "driver/st7789.h"
#include "fonts/TomThumb.h"
#include <string.h>
#include <stdlib.h>

/* Строчный буфер — 240 пикселей * 2 байта = 480 байт */
static uint16_t line_buf[ST7789_WIDTH];
static uint16_t fg_color = C_WHITE;
static uint16_t bg_color = C_BLACK;

/* === Pixel buffer helpers === */
static int16_t clip_x(int16_t x)
{
    if (x < 0) return 0;
    if (x >= ST7789_WIDTH) return ST7789_WIDTH - 1;
    return x;
}
static int16_t clip_y(int16_t y)
{
    if (y < 0) return 0;
    if (y >= ST7789_HEIGHT) return ST7789_HEIGHT - 1;
    return y;
}

void PutPixel(int16_t x, int16_t y, Color c)
{
    x = clip_x(x); y = clip_y(y);
    line_buf[x] = c;
}

void FB_Clear(Color c)
{
    for (int i = 0; i < ST7789_WIDTH; i++)
        line_buf[i] = c;
}

void FB_Flush(void)
{
    static uint16_t prev[ST7789_WIDTH];
    static int dirty = 1;
    for (int i = 0; i < ST7789_WIDTH; i++)
        if (line_buf[i] != prev[i]) { dirty = 1; break; }
    if (!dirty) return;
    memcpy(prev, line_buf, sizeof(line_buf));
    st7789_write_cmd(0x2A); /* CASET */
    st7789_write_data16(0); st7789_write_data16(ST7789_WIDTH - 1);
    st7789_write_cmd(0x2B); /* RASET */
    st7789_write_data16(0); st7789_write_data16(0);
    st7789_write_cmd(0x2C); /* RAMWR */
    st7789_start_pixels();
    for (int i = 0; i < ST7789_WIDTH; i++) {
        st7789_write_data16(line_buf[i]);
    }
    st7789_end_pixels();
}

/* Полный рефреш — заливка всего экрана построчно */
static void fb_flush_full(void)
{
    for (int y = 0; y < ST7789_HEIGHT; y++) {
        st7789_set_addr_window(0, y, ST7789_WIDTH, 1);
        st7789_start_pixels();
        for (int x = 0; x < ST7789_WIDTH; x++)
            st7789_write_data16(line_buf[x]);
        st7789_end_pixels();
    }
}

void UI_ClearScreen(Color bg)
{
    bg_color = bg;
    FB_Clear(bg);
    fb_flush_full();
}

/* === Drawing primitives === */
void DrawHLine(int16_t x, int16_t y, int16_t w, Color c)
{
    if (y < 0 || y >= ST7789_HEIGHT || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > ST7789_WIDTH) w = ST7789_WIDTH - x;
    if (w <= 0) return;
    st7789_set_addr_window(x, y, w, 1);
    st7789_start_pixels();
    for (int i = 0; i < w; i++) st7789_write_data16(c);
    st7789_end_pixels();
}

void DrawVLine(int16_t x, int16_t y, int16_t h, Color c)
{
    if (x < 0 || x >= ST7789_WIDTH || h <= 0) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > ST7789_HEIGHT) h = ST7789_HEIGHT - y;
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
    if (x + w > ST7789_WIDTH)  w = ST7789_WIDTH - x;
    if (y + h > ST7789_HEIGHT) h = ST7789_HEIGHT - y;
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
        DrawHLine(x0 - x, y0 - y, 2 * x + 1, c);
        DrawHLine(x0 - x, y0 + y, 2 * x + 1, c);
        DrawHLine(x0 - y, y0 - x, 2 * y + 1, c);
        DrawHLine(x0 - y, y0 + x, 2 * y + 1, c);
        x++;
        if (d > 0) { y--; d = d + 4 * (x - y) + 10; }
        else d = d + 4 * x + 6;
    }
}

/* === Font drawing === */
static void putchar(int16_t x, int16_t y, uint8_t c, Color col, const GFXfont *f)
{
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

static void get_text_bounds(const char *s, int16_t x, int16_t y,
                            int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h,
                            const GFXfont *f)
{
    int16_t minx = 0x7FFF, miny = 0x7FFF, maxx = -1, maxy = -1;
    for (; *s; s++) {
        if (*s < f->first || *s > f->last) continue;
        const GFXglyph *g = &f->glyph[*s - f->first];
        int16_t x2 = x + g->xOffset + g->width - 1;
        int16_t y2 = y + g->yOffset + g->height - 1;
        if (x < minx) minx = x;
        if (y < miny) miny = y;
        if (x2 > maxx) maxx = x2;
        if (y2 > maxy) maxy = y2;
        x += g->xAdvance;
    }
    *x1 = maxx >= minx ? minx : x;
    *y1 = maxy >= miny ? miny : y;
    *w  = maxx >= minx ? maxx - minx + 1 : 0;
    *h  = maxy >= miny ? maxy - miny + 1 : 0;
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
    int16_t bx, by;
    uint16_t bw, bh;

    if (align == POS_L) {
        for (char *p = buf; *p; p++)
            putchar(x, y, *p, c, f);
    } else {
        get_text_bounds(buf, x, y, &bx, &by, &bw, &bh, f);
        int16_t sx = (align == POS_C) ? x - (bw >> 1) : x - bw;
        for (char *p = buf; *p; p++)
            putchar(sx, y, *p, c, f);
    }
}
