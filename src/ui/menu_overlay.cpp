#include "menu_overlay.h"
#include "graphics.h"
#include "lv_helpers.h"
#include "lv_port.h"
#include <stdio.h>
#include <string.h>

/* F_S: line_height 13 — подсказка + 2 пункта в компактном окне. */
#define ROW_H 13
#define HINT "UP/DN S1+ S2-"

static struct {
  const MenuItem *items;
  uint8_t count;
  uint8_t sel;
  bool open;
  bool spectrum;
  bool dirty;
  lv_coord_t w;
  lv_obj_t *box;
  lv_obj_t *selBox;
  lv_obj_t *hint;
  lv_obj_t *rows;
  char rowsBuf[192];
} ctx;

/* Подсветка выбранной строки: в светлой теме фиксированный голубой,
 * не пропускаем через TC(). */
static lv_color_t selColor(void) {
  if (gThemeLight)
    return lv_color_make(190, 215, 255);
  Color c = (Color)(((0 & 0xF8) << 8) | ((40 & 0xFC) << 3) | (96 >> 3));
  return lvColor(c);
}

static void refresh(void) {
  if (!ctx.open || !ctx.rows)
    return;
  int cols = (ctx.w - 8) / FontW(F_S);
  char *p = ctx.rowsBuf;
  char *end = ctx.rowsBuf + sizeof(ctx.rowsBuf);
  p[0] = 0;
  for (uint8_t i = 0; i < ctx.count; ++i) {
    const char *v = ctx.items[i].value ? ctx.items[i].value() : "";
    int pad = cols - (int)strlen(ctx.items[i].name) - (int)strlen(v);
    if (pad < 1)
      pad = 1;
    int n = snprintf(p, end - p, "%s%*s%s%s", ctx.items[i].name, pad, "", v,
                     i + 1 < ctx.count ? "\n" : "");
    if (n < 0 || n >= end - p)
      break;
    p += n;
  }
  lv_label_set_text_static(ctx.rows, ctx.rowsBuf);
  if (ctx.selBox)
    lv_obj_set_pos(ctx.selBox, 1, 1 + ROW_H + (int)ctx.sel * ROW_H);
  /* Пункт мог перерисовать зону напрямую (например, onRangeChanged в
   * сканере/анализаторе) — просим LVGL заново нарисовать всё меню. */
  lv_obj_invalidate(ctx.box);
  ctx.dirty = false;
}

void MenuOverlay_Close(void) {
  bool spectrum = ctx.spectrum;
  ctx.open = false;
  ctx.dirty = false;
  ctx.items = NULL;
  ctx.count = 0;
  if (ctx.box)
    lv_obj_del(ctx.box);
  ctx.box = ctx.selBox = ctx.hint = ctx.rows = NULL;
  ctx.spectrum = false;
  if (spectrum)
    lv_port_set_spectrum_overlay(false);
}

void MenuOverlay_Open(const MenuItem *items, uint8_t count, lv_coord_t x,
                      lv_coord_t y, lv_coord_t w, lv_coord_t h,
                      bool spectrumOverlay) {
  MenuOverlay_Close();
  if (!items || !count)
    return;

  ctx.items = items;
  ctx.count = count;
  ctx.sel = 0;
  ctx.w = w;
  ctx.spectrum = spectrumOverlay;
  if (spectrumOverlay)
    lv_port_set_spectrum_overlay(true);

  ctx.box = lvBox(lv_scr_act(), x, y, w, h, C_BLACK, LV_OPA_COVER);
  if (!ctx.box) {
    if (spectrumOverlay)
      lv_port_set_spectrum_overlay(false);
    ctx.spectrum = false;
    return;
  }
  lv_obj_set_style_border_width(ctx.box, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(ctx.box, lvColor(C_GRAY), LV_PART_MAIN);

  ctx.selBox = lvBox(ctx.box, 1, 1 + ROW_H, w - 2, ROW_H, C_BLACK,
                     LV_OPA_COVER);
  if (ctx.selBox)
    lv_obj_set_style_bg_color(ctx.selBox, selColor(), LV_PART_MAIN);
  ctx.hint = lvLabel(ctx.box, F_S, C_GRAY, 4, 1, w - 8, LV_TEXT_ALIGN_LEFT);
  if (ctx.hint)
    lv_label_set_text_static(ctx.hint, HINT);
  ctx.rows = lvLabel(ctx.box, F_S, C_WHITE, 4, 1 + ROW_H, w - 8,
                     LV_TEXT_ALIGN_LEFT);

  ctx.open = true;
  ctx.dirty = true;
  refresh();
}

bool MenuOverlay_IsOpen(void) { return ctx.open; }

void MenuOverlay_Render(void) {
  if (ctx.open && ctx.dirty)
    refresh();
}

void MenuOverlay_Invalidate(void) {
  if (ctx.open)
    ctx.dirty = true;
}

bool MenuOverlay_Key(KEY_Code_t key, Key_State_t state) {
  if (!ctx.open)
    return false;

  if (state == KEY_LONG_PRESSED_CONT) {
    if (key == KEY_UP || key == KEY_DOWN) {
      ctx.sel = (uint8_t)((ctx.sel + ctx.count +
                           (key == KEY_DOWN ? 1 : ctx.count - 1)) %
                          ctx.count);
      ctx.dirty = true;
    }
    return true;
  }

  if (state != KEY_RELEASED)
    return true;

  switch (key) {
  case KEY_UP:
    ctx.sel = (uint8_t)((ctx.sel + ctx.count - 1) % ctx.count);
    ctx.dirty = true;
    return true;
  case KEY_DOWN:
    ctx.sel = (uint8_t)((ctx.sel + 1) % ctx.count);
    ctx.dirty = true;
    return true;
  case KEY_SIDE1:
    if (ctx.items[ctx.sel].change)
      ctx.items[ctx.sel].change(+1);
    ctx.dirty = true;
    return true;
  case KEY_SIDE2:
    if (ctx.items[ctx.sel].change)
      ctx.items[ctx.sel].change(-1);
    ctx.dirty = true;
    return true;
  case KEY_MENU:
  case KEY_EXIT:
    MenuOverlay_Close();
    return true;
  case KEY_STAR: /* закрыть меню и отдать STAR режиму (loot) */
    MenuOverlay_Close();
    return false;
  default:
    return true; /* модальное меню: остальные клавиши не проваливаются */
  }
}
