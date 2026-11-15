#ifndef LV_FONT_TOMTHUMB_H
#define LV_FONT_TOMTHUMB_H

/* Forward declaration — нельзя включать lvgl.h из заголовков шрифтов:
   lv_conf.h подключается внутри lvgl.h, получилась бы циклическая зависимость. */
#ifdef __cplusplus
extern "C" {
#endif

struct _lv_font_t;
typedef struct _lv_font_t lv_font_t;

extern const lv_font_t lv_font_tomthumb;

#ifdef __cplusplus
}
#endif

#endif
