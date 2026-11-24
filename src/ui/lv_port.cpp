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
#include "../driver/uart.h"
#include "lvgl.h"
#include "screen_layout.h"
#include <string.h>

extern void Graphics_MarkLvglReady(void);

/* Один буфер на 10 строк (240×10×2 = 4800 байт): экономим 4.8К RAM,
   CPU чуть ждёт DMA в конце каждого чанка. */
#define LV_BUF_H 10
#define LV_BUF_PX (ST7789_WIDTH * LV_BUF_H)

static lv_color_t draw_buf1[LV_BUF_PX];
static lv_disp_draw_buf_t disp_buf;
static lv_disp_drv_t disp_drv;
static bool s_directAreas;
static bool s_spectrumOverlay;

/* Счётчики для `lv` в консоли. monitor_cb даёт пиксели и время одного
 * refresh (рендер + flush), flush_cb — реально отправленное в панель. */
static struct {
  uint32_t frames;
  uint32_t px;
  uint32_t pxMax;
  uint32_t timeMs;
  uint32_t timeMax;
  uint32_t flushes;
  uint32_t sentPx;
  uint32_t skippedPx;
  uint32_t bandSpec;  /* LVGL-пиксели, попавшие в полосу спектра */
  uint32_t bandTicks; /* ... в полосу рисок */
  uint32_t bandWf;    /* ... в полосу водопада */
  uint32_t sinceMs;
} s_stats;

static void monitor_cb(lv_disp_drv_t *drv, uint32_t time, uint32_t px) {
  (void)drv;
  s_stats.frames++;
  s_stats.px += px;
  if (px > s_stats.pxMax)
    s_stats.pxMax = px;
  s_stats.timeMs += time;
  if (time > s_stats.timeMax)
    s_stats.timeMax = time;
}

static uint32_t band_px(const lv_area_t *area, int32_t y1, int32_t y2) {
  if (y2 < y1 || area->y1 > y2 || area->y2 < y1)
    return 0;
  int32_t a = area->y1 > y1 ? area->y1 : y1;
  int32_t b = area->y2 < y2 ? area->y2 : y2;
  return (uint32_t)(area->x2 - area->x1 + 1) * (uint32_t)(b - a + 1);
}

static void flush_done(lv_disp_drv_t *drv, const lv_area_t *area,
                       uint32_t sentBefore) {
  uint32_t areaPx = (uint32_t)(area->x2 - area->x1 + 1) *
                    (uint32_t)(area->y2 - area->y1 + 1);
  uint32_t sent = s_stats.sentPx - sentBefore;
  s_stats.flushes++;
  if (areaPx > sent)
    s_stats.skippedPx += areaPx - sent;
  s_stats.bandSpec += band_px(area, UI_SPECTRUM_Y, UI_SPECTRUM_Y + UI_SPECTRUM_H - 1);
  s_stats.bandTicks += band_px(area, UI_TICKS_Y, UI_TICKS_Y + UI_TICKS_H - 1);
  s_stats.bandWf += band_px(area, UI_WATERFALL_Y, ST7789_HEIGHT - 1);
  lv_disp_flush_ready(drv);
}

void lv_port_set_direct_areas(bool on) { s_directAreas = on; }
void lv_port_set_spectrum_overlay(bool on) { s_spectrumOverlay = on; }

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
  s_stats.sentPx += (uint32_t)w * h;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                     lv_color_t *color_p) {
  uint32_t sentBefore = s_stats.sentPx;
  if (!s_directAreas) {
    flush_rows(area, color_p, area->y1, area->y2);
    flush_done(drv, area, sentBefore);
    return;
  }

  /* В сканере/анализаторе спектр и водопад рисуются напрямую в ST7789.
   * Пропускаем только разрешённые полосы: над спектром и между спектром и
   * водопадом. Иначе непрозрачный фон лейбла затирает спектр, а пиксели,
   * попавшие в память аппаратного vscroll, ползут вниз чёрными
   * прямоугольниками. */
  if (s_spectrumOverlay) {
    /* Открыто меню поверх спектра: LVGL можно писать до водопада, спектр
     * продолжает рисоваться напрямую, меню перерисовывается поверх своей
     * области. Водопад по-прежнему только DMA. */
    const int32_t clip_bottom = UI_WATERFALL_Y - 3;
    int32_t y2 = area->y2 < clip_bottom ? area->y2 : clip_bottom;
    if (area->y1 <= y2)
      flush_rows(area, color_p, area->y1, y2);
    flush_done(drv, area, sentBefore);
    return;
  }

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

  flush_done(drv, area, sentBefore);
}

void lv_port_init(void) {
  lv_init();

  lv_disp_draw_buf_init(&disp_buf, draw_buf1, NULL, LV_BUF_PX);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = ST7789_WIDTH;
  disp_drv.ver_res = ST7789_HEIGHT;
  disp_drv.flush_cb = flush_cb;
  disp_drv.monitor_cb = monitor_cb;
  disp_drv.draw_buf = &disp_buf;
  lv_disp_drv_register(&disp_drv);

  /* Экран непрозрачный чёрный — подложка под прямые DMA-заливки. */
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

  Graphics_MarkLvglReady();
  lv_port_stats_reset();
}

static void count_tree(lv_obj_t *o, uint16_t *objs, uint16_t *labels) {
  if (!o)
    return;
  (*objs)++;
  if (lv_obj_check_type(o, &lv_label_class))
    (*labels)++;
  uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; i++)
    count_tree(lv_obj_get_child(o, i), objs, labels);
}

void lv_port_stats_reset(void) {
  memset(&s_stats, 0, sizeof(s_stats));
  s_stats.sinceMs = lv_tick_get();
}

void lv_port_stats_print(void) {
  uint32_t ms = lv_tick_get() - s_stats.sinceMs;
  if (!ms)
    ms = 1;
  uint16_t objs = 0, labels = 0;
  count_tree(lv_scr_act(), &objs, &labels);

  lv_mem_monitor_t m;
  lv_mem_monitor(&m);

  uint32_t fr = s_stats.frames;
  uint32_t pxps = (uint32_t)((uint64_t)s_stats.px * 1000u / ms);
  uint32_t frps = (uint32_t)((uint64_t)fr * 1000u / ms);

  uart_printf("LVSTAT ms=%lu fr=%lu fr/s=%lu px=%lu px/fr=%lu pxmax=%lu "
              "t/fr=%lu tmax=%lu fl=%lu sent=%lu clip=%lu bsp=%lu btk=%lu "
              "bwf=%lu objs=%u lbl=%u\r\n",
              (unsigned long)ms, (unsigned long)fr, (unsigned long)frps,
              (unsigned long)s_stats.px,
              (unsigned long)(fr ? s_stats.px / fr : 0),
              (unsigned long)s_stats.pxMax,
              (unsigned long)(fr ? s_stats.timeMs / fr : 0),
              (unsigned long)s_stats.timeMax, (unsigned long)s_stats.flushes,
              (unsigned long)s_stats.sentPx,
              (unsigned long)s_stats.skippedPx,
              (unsigned long)s_stats.bandSpec,
              (unsigned long)s_stats.bandTicks,
              (unsigned long)s_stats.bandWf, (unsigned)objs,
              (unsigned)labels);
  uart_printf("LVMEM tot=%lu free=%lu big=%lu used=%u%% frag=%u%% max=%lu "
              "px/s=%lu B/s=%lu\r\n",
              (unsigned long)m.total_size, (unsigned long)m.free_size,
              (unsigned long)m.free_biggest_size, (unsigned)m.used_pct,
              (unsigned)m.frag_pct,               (unsigned long)m.max_used,
              (unsigned long)pxps, (unsigned long)(pxps * 2u));
}
