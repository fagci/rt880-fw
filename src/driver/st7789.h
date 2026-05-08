#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>

#define ST7789_WIDTH 240
#define ST7789_HEIGHT 320

#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED 0xF800
#define C_GREEN 0x07E0
#define C_BLUE 0x001F
#define C_YELLOW 0xFFE0
#define C_CYAN 0x07FF
#define C_MAGENTA 0xF81F
#define C_GRAY 0x8410
#define C_DARK 0x000F

void st7789_init(void);
void st7789_reset(void);

void st7789_backlight_off();
void st7789_backlight_on();

void st7789_set_addr_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void st7789_write_cmd(uint8_t cmd);
void st7789_write_data(uint8_t data);
void st7789_write_data16(uint16_t data);
void st7789_cs_low(void);
void st7789_cs_high(void);
void st7789_start_pixels(void);
void st7789_end_pixels(void);

void st7789_flush(uint16_t color);

#endif
