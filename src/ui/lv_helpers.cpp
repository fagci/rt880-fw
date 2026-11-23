#include "lv_helpers.h"

lv_color_t lvColor(Color c) {
  c = TC(c);
  return lv_color_make((uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC),
                       (uint8_t)(c << 3));
}

lv_obj_t *lvBox(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                lv_coord_t h, Color bg, lv_opa_t opa, lv_coord_t radius) {
  lv_obj_t *o = lv_obj_create(parent);
  if (!o) return NULL; /* пул LVGL исчерпан */
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
  lv_obj_set_style_bg_color(o, lvColor(bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN);
  return o;
}

lv_obj_t *lvLabel(lv_obj_t *parent, Font font, Color fg, lv_coord_t x,
                  lv_coord_t y, lv_coord_t w, lv_text_align_t align) {
  lv_obj_t *l = lv_label_create(parent);
  if (!l) return NULL;
  lv_obj_set_pos(l, x, y);
  if (w) {
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(l, align, LV_PART_MAIN);
  }
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lvColor(fg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_pad_all(l, 0, LV_PART_MAIN);
  return l;
}
