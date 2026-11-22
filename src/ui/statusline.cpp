/* Статуслайн — нативные LVGL-виджеты, создаются один раз и живут
 * поверх всех режимов. Обновляются только при изменении значений.
 *
 * Слева: заголовок режима.
 * Справа: A1/A2 — положение реле антенны (PF0/K200), напряжение
 * и батарея (рамка + заливка).
 */
#include "statusline.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../helper/measurements.h"
#include "../modes/mode.h"
#include "graphics.h"
#include "lvgl.h"
#include "printf.h"
#include "screen_layout.h"
#include <string.h>

#define BAT_W 22 /* корпус батареи */
#define BAT_H 11
#define BAT_X (LCD_WIDTH - 8 - BAT_W - 3)
#define BAT_Y ((UI_STATUS_H - BAT_H) / 2)
#define BAT_MIN 600
#define BAT_MAX 840

static lv_obj_t *s_root;
static lv_obj_t *s_batFrame;
static lv_obj_t *s_batNub;
static lv_obj_t *s_batFill;
static lv_obj_t *s_batText;
static lv_obj_t *s_antText;
static lv_obj_t *s_lockText;
static lv_obj_t *s_title;
static uint16_t s_prevBatDraw;
static bool s_antShown;
static bool s_lockShown;
static const char *s_titleText = "";

static inline lv_color_t lvc(Color c) {
  c = TC(c);
  return lv_color_make((uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC),
                       (uint8_t)(c << 3));
}

static lv_obj_t *mkBox(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, Color bg, lv_opa_t opa,
                       lv_coord_t radius) {
  lv_obj_t *o = lv_obj_create(parent);
  if (!o)
    return NULL;
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
  lv_obj_set_style_bg_color(o, lvc(bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN);
  return o;
}

static lv_obj_t *mkText(lv_coord_t x, lv_coord_t w, lv_text_align_t al,
                        Color c) {
  lv_obj_t *l = lv_label_create(s_root);
  if (!l)
    return NULL;
  lv_obj_set_pos(l, x, (UI_STATUS_H - FontH(F_S)) / 2);
  lv_obj_set_width(l, w);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(l, al, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, F_S, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lvc(c), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_pad_all(l, 0, LV_PART_MAIN);
  return l;
}

static void build(void) {
  s_root =
      mkBox(lv_scr_act(), 0, 0, LCD_WIDTH, UI_STATUS_H, C_BLACK, LV_OPA_TRANSP, 0);
  if (!s_root)
    return;
  /* батарея: контур с носиком, заливка внутри */
  s_batFrame = mkBox(s_root, BAT_X, BAT_Y, BAT_W, BAT_H, C_BLACK,
                     LV_OPA_TRANSP, 3);
  if (s_batFrame) {
    lv_obj_set_style_border_width(s_batFrame, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_batFrame, lvc(C_GRAY), LV_PART_MAIN);
  }
  s_batNub = mkBox(s_root, BAT_X + BAT_W, BAT_Y + 3, 2, 5, C_GRAY,
                   LV_OPA_COVER, 1);
  s_batFill = mkBox(s_root, BAT_X + 2, BAT_Y + 2, 0, BAT_H - 4, C_GREEN,
                    LV_OPA_COVER, 1);
  s_batText = mkText(BAT_X - 8 - 30, 30, LV_TEXT_ALIGN_RIGHT, C_GRAY);
  /* A1 — только второй разъём (PF0 LOW), A2 — добавлена основная. */
  s_antText = mkText(132, 14, LV_TEXT_ALIGN_LEFT, C_GRAY);
  s_antShown = ant_is_main();
  if (s_antText)
    lv_label_set_text_static(s_antText, s_antShown ? "A2" : "A1");
  s_lockText = mkText(70, 30, LV_TEXT_ALIGN_LEFT, C_YELLOW);
  s_lockShown = Mode_KeyLocked();
  if (s_lockText)
    lv_label_set_text_static(s_lockText, s_lockShown ? "LOCK" : " ");
  s_title = mkText(8, 120, LV_TEXT_ALIGN_LEFT, C_GRAY);
  if (s_title)
    lv_label_set_text_static(s_title, s_titleText);
  s_prevBatDraw = 0;
}

/* Смена темы: пересобираем виджеты в новых цветах */
void STATUSLINE_ApplyTheme(void) {
  if (s_root)
    lv_obj_del(s_root);
  s_root = s_batFrame = s_batNub = s_batFill = s_batText = s_antText =
      s_lockText = s_title = NULL;
}

void STATUSLINE_SetTitle(const char *t) {
  s_titleText = t ? t : "";
  if (s_title)
    lv_label_set_text_static(s_title, s_titleText);
}

void STATUSLINE_render(void) {
  if (!s_root)
    build();
  if (!s_root)
    return;

  bool antMain = ant_is_main();
  if (antMain != s_antShown) {
    s_antShown = antMain;
    if (s_antText)
      lv_label_set_text_static(s_antText, antMain ? "A2" : "A1");
  }

  bool locked = Mode_KeyLocked();
  if (locked != s_lockShown) {
    s_lockShown = locked;
    if (s_lockText)
      lv_label_set_text_static(s_lockText, locked ? "LOCK" : " ");
  }

  uint16_t bat = battery_voltage_cv();
  /* гистерезис 2 cV: без него ADC-шум дёргает перерисовку каждый кадр */
  if (s_prevBatDraw && bat >= s_prevBatDraw - 1 && bat <= s_prevBatDraw + 1)
    return;
  s_prevBatDraw = bat;

  uint8_t fill =
      (uint8_t)ConvertDomain((int)bat, BAT_MIN, BAT_MAX, 0, BAT_W - 4);
  if (fill > BAT_W - 4)
    fill = BAT_W - 4;
  Color fcol = bat >= 740 ? C_GREEN : bat >= 680 ? C_YELLOW : C_RED;

  if (s_batFill) {
    lv_obj_set_size(s_batFill, fill, BAT_H - 4);
    lv_obj_set_style_bg_color(s_batFill, lvc(fcol), LV_PART_MAIN);
  }
  if (s_batText) {
    static char t[8];
    snprintf(t, sizeof(t), "%u.%02uV", bat / 100, bat % 100);
    lv_label_set_text_static(s_batText, t);
  }
}

void STATUSLINE_SetText(const char *pattern, ...) {
  (void)pattern;
}
