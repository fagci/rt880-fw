#ifndef LV_PORT_H
#define LV_PORT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void lv_port_init(void);

/* Запретить LVGL писать в зоны прямой отрисовки (спектр и водопад).
 * Включается режимами сканера/анализатора: фон лейблов и инвалидация не
 * должны затирать спектр и попадать в память аппаратного скролла. */
void lv_port_set_direct_areas(bool on);

/* Меню-оверлей в зоне спектра: разрешить LVGL писать поверх прямого DMA
 * (спектр и водопад продолжают рисоваться), водопад не трогаем. */
void lv_port_set_spectrum_overlay(bool on);

/* Спектр в режиме direct areas не рисуется (мультискан): LVGL разрешено
 * писать и в зону спектра, чтобы список находок занял её. Сбрасывается
 * в lv_port_set_direct_areas(). */
void lv_port_set_spectrum_hidden(bool on);

/* Последняя строка, которую LVGL может писать в режиме direct areas
 * (по умолчанию — над зоной водопада). Сканер без водопада расширяет её
 * вниз до S-метра: под спектром живут лейблы списка находок. */
void lv_port_set_text_bottom(uint16_t y);

/* Диагностика по UART (команда `lv`): кадры/пиксели/время LVGL и куча. */
void lv_port_stats_reset(void);
void lv_port_stats_print(void);

#ifdef __cplusplus
}
#endif

#endif
