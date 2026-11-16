/* Порт LVGL 8.3 на ST7789 (SPI + DMA) для RT-880.
 *
 * LV_COLOR_16_SWAP=1: lv_color_t в памяти уже big-endian (как ждёт ST7789),
 * поэтому flush — это нулевое копирование: окно + DMA напрямую из буфера LVGL.
 *
 * Спектр/водопад по-прежнему рисуют себя напрямую через st7789_* (DMA),
 * минуя LVGL — они живут ниже зоны текстов и не пересекаются с его flush.
 */
#include "lv_port.h"
#include "../driver/board.h"
#include "../driver/st7789.h"
#include "lvgl.h"
#include <string.h>

extern void Graphics_MarkLvglReady(void);

/* Один буфер на 10 строк (240×10×2 = 4800 байт): экономим 4.8К RAM,
   CPU чуть ждёт DMA в конце каждого чанка. */
#define LV_BUF_H 14
#define LV_BUF_PX (ST7789_WIDTH * LV_BUF_H)

static lv_color_t draw_buf1[LV_BUF_PX];
static lv_disp_draw_buf_t disp_buf;
static lv_disp_drv_t disp_drv;

/* Статистика flush: для профилирования (кадр >600 мс — это рендер или SPI?) */
static uint32_t s_stCalls, s_stPx, s_stMs;

void lv_port_stats(uint32_t *calls, uint32_t *px, uint32_t *flushMs) {
  *calls = s_stCalls;
  *px = s_stPx;
  *flushMs = s_stMs;
  s_stCalls = s_stPx = s_stMs = 0;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                     lv_color_t *color_p) {
  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = area->y2 - area->y1 + 1;
  uint32_t t0 = millis();

  st7789_cs_low();
  st7789_set_addr_window_raw((uint16_t)area->x1, (uint16_t)area->y1,
                             (uint16_t)w, (uint16_t)h);
  st7789_dma_start((const uint8_t *)color_p, (uint16_t)(w * h * 2));
  st7789_dma_wait();
  st7789_cs_high();

  s_stCalls++;
  s_stPx += (uint32_t)(w * h);
  s_stMs += millis() - t0;

  lv_disp_flush_ready(drv);
}

void lv_port_init(void) {
  lv_init();

  lv_disp_draw_buf_init(&disp_buf, draw_buf1, NULL, LV_BUF_PX);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = ST7789_WIDTH;
  disp_drv.ver_res = ST7789_HEIGHT;
  disp_drv.flush_cb = flush_cb;
  disp_drv.draw_buf = &disp_buf;
  lv_disp_drv_register(&disp_drv);

  /* Экран непрозрачный чёрный — подложка под прямые DMA-заливки. */
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

  Graphics_MarkLvglReady();
}
