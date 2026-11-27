#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST7789_WIDTH 240
#define ST7789_HEIGHT 320

#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
/* Пастельная палитра интерфейса: на чёрном фоне не режет глаз, в светлой
 * теме TC() переводит её в тёмные варианты. */
#define C_RED RGB565(0xF2, 0x8B, 0x82)
#define C_GREEN RGB565(0x8C, 0xE9, 0x9A)
#define C_BLUE RGB565(0x8A, 0xB4, 0xF8)
#define C_YELLOW RGB565(0xF5, 0xE6, 0xA3)
#define C_CYAN RGB565(0x9A, 0xDC, 0xE8)
#define C_MAGENTA RGB565(0xE8, 0xA3, 0xD4)
#define C_GRAY RGB565(0x9A, 0xA0, 0xA6)
#define C_DARK 0x000F

void st7789_init(void);
void st7789_reset(void);

/* Гамма ST7789 (GAMSET, 26h): 0 — G2.2, 1 — G1.8, 2 — G2.5, 3 — G1.0. */
void st7789_set_gamma(uint8_t curve);

/* Подсветка LCD: ШИМ 200 Гц на PA4 (TMR14_CH1), 0..ST7789_BACKLIGHT_MAX.
 * Медленный ключ Q500 (390К/1нФ) выше 200 Гц не даёт чистых фронтов. */
#define ST7789_BACKLIGHT_MAX 1000
void st7789_backlight_set(uint16_t duty);
void st7789_backlight_off();
void st7789_backlight_on();
void st7789_set_spi_div(uint8_t div); /* TEMP: тест гармоник */
void st7789_set_mute(bool mute);     /* TEMP: тест гармоник */

void st7789_set_addr_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void st7789_write_cmd(uint8_t cmd);
void st7789_write_data(uint8_t data);
void st7789_write_data16(uint16_t data);
void st7789_cs_low(void);
void st7789_cs_high(void);
void st7789_start_pixels(void);
void st7789_end_pixels(void);

void st7789_dma_start(const uint8_t *buf, uint16_t byte_count);
void st7789_dma_wait(void);

void st7789_flush(uint16_t color);
void st7789_fill_rect_dma(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          uint16_t color);

void st7789_set_addr_window_raw(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void st7789_write_pixels_dma(const uint16_t *pixels, uint32_t count);
void st7789_set_vscroll_area(uint16_t tfa, uint16_t vsa, uint16_t bfa);
void st7789_set_vscroll_start(uint16_t vsp);
void st7789_scroll_reset(void);

#ifdef __cplusplus
}
#endif

#endif
