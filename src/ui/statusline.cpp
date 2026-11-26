/* Статуслайн — нативные LVGL-виджеты, создаются один раз и живут
 * поверх всех режимов. Обновляются только при изменении значений.
 *
 * Слева: заголовок режима и иконка блокировки.
 * Справа: A1/A2 — положение реле антенны (PF0/K200), батарея
 * (рамка + горизонтальная заливка уровня + напряжение внутри).
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

#define TITLE_X 8
#define TITLE_W 52

/* Иконка замка: 10x12, рисуется прямоугольниками (LV_DRAW_COMPLEX=0). */
#define LOCK_X 174
#define LOCK_W 10
#define LOCK_H 12
#define LOCK_Y ((UI_STATUS_H - LOCK_H) / 2)

#define ANT_X 158
#define ANT_W 14

/* Напряжение внутри батареи: заливка уровня на всю высоту, под ней —
 * серая «дорожка» (иначе текст на чёрном фоне не читается). */
#define BAT_W 44
#define BAT_H 15
#define BAT_X (LCD_WIDTH - 8 - BAT_W - 2)
#define BAT_Y ((UI_STATUS_H - BAT_H) / 2)
#define BAT_FILL_H (BAT_H - 2)
#define BAT_FILL_Y (BAT_Y + 1)
/* Пакет 2S: 6.00–8.40 В (номинал 7.4 В). */
#define BAT_MIN 600
#define BAT_MAX 840
/* Батарея «дрыгается»: ADC шумит. Читаем не чаще раза в 500 мс и усредняем
 * по 8 отсчётам (окно ~4 с). */
#define BAT_SAMPLE_MS 500
#define BAT_AVG_N 8

static lv_obj_t *s_root;
static lv_obj_t *s_batFrame;
static lv_obj_t *s_batNub;
static lv_obj_t *s_batTrack;
static lv_obj_t *s_batFill;
static lv_obj_t *s_batText;
static lv_obj_t *s_antText;
static lv_obj_t *s_lockIcon;
static lv_obj_t *s_title;
static uint16_t s_prevBatDraw;
static uint16_t s_batBuf[BAT_AVG_N];
static uint8_t s_batIdx;
static uint8_t s_batCnt;
static uint32_t s_batLastMs;
static uint16_t s_batCv;
static bool s_batInit;
static bool s_antShown;
static bool s_lockShown;
static const char *s_titleText = "";

static lv_obj_t *mkText(lv_coord_t x, lv_coord_t w, lv_text_align_t al,
                        Color c) {
  return lvLabel(s_root, F_S, c, x, (UI_STATUS_H - FontH(F_S)) / 2, w, al);
}

/* HIDDEN трогаем только при реальной смене: иначе каждый кадр
 * инвалидируется область и LVGL перерисовывает весь экран. */
static void setHidden(lv_obj_t *o, bool hidden) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden)
    return;
  if (hidden)
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void lockRect(lv_draw_ctx_t *dc, lv_draw_rect_dsc_t *d, lv_coord_t x,
                     lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_color_t c) {
  lv_area_t a = {x, y, (lv_coord_t)(x + w - 1), (lv_coord_t)(y + h - 1)};
  d->bg_color = c;
  lv_draw_rect(dc, d, &a);
}

/* Пиксельный замок: скоба 2+6+2 px и корпус 10x8 с прорезью. */
static void lockDrawCb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);

  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = 0;
  d.bg_opa = LV_OPA_COVER;

  lv_color_t fg = lvColor(C_YELLOW);
  lv_color_t bg = lvColor(C_BLACK);
  lockRect(dc, &d, a.x1 + 2, a.y1, 6, 1, fg);
  lockRect(dc, &d, a.x1 + 1, a.y1 + 1, 2, 3, fg);
  lockRect(dc, &d, a.x1 + 7, a.y1 + 1, 2, 3, fg);
  lockRect(dc, &d, a.x1, a.y1 + 4, 10, 8, fg);
  lockRect(dc, &d, a.x1 + 4, a.y1 + 6, 2, 3, bg);
}

static void build(void) {
  s_root =
      lvBox(lv_scr_act(), 0, 0, LCD_WIDTH, UI_STATUS_H, C_BLACK, LV_OPA_TRANSP, 0);
  if (!s_root)
    return;
  /* Батарея: контур с носиком, заливка-уровень и напряжение внутри. */
  s_batFrame = lvBox(s_root, BAT_X, BAT_Y, BAT_W, BAT_H, C_BLACK,
                     LV_OPA_TRANSP, 3);
  if (s_batFrame) {
    lv_obj_set_style_border_width(s_batFrame, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_batFrame, lvColor(C_GRAY), LV_PART_MAIN);
  }
  s_batNub = lvBox(s_root, BAT_X + BAT_W, BAT_Y + (BAT_H - 5) / 2, 2, 5,
                   C_GRAY, LV_OPA_COVER, 1);
  s_batTrack = lvBox(s_root, BAT_X + 1, BAT_FILL_Y, BAT_W - 2, BAT_FILL_H,
                     C_GRAY, LV_OPA_COVER, 1);
  s_batFill = lvBox(s_root, BAT_X + 1, BAT_FILL_Y, 0, BAT_FILL_H, C_GREEN,
                    LV_OPA_COVER, 1);
  s_batText = lvLabel(s_root, F_S, C_BLACK, BAT_X + 2, BAT_Y + 1, BAT_W - 4,
                      LV_TEXT_ALIGN_CENTER);
  if (s_batText)
    lv_obj_set_style_text_color(s_batText, lv_color_make(0, 0, 0), LV_PART_MAIN);
  /* A1 — только второй разъём (PF0 LOW), A2 — добавлена основная. */
  s_antText = mkText(ANT_X, ANT_W, LV_TEXT_ALIGN_LEFT, C_GRAY);
  s_antShown = ant_is_main();
  if (s_antText)
    lv_label_set_text_static(s_antText, s_antShown ? "A2" : "A1");

  s_lockIcon =
      lvBox(s_root, LOCK_X, LOCK_Y, LOCK_W, LOCK_H, C_BLACK, LV_OPA_TRANSP, 0);
  if (s_lockIcon)
    lv_obj_add_event_cb(s_lockIcon, lockDrawCb, LV_EVENT_DRAW_MAIN, NULL);
  s_lockShown = Mode_KeyLocked();
  setHidden(s_lockIcon, !s_lockShown);

  s_title = mkText(TITLE_X, TITLE_W, LV_TEXT_ALIGN_LEFT, C_GRAY);
  if (s_title)
    lv_label_set_text_static(s_title, s_titleText);
  s_prevBatDraw = 0;
}

/* Смена темы: пересобираем виджеты в новых цветах */
void STATUSLINE_ApplyTheme(void) {
  if (s_root)
    lv_obj_del(s_root);
  s_root = s_batFrame = s_batNub = s_batTrack = s_batFill = s_batText =
      s_antText = s_lockIcon = s_title = NULL;
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
    setHidden(s_lockIcon, !locked);
  }

  uint32_t now = millis();
  if (!s_batInit || now - s_batLastMs >= BAT_SAMPLE_MS) {
    s_batLastMs = now;
    s_batInit = true;
    s_batBuf[s_batIdx] = battery_voltage_cv();
    s_batIdx = (uint8_t)((s_batIdx + 1) % BAT_AVG_N);
    if (s_batCnt < BAT_AVG_N)
      s_batCnt++;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < s_batCnt; i++)
      sum += s_batBuf[i];
    s_batCv = (uint16_t)(sum / s_batCnt);
  }
  uint16_t bat = s_batCv;
  /* гистерезис 2 cV: без него ADC-шум дёргает перерисовку каждый кадр */
  if (s_prevBatDraw && bat >= s_prevBatDraw - 1 && bat <= s_prevBatDraw + 1)
    return;
  s_prevBatDraw = bat;

  uint8_t fill =
      (uint8_t)ConvertDomain((int)bat, BAT_MIN, BAT_MAX, 0, BAT_W - 2);
  if (fill > BAT_W - 2)
    fill = BAT_W - 2;
  Color fcol = bat >= 740 ? C_GREEN : bat >= 680 ? C_YELLOW : C_RED;

  if (s_batFill) {
    lv_obj_set_size(s_batFill, fill, BAT_FILL_H);
    lv_obj_set_style_bg_color(s_batFill, lvColor(fcol), LV_PART_MAIN);
  }
  if (s_batText) {
    static char t[8];
    snprintf(t, sizeof(t), "%u.%02uV", bat / 100, bat % 100);
    lv_label_set_text_static(s_batText, t);
  }
}
