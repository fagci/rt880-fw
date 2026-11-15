/* Статуслайн — нативные LVGL-виджеты, создаются один раз и живут
 * поверх всех режимов. Обновляются только при изменении значений.
 *
 * Слева: индикатор фильтров (HF/VHF/UHF/800, активные — цветом).
 * Справа: батарея (рамка + заливка + напряжение).
 */
#include "statusline.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../helper/measurements.h"
#include "graphics.h"
#include "lvgl.h"
#include "lvgl_fonts/lv_font_tomthumb.h"
#include "printf.h"
#include <string.h>

#define BAT_X (LCD_WIDTH - 46)
#define BAT_Y 4
#define BAT_W 42
#define BAT_H 16
#define BAT_MIN 600
#define BAT_MAX 840

static lv_obj_t *s_root;
static lv_obj_t *s_divider;
static lv_obj_t *s_batFrame;
static lv_obj_t *s_batFill;
static lv_obj_t *s_batText;
static lv_obj_t *s_fltLbl;
static uint16_t s_prevBatDraw;
static uint16_t s_prevFltDraw = 0xFFFF;

static inline lv_color_t lvc(Color c) {
  return lv_color_make((uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC),
                       (uint8_t)(c << 3));
}

static lv_obj_t *mkBox(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, Color bg, lv_opa_t opa) {
  lv_obj_t *o = lv_obj_create(parent);
  if (!o)
    return NULL;
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(o, lvc(bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN);
  return o;
}

static void build(void) {
  s_root = mkBox(lv_scr_act(), 0, 0, LCD_WIDTH, 24, C_BLACK, LV_OPA_TRANSP);
  if (!s_root)
    return;
  s_divider = mkBox(s_root, 0, 23, LCD_WIDTH, 1, C_GRAY, LV_OPA_COVER);

  s_batFrame = mkBox(s_root, BAT_X, BAT_Y, BAT_W, BAT_H, C_BLACK,
                     LV_OPA_TRANSP);
  if (s_batFrame) {
    lv_obj_set_style_border_width(s_batFrame, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_batFrame, lvc(C_GRAY), LV_PART_MAIN);
    lv_obj_set_style_border_side(s_batFrame, LV_BORDER_SIDE_FULL,
                                 LV_PART_MAIN);
  }
  s_batFill = mkBox(s_root, BAT_X + 2, BAT_Y + 2, 0, BAT_H - 4, C_GREEN,
                    LV_OPA_COVER);

  s_batText = lv_label_create(s_root);
  if (s_batText) {
    lv_obj_set_pos(s_batText, BAT_X, BAT_Y + 5);
    lv_obj_set_width(s_batText, BAT_W);
    lv_label_set_long_mode(s_batText, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_batText, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_batText, &lv_font_tomthumb, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_batText, lvc(C_WHITE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_batText, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_batText, 0, LV_PART_MAIN);
  }

  s_fltLbl = lv_label_create(s_root);
  if (s_fltLbl) {
    lv_obj_set_pos(s_fltLbl, 4, 5);
    lv_obj_set_width(s_fltLbl, BAT_X - 12);
    lv_label_set_long_mode(s_fltLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_fltLbl, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_fltLbl, &lv_font_tomthumb, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_fltLbl, lvc(C_GRAY), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_fltLbl, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_fltLbl, 0, LV_PART_MAIN);
    lv_label_set_recolor(s_fltLbl, true);
  }
}

void STATUSLINE_render(void) {
  if (!s_root)
    build();
  if (!s_root)
    return;

  uint16_t flt = BK4819_GetFilter();
  if (flt != s_prevFltDraw && s_fltLbl) {
    s_prevFltDraw = flt;
    static char buf[56];
    const struct {
      uint16_t mask;
      const char *name;
    } F[] = {{FILTER_HF, "HF"},
             {FILTER_VHF, "VHF"},
             {FILTER_UHF, "UHF"},
             {FILTER_800, "800"}};
    buf[0] = '\0';
    for (uint8_t i = 0; i < 4; i++) {
      if (i)
        strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
      if (flt & F[i].mask)
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "#41CA41 %s#",
                 F[i].name);
      else
        strncat(buf, F[i].name, sizeof(buf) - strlen(buf) - 1);
    }
    lv_label_set_text_static(s_fltLbl, buf);
  }

  uint16_t bat = battery_voltage_cv();
  /* гистерезис 2 cV: без него ADC-шум дёргает перерисовку каждый кадр */
  if (s_prevBatDraw && bat >= s_prevBatDraw - 1 && bat <= s_prevBatDraw + 1)
    return;
  s_prevBatDraw = bat;

  uint8_t fill =
      (uint8_t)ConvertDomain((int)bat, BAT_MIN, BAT_MAX, 0, BAT_W - 4);
  Color fcol;
  if (bat >= 740)
    fcol = C_GREEN;
  else if (bat >= 680)
    fcol = C_YELLOW;
  else
    fcol = C_RED;

  if (s_batFill) {
    lv_obj_set_size(s_batFill, fill, BAT_H - 4);
    lv_obj_set_style_bg_color(s_batFill, lvc(fcol), LV_PART_MAIN);
  }
  if (s_batText) {
    static char t[8];
    snprintf(t, sizeof(t), "%u.%02u", bat / 100, bat % 100);
    lv_label_set_text_static(s_batText, t);
  }
}

void STATUSLINE_SetText(const char *pattern, ...) {
  (void)pattern;
}
