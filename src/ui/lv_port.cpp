/* Порт LVGL 8.3 на ST7789 (SPI + DMA) для RT-880.
 *
 * LV_COLOR_16_SWAP=1: lv_color_t в памяти уже big-endian (как ждёт ST7789),
 * поэтому flush — это нулевое копирование: окно + DMA напрямую из буфера LVGL.
 *
 * Спектр/водопад по-прежнему рисуют себя напрямую через st7789_* (DMA),
 * минуя LVGL — они живут ниже зоны текстов и не пересекаются с его flush.
 */
#include "lv_port.h"
#include "../driver/st7789.h"
#include "lvgl.h"
#include "screen_layout.h"
#include <string.h>

extern void Graphics_MarkLvglReady(void);

/* Один буфер на 10 строк (240×10×2 = 4800 байт): экономим 4.8К RAM,
   CPU чуть ждёт DMA в конце каждого чанка. */
#define LV_BUF_H 14
#define LV_BUF_PX (ST7789_WIDTH * LV_BUF_H)

static lv_color_t draw_buf1[LV_BUF_PX];
static lv_disp_draw_buf_t disp_buf;
static lv_disp_drv_t disp_drv;
static bool s_directAreas;

void lv_port_set_direct_areas(bool on) { s_directAreas = on; }

static void flush_rows(const lv_area_t *area, const lv_color_t *color_p,
                       int32_t y1, int32_t y2) {
  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = y2 - y1 + 1;
  if (w <= 0 || h <= 0)
    return;

  const lv_color_t *src = color_p + (y1 - area->y1) * w;

  st7789_cs_low();
  st7789_set_addr_window_raw((uint16_t)area->x1, (uint16_t)y1, (uint16_t)w,
                             (uint16_t)h);
  st7789_dma_start((const uint8_t *)src, (uint16_t)(w * h * 2));
  st7789_dma_wait();
  st7789_cs_high();
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                     lv_color_t *color_p) {
  if (!s_directAreas) {
    flush_rows(area, color_p, area->y1, area->y2);
    lv_disp_flush_ready(drv);
    return;
  }

  /* В сканере/анализаторе спектр и водопад рисуются напрямую в ST7789.
   * Пропускаем только разрешённые полосы: над спектром и между спектром и
   * водопадом. Иначе непрозрачный фон лейбла затирает спектр, а пиксели,
   * попавшие в память аппаратного vscroll, ползут вниз чёрными
   * прямоугольниками. */
  int32_t y2 = area->y2 < UI_SPECTRUM_Y - 1 ? area->y2 : UI_SPECTRUM_Y - 1;
  if (area->y1 <= y2)
    flush_rows(area, color_p, area->y1, y2);

  /* Пропускаем спектр и полосу рисок под ним: риски рисуются прямым DMA и
   * не должны затираться чанками LVGL (буфер 14 строк задевает 100..102). */
  int32_t y1 = area->y1 > UI_TICKS_Y + UI_TICKS_H
                   ? area->y1
                   : UI_TICKS_Y + UI_TICKS_H;
  /* 2 px запаса до зоны аппаратного скролла: граница TFA не должна
   * попадать в LVGL-flush. */
  const int32_t clip_bottom = UI_WATERFALL_Y - 3;
  y2 = area->y2 < clip_bottom ? area->y2 : clip_bottom;
  if (y1 <= y2)
    flush_rows(area, color_p, y1, y2);

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
