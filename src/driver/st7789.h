#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>

#define ST7789_WIDTH  240
#define ST7789_HEIGHT 320

void st7789_init(void);
void st7789_reset(void);
void st7789_flush(uint16_t color);

#endif
