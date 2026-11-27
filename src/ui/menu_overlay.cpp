#include "menu_overlay.h"
#include "graphics.h"
#include "lv_helpers.h"
#include "lv_port.h"
#include <stdio.h>
#include <string.h>

/* F_S: line_height 13 — подсказка + видимые пункты в компактном окне. */
#define ROW_H 13
#define HINT "UP/DN S1+ S2-"
/* Пункты со значением N/A (не поддержано железом) — серым. */
#define NA_COLOR "#808080 "

static struct {
  const MenuItem *items;
  uint8_t count;
  uint8_t sel;
  uint8_t offset;  /* первый видимый пункт при прокрутке */
  uint8_t visible; /* сколько строк помещается в окно */
  bool open;
  bool spectrum;
  bool dirty;
  bool bandDirty; /* прямой DMA затёр полосу — перерисовать пересечение */
  int16_t bandY1, bandY2;
  lv_coord_t w;
  lv_obj_t *box;
  lv_obj_t *selBox;
  lv_obj_t *hint;
  lv_obj_t *rows;
  lv_obj_t *scrollTrack;
  lv_obj_t *scrollThumb;
  char rowsBuf[640];
  char hintBuf[20];
  char hintShown[20];
  uint32_t rowsHash;
  uint8_t shownSel;
  int16_t thumbY;
  int16_t thumbH;
  bool rowsShown;
} ctx;

/* Подсветка выбранной строки: в светлой теме фиксированный голубой,
 * не пропускаем через TC(). */
static lv_color_t selColor(void) {
  if (gThemeLight)
    return lv_color_make(190, 215, 255);
  Color c = (Color)(((0 & 0xF8) << 8) | ((40 & 0xFC) << 3) | (96 >> 3));
  return lvColor(c);
}

static void setHidden(lv_obj_t *o, bool hidden) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden)
    return;
  if (hidden)
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* Ползунок: видимая часть списка и положение offset в окне. */
static void updateScrollbar(void) {
  if (!ctx.scrollTrack || !ctx.scrollThumb)
    return;
  bool need = ctx.count > ctx.visible;
  setHidden(ctx.scrollTrack, !need);
  setHidden(ctx.scrollThumb, !need);
  if (!need)
    return;

  int16_t trackH = (int16_t)(ctx.visible * ROW_H);
  int16_t thumbH = (int16_t)(trackH * ctx.visible / ctx.count);
  if (thumbH < 5)
    thumbH = 5;
  if (thumbH > trackH)
    thumbH = trackH;

  uint8_t maxOffset = (uint8_t)(ctx.count - ctx.visible);
  int16_t thumbY = (int16_t)(1 + ROW_H);
  if (maxOffset > 0)
    thumbY += (int16_t)((trackH - thumbH) * ctx.offset / maxOffset);

  if (thumbH != ctx.thumbH) {
    lv_obj_set_height(ctx.scrollThumb, thumbH);
    ctx.thumbH = thumbH;
  }
  if (thumbY != ctx.thumbY) {
    lv_obj_set_y(ctx.scrollThumb, thumbY);
    ctx.thumbY = thumbY;
  }
}

/* Копия строки в буфер с экранированием '#': в режиме recolor одиночная
 * решётка начинает команду цвета (в BUTTON_ITEMS есть имена «# short»). */
static char *appendText(char *p, char *end, const char *s) {
  for (; *s; ++s) {
    if ((*s == '#' ? p + 2 : p + 1) >= end)
      break;
    *p++ = *s;
    if (*s == '#')
      *p++ = '#';
  }
  *p = 0;
  return p;
}

static uint32_t hashStr(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) {
    h ^= (uint8_t)*s;
    h *= 16777619u;
  }
  return h;
}

static void refresh(void) {
  if (!ctx.open || !ctx.rows)
    return;

  /* Прокрутка: держим выбранный пункт внутри окна. */
  if (ctx.sel < ctx.offset)
    ctx.offset = ctx.sel;
  if (ctx.sel >= ctx.offset + ctx.visible)
    ctx.offset = (uint8_t)(ctx.sel - ctx.visible + 1);

  int cols = (ctx.w - 8) / FontW(F_S);
  char *p = ctx.rowsBuf;
  char *end = ctx.rowsBuf + sizeof(ctx.rowsBuf);
  p[0] = 0;
  uint8_t shown = 0;
  for (uint8_t i = 0; i < ctx.visible; ++i) {
    uint8_t idx = (uint8_t)(ctx.offset + i);
    if (idx >= ctx.count)
      break;
    const MenuItem *item = &ctx.items[idx];
    const char *v = item->value ? item->value(item) : "";
    bool na = strcmp(v, "N/A") == 0;
    int pad = cols - (int)strlen(item->name) - (int)strlen(v);
    if (pad < 1)
      pad = 1;
    if (na) {
      int n = snprintf(p, end - p, "%s", NA_COLOR);
      if (n < 0 || n >= end - p)
        break;
      p += n;
    }
    p = appendText(p, end, item->name);
    int n = snprintf(p, end - p, "%*s", pad, "");
    if (n < 0 || n >= end - p)
      break;
    p += n;
    p = appendText(p, end, v);
    if (na) {
      if (p + 1 >= end)
        break;
      *p++ = '#';
    }
    if (idx + 1 < ctx.count) {
      if (p + 1 >= end)
        break;
      *p++ = '\n';
    }
    *p = 0;
    shown++;
  }
  /* Без хвостового \n, если последний видимый пункт не последний в списке:
   * иначе LVGL добавляет пустую строку и текст уезжает. */
  if (shown && p > ctx.rowsBuf && p[-1] == '\n')
    p[-1] = 0;

  /* lv_label_set_text_static всегда инвалидирует лейбл, даже если строка
   * та же. При простом перемещении выбора текст не трогаем — перерисуется
   * только подсветка. */
  uint32_t hash = hashStr(ctx.rowsBuf);
  if (!ctx.rowsShown || hash != ctx.rowsHash) {
    lv_label_set_text_static(ctx.rows, ctx.rowsBuf);
    ctx.rowsHash = hash;
    ctx.rowsShown = true;
  }

  if (ctx.selBox && ctx.sel != ctx.shownSel) {
    lv_obj_set_pos(ctx.selBox, 1,
                   1 + ROW_H + (int)(ctx.sel - ctx.offset) * ROW_H);
    ctx.shownSel = ctx.sel;
  }

  updateScrollbar();

  if (ctx.hint) {
    const char *h = HINT;
    if (ctx.count > ctx.visible) {
      snprintf(ctx.hintBuf, sizeof(ctx.hintBuf), "UD S1+ S2- %u/%u",
               (unsigned)(ctx.sel + 1), (unsigned)ctx.count);
      h = ctx.hintBuf;
    }
    if (strcmp(h, ctx.hintShown) != 0) {
      snprintf(ctx.hintShown, sizeof(ctx.hintShown), "%s", h);
      lv_label_set_text_static(ctx.hint, h);
    }
  }

  /* Прямой DMA (спектр/курсор) мог затереть только пересечение окна с
   * нарисованной полосой — остальную часть меню не перерисовываем. */
  if (ctx.bandDirty) {
    lv_area_t c;
    lv_obj_get_coords(ctx.box, &c);
    lv_area_t a;
    a.x1 = c.x1;
    a.x2 = c.x2;
    a.y1 = c.y1 > ctx.bandY1 ? c.y1 : ctx.bandY1;
    a.y2 = c.y2 < ctx.bandY2 ? c.y2 : ctx.bandY2;
    if (a.y1 <= a.y2)
      lv_obj_invalidate_area(ctx.box, &a);
    ctx.bandDirty = false;
  }
  ctx.dirty = false;
}

void MenuOverlay_Close(void) {
  bool spectrum = ctx.spectrum;
  ctx.open = false;
  ctx.dirty = false;
  ctx.bandDirty = false;
  ctx.rowsShown = false;
  ctx.rowsHash = 0;
  ctx.shownSel = 0;
  ctx.thumbY = -1;
  ctx.thumbH = -1;
  ctx.hintShown[0] = 0;
  ctx.items = NULL;
  ctx.count = 0;
  if (ctx.box)
    lv_obj_del(ctx.box);
  ctx.box = ctx.selBox = ctx.hint = ctx.rows = NULL;
  ctx.scrollTrack = ctx.scrollThumb = NULL;
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
  ctx.offset = 0;
  ctx.shownSel = 0;
  ctx.thumbY = -1;
  ctx.thumbH = -1;
  ctx.rowsShown = false;
  ctx.rowsHash = 0;
  ctx.hintShown[0] = 0;
  ctx.bandDirty = false;
  int visible = (h - ROW_H) / ROW_H;
  if (visible < 1)
    visible = 1;
  if (visible > count)
    visible = count;
  ctx.visible = (uint8_t)visible;
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
  if (ctx.rows)
    lv_label_set_recolor(ctx.rows, true);
  /* Скроллбар у правого края: 2 px дорожка и ползунок на видимую часть. */
  ctx.scrollTrack = lvBox(ctx.box, w - 4, 1 + ROW_H, 2,
                          (lv_coord_t)(ctx.visible * ROW_H), C_GRAY,
                          LV_OPA_60);
  ctx.scrollThumb = lvBox(ctx.box, w - 4, 1 + ROW_H, 2, 5, C_CYAN,
                          LV_OPA_COVER);

  ctx.open = true;
  ctx.dirty = true;
  refresh();
}

bool MenuOverlay_IsOpen(void) { return ctx.open; }

uint8_t MenuOverlay_Selected(void) { return ctx.sel; }

void MenuOverlay_SetSelected(uint8_t sel) {
  if (!ctx.open || !ctx.count)
    return;
  ctx.sel = sel < ctx.count ? sel : (uint8_t)(ctx.count - 1);
  ctx.dirty = true;
}

bool MenuOverlay_Render(void) {
  if (ctx.open && ctx.dirty) {
    refresh();
    return true;
  }
  return false;
}

void MenuOverlay_InvalidateBand(int16_t y1, int16_t y2) {
  if (!ctx.open || y2 < y1)
    return;
  ctx.bandY1 = y1;
  ctx.bandY2 = y2;
  ctx.bandDirty = true;
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
      ctx.items[ctx.sel].change(&ctx.items[ctx.sel], +1);
    ctx.dirty = true;
    return true;
  case KEY_SIDE2:
    if (ctx.items[ctx.sel].change)
      ctx.items[ctx.sel].change(&ctx.items[ctx.sel], -1);
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
