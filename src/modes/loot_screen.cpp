/* Экран списка находок — нативный LVGL (по образцу vfo.cpp).
 *
 * Весь список — один многострочный лейбл с recolor (по цвету на строку),
 * подсветка выбранной строки — одна подложка. Так на экран уходит всего
 * несколько LVGL-объектов (куча LVGL — 12 КБ).
 *
 * Клавиши:
 *   UP/DOWN   — выбор строки (приёмник перестраивается на неё — слышно);
 *   1/2/3/4   — сортировка: время/длительность/чёрные/частота (повтор —
 *               обратный порядок);
 *   SIDE1     — чёрный список вкл/выкл для выбранной; SIDE2 — белый;
 *   0         — удалить выбранную; длинный 0 — очистить весь список;
 *   MENU      — выбранную частоту в VFO и выйти в VFO;
 *   EXIT      — назад (параметры VFO восстанавливаются).
 */
#include "loot_screen.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../helper/lootlist.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/statusline.h"
#include "lvgl.h"
#include "mode.h"
#include "printf.h"
#include "vfo.h"
#include <stdio.h>
#include <string.h>

#define TITLE_Y 26
#define LIST_Y 42
#define ROW_H 15
#define ROWS 15
#define DETAIL_Y (LIST_Y + ROWS * ROW_H + 3)
#define HINT_Y (DETAIL_Y + 18)

typedef enum { SORT_TIME, SORT_DUR, SORT_BL, SORT_F, SORT_COUNT } SortType;

static const LootCmp SORTERS[SORT_COUNT] = {
    LOOT_SortByLastOpenTime, LOOT_SortByDuration, LOOT_SortByBlacklist,
    LOOT_SortByF};
static const char *const SORT_NAMES[SORT_COUNT] = {"time", "dur", "bl", "freq"};
static const char *const MOD_NAMES[8] = {"FM", "AM", "LSB", "USB",
                                         "BYP", "RAW", "WFM", "?"};
static const char *const RADIO_NAMES[3] = {"BK-A", "BK-B", "SI"};

static Mode *s_prev;
static lv_obj_t *s_scr;
static lv_obj_t *s_sel;
static lv_obj_t *s_title;
static lv_obj_t *s_list;
static lv_obj_t *s_detail;
static lv_obj_t *s_hint;

static uint16_t s_selIdx;
static uint16_t s_top;
static SortType s_sort = SORT_TIME;
static bool s_sortRev;
static bool s_dirty;
static bool s_keep; /* MENU: оставить выбранную частоту в VFO */
static uint16_t s_lastSec;
static uint16_t s_lastCount;

/* параметры VFO на входе — на EXIT возвращаем (прослушивание их меняет) */
static uint32_t s_savedF;
static uint8_t s_savedMod;

static char s_listBuf[ROWS * 40];
static char s_titleBuf[40];
static char s_detailBuf[48];

static inline lv_color_t lvc(uint8_t r, uint8_t g, uint8_t b) {
  /* палитра «тёмной» темы → через TC() в светлую */
  Color c = (Color)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  if (gThemeLight && r == 0 && g == 40 && b == 96)
    return lv_color_make(190, 215, 255); /* подсветка выбранной строки */
  c = TC(c);
  return lv_color_make((uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC),
                       (uint8_t)(c << 3));
}

static lv_obj_t *mkBox(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, lv_color_t c, lv_opa_t opa) {
  lv_obj_t *o = lv_obj_create(parent);
  if (!o)
    return NULL;
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(o, c, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN);
  return o;
}

static lv_obj_t *mkLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t c,
                         lv_coord_t x, lv_coord_t y, lv_coord_t w) {
  lv_obj_t *l = lv_label_create(parent);
  if (!l)
    return NULL;
  lv_obj_set_pos(l, x, y);
  if (w) {
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  }
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, c, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_pad_all(l, 0, LV_PART_MAIN);
  return l;
}

/* компактное время: «45s», «12m», «3h» (3 символа) */
static void fmtSec(char *out, uint32_t s) {
  if (s < 100)
    snprintf(out, 5, "%2us", (unsigned)s);
  else if (s < 6000)
    snprintf(out, 5, "%2um", (unsigned)(s / 60));
  else
    snprintf(out, 5, "%2uh", (unsigned)(s / 3600 > 99 ? 99 : s / 3600));
}

static void tuneSel(void) {
  Loot *it = LOOT_Item(s_selIdx);
  if (!it)
    return;
  vfos[currentVfo].modulation = it->modulation;
  Radio_TuneTo(it->f, true);
}

static void clampSel(void) {
  uint16_t n = LOOT_Size();
  if (!n) {
    s_selIdx = s_top = 0;
    return;
  }
  if (s_selIdx >= n)
    s_selIdx = n - 1;
  if (s_selIdx < s_top)
    s_top = s_selIdx;
  if (s_selIdx >= s_top + ROWS)
    s_top = s_selIdx - ROWS + 1;
}

/* пересобрать тексты и подсветку */
static void refresh(void) {
  uint16_t n = LOOT_Size();
  clampSel();

  /* заголовок */
  snprintf(s_titleBuf, sizeof(s_titleBuf), "LOOT %u/%u  by %s %s",
           (unsigned)(n ? s_selIdx + 1 : 0), (unsigned)n, SORT_NAMES[s_sort],
           s_sortRev ? "rev" : "");
  lv_label_set_text_static(s_title, s_titleBuf);

  /* список: строка = метка, частота, «сколько назад», длительность */
  char *p = s_listBuf;
  char *end = s_listBuf + sizeof(s_listBuf);
  p[0] = 0;
  if (!n) {
    snprintf(p, end - p, "#808080 empty: scan to find#");
  }
  for (uint16_t r = 0; r < ROWS && s_top + r < n; ++r) {
    const Loot *it = LOOT_Item(s_top + r);
    const bool L = gThemeLight;
    const char *col = it->blacklist ? "909090"
                      : it->whitelist ? (L ? "108010" : "40ff40")
                      : it->open      ? (L ? "a08000" : "ffff00")
                                      : (L ? "000000" : "ffffff");
    char ago[5], dur[5];
    fmtSec(ago, LOOT_SecondsAgo(it));
    fmtSec(dur, it->duration);
    int w = snprintf(p, end - p, "#%s %c%4u.%05u %s %s#\n", col,
                     it->blacklist ? '-' : it->whitelist ? '+' : ' ',
                     (unsigned)(it->f / MHZ), (unsigned)(it->f % MHZ), ago,
                     dur);
    if (w < 0 || w >= end - p)
      break;
    p += w;
  }
  lv_label_set_text_static(s_list, s_listBuf);

  /* подсветка выбранной строки */
  if (n) {
    lv_obj_clear_flag(s_sel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_sel, 0, LIST_Y + (s_selIdx - s_top) * ROW_H);
  } else {
    lv_obj_add_flag(s_sel, LV_OBJ_FLAG_HIDDEN);
  }

  /* детали выбранной */
  if (n) {
    const Loot *it = LOOT_Item(s_selIdx);
    char dur[5];
    fmtSec(dur, it->duration);
    snprintf(s_detailBuf, sizeof(s_detailBuf), "%s %s %ddBm dur %s",
             MOD_NAMES[it->modulation & 7],
             RADIO_NAMES[it->radio > 2 ? 2 : it->radio], (int)it->peak - 160,
             dur);
  } else {
    s_detailBuf[0] = ' ';
    s_detailBuf[1] = 0;
  }
  lv_label_set_text_static(s_detail, s_detailBuf);

  s_dirty = false;
  s_lastCount = n;
  s_lastSec = (uint16_t)(millis() / 1000);
}

static void move(int8_t d) {
  uint16_t n = LOOT_Size();
  if (!n)
    return;
  if (d < 0)
    s_selIdx = s_selIdx ? s_selIdx - 1 : n - 1;
  else
    s_selIdx = (s_selIdx + 1 < n) ? s_selIdx + 1 : 0;
  s_dirty = true;
  clampSel();
  tuneSel();
}

static void sortBy(SortType t) {
  if (s_sort == t)
    s_sortRev = !s_sortRev;
  else {
    s_sort = t;
    s_sortRev = false;
  }
  /* запоминаем выбранную по частоте — после сортировки найдём заново */
  const Loot *cur = LOOT_Item(s_selIdx);
  uint32_t f = cur ? cur->f : 0;
  LOOT_Sort(SORTERS[t], s_sortRev);
  if (f) {
    int16_t i = LOOT_IndexOf(LOOT_Get(f));
    s_selIdx = i < 0 ? 0 : i;
  }
  s_top = 0;
  s_dirty = true;
}

class LootListMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("");
    s_keep = false;
    s_savedF = vfos[currentVfo].rxF;
    s_savedMod = vfos[currentVfo].modulation;

    UI_ReleaseLabels();
    UI_ClearScreen(BG());
    Radio_Init();

    s_scr = mkBox(lv_scr_act(), 0, 0, LCD_WIDTH, LCD_HEIGHT, lvc(0, 0, 0),
                  LV_OPA_TRANSP);
    s_sel = mkBox(s_scr, 0, LIST_Y, LCD_WIDTH, ROW_H, lvc(0, 40, 96),
                  LV_OPA_COVER);
    s_title = mkLabel(s_scr, F_M, lvc(255, 255, 0), 4, TITLE_Y,
                      LCD_WIDTH - 8);
    s_list = mkLabel(s_scr, F_M, lvc(255, 255, 255), 4, LIST_Y,
                     LCD_WIDTH - 8);
    if (s_list)
      lv_label_set_recolor(s_list, true);
    s_detail = mkLabel(s_scr, F_M, lvc(0, 255, 255), 4, DETAIL_Y,
                       LCD_WIDTH - 8);
    s_hint = mkLabel(s_scr, F_S, lvc(128, 128, 128), 4, HINT_Y,
                     LCD_WIDTH - 8);
    if (s_hint)
      lv_label_set_text_static(
          s_hint, "1 TIME 2 DUR 3 BL 4 FREQ\nS1 BLACK S2 WHITE 0 DEL MENU>VFO");

    if (!s_scr || !s_sel || !s_title || !s_list || !s_detail)
      return; /* куча LVGL исчерпана — экран пустой, но без падения */

    clampSel();
    s_dirty = true;
    refresh();
    if (LOOT_Size())
      tuneSel();
  }

  void exit() override {
    if (s_scr) {
      lv_obj_del(s_scr);
      s_scr = s_sel = s_title = s_list = s_detail = s_hint = NULL;
    }
    if (!s_keep) {
      vfos[currentVfo].modulation = s_savedMod;
      Radio_TuneTo(s_savedF, true);
    }
  }

  void update() override { Radio_Update(); /* шумодав/звук выбранной */ }

  void render() override {
    STATUSLINE_render();
    if (!s_scr || !s_list)
      return;
    /* раз в секунду (время «назад») и при смене размера/состояния */
    if (s_dirty || LOOT_Size() != s_lastCount ||
        (uint16_t)(millis() / 1000) != s_lastSec)
      refresh();
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (!s_scr || !s_list)
      return false;

    if (state == KEY_LONG_PRESSED) {
      if (k == KEY_0) { /* очистить всё */
        LOOT_Clear();
        s_selIdx = s_top = 0;
        s_dirty = true;
        return true;
      }
      return false;
    }
    if (state == KEY_LONG_PRESSED_CONT && (k == KEY_UP || k == KEY_DOWN)) {
      move(k == KEY_UP ? -1 : +1);
      return true;
    }
    if (state != KEY_RELEASED)
      return false;

    switch (k) {
    case KEY_UP:
      move(-1);
      return true;
    case KEY_DOWN:
      move(+1);
      return true;
    case KEY_1:
      sortBy(SORT_TIME);
      return true;
    case KEY_2:
      sortBy(SORT_DUR);
      return true;
    case KEY_3:
      sortBy(SORT_BL);
      return true;
    case KEY_4:
      sortBy(SORT_F);
      return true;
    case KEY_SIDE1:
      LOOT_ToggleBlacklist(LOOT_Item(s_selIdx));
      s_dirty = true;
      return true;
    case KEY_SIDE2:
      LOOT_ToggleWhitelist(LOOT_Item(s_selIdx));
      s_dirty = true;
      return true;
    case KEY_0:
      if (LOOT_Size()) {
        LOOT_Remove(s_selIdx);
        s_dirty = true;
        clampSel();
        tuneSel();
      }
      return true;
    case KEY_MENU: /* выбранную — в VFO */
      if (LOOT_Size()) {
        tuneSel();
        s_keep = true;
        Mode_Switch(MODE_VFO);
      }
      return true;
    case KEY_EXIT:
      Mode_Switch(s_prev ? *s_prev : MODE_VFO);
      return true;
    default:
      break;
    }
    return false;
  }
};

static LootListMode lootListInstance;
Mode &MODE_LOOTLIST = lootListInstance;

void LOOTLIST_OpenFrom(Mode &from) {
  s_prev = &from;
  Mode_Switch(MODE_LOOTLIST);
}
