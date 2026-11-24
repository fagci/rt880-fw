#ifndef LV_HELPERS_H
#define LV_HELPERS_H

#include "graphics.h"
#include "lvgl.h"

/* Цвет с учётом глобальной темы: TC() + упаковка в lv_color_t. */
lv_color_t lvColor(Color c);

lv_obj_t *lvBox(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                lv_coord_t h, Color bg, lv_opa_t opa, lv_coord_t radius = 0);

lv_obj_t *lvLabel(lv_obj_t *parent, Font font, Color fg, lv_coord_t x,
                  lv_coord_t y, lv_coord_t w, lv_text_align_t align);

/* Моноширинный текст не выходит за bbox: гасим штатный ext_draw_size
 * (font_h/4), иначе соседние строки перекрываются и LVGL склеивает их
 * области инвалидации в перерисовку целого блока. */
void lvLabelNoExt(lv_obj_t *label);

#endif
