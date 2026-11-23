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
#include "lv_helpers.h"
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

static lv_obj_t *mkText(lv_coord_t x, lv_coord_t w, lv_text_align_t al,
                        Color c) {
  return lvLabel(s_root, F_S, c, x, (UI_STATUS_H - FontH(F_S)) / 2, w, al);
}

static void build(void) {
  s_root =
      lvBox(lv_scr_act(), 0, 0, LCD_WIDTH, UI_STATUS_H, C_BLACK, LV_OPA_TRANSP, 0);
  if (!s_root)
    return;
  /* батарея: контур с носиком, заливка внутри */
  s_batFrame = lvBox(s_root, BAT_X, BAT_Y, BAT_W, BAT_H, C_BLACK,
                     LV_OPA_TRANSP, 3);
  if (s_batFrame) {
    lv_obj_set_style_border_width(s_batFrame, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_batFrame, lvColor(C_GRAY), LV_PART_MAIN);
  }
  s_batNub = lvBox(s_root, BAT_X + BAT_W, BAT_Y + 3, 2, 5, C_GRAY,
                   LV_OPA_COVER, 1);
  s_batFill = lvBox(s_root, BAT_X + 2, BAT_Y + 2, 0, BAT_H - 4, C_GREEN,
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
    lv_obj_set_style_bg_color(s_batFill, lvColor(fcol), LV_PART_MAIN);
  }
  if (s_batText) {
    static char t[8];
    snprintf(t, sizeof(t), "%u.%02uV", bat / 100, bat % 100);
    lv_label_set_text_static(s_batText, t);
  }
}
