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
 *   длинный 5 — whitelist-находки в каналы; длинный 8 — blacklist в каналы;
 *   длинный 9 — чёрный список каналов в loot (вручную);
 *   MENU      — выбранную частоту в VFO и выйти в VFO;
 *   EXIT      — назад (параметры VFO восстанавливаются).
 */
#include "modes.h"
#include "vfo_session.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../helper/channels.h"
#include "../helper/lootlist.h"
#include "../helper/names.h"
#include "../helper/unit_fmt.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/lv_helpers.h"
#include "../ui/statusline.h"
#include "lvgl.h"
#include "mode.h"
#include "printf.h"
#include <stdio.h>
#include <string.h>

#define TITLE_Y 26
#define LIST_Y 42
#define ROW_H 16 /* line_height шрифта F_M: иначе подсветка уезжает от текста */
#define ROWS 13
#define DETAIL_Y (LIST_Y + ROWS * ROW_H + 3)
#define HINT_Y (DETAIL_Y + 18)

typedef enum { SORT_TIME, SORT_DUR, SORT_BL, SORT_F, SORT_COUNT } SortType;

static const LootCmp SORTERS[SORT_COUNT] = {
    LOOT_SortByLastOpenTime, LOOT_SortByDuration, LOOT_SortByBlacklist,
    LOOT_SortByF};
static const char *const SORT_NAMES[SORT_COUNT] = {"time", "dur", "bl", "freq"};

static struct Ctx {
  Mode *s_prev;
  lv_obj_t *s_scr;
  lv_obj_t *s_sel;
  lv_obj_t *s_title;
  lv_obj_t *s_list;
  lv_obj_t *s_detail;
  lv_obj_t *s_hint;
  uint16_t s_selIdx;
  uint16_t s_top;
  SortType s_sort = SORT_TIME;
  bool s_sortRev;
  bool s_dirty;
  bool s_keep; /* MENU: оставить выбранную частоту в VFO */
  uint16_t s_lastSec;
  uint16_t s_lastCount;
  VfoSnapshot s_saved;
  char s_listBuf[ROWS * 40];
  char s_titleBuf[40];
  char s_detailBuf[48];
  char s_msg[16];
  uint32_t s_msgUntil;
} ctx;

static void flashMsg(const char *tag, int16_t n) {
  if (n < 0)
    snprintf(ctx.s_msg, sizeof(ctx.s_msg), "ERR");
  else
    snprintf(ctx.s_msg, sizeof(ctx.s_msg), "%s %d", tag, (int)n);
  ctx.s_msgUntil = millis() + 1200;
  ctx.s_dirty = true;
}

/* Подсветка выбранной строки: в светлой теме фиксированный голубой,
 * не пропускаем через TC(). */
static lv_color_t lootSelColor(void) {
  if (gThemeLight)
    return lv_color_make(190, 215, 255);
  Color c = (Color)(((0 & 0xF8) << 8) | ((40 & 0xFC) << 3) | (96 >> 3));
  return lvColor(c);
}

static void tuneSel(void) {
  Loot *it = LOOT_Item(ctx.s_selIdx);
  if (!it)
    return;
  vfos[currentVfo].modulation = it->modulation;
  Radio_TuneTo(it->f, true);
}

static void clampSel(void) {
  uint16_t n = LOOT_Size();
  if (!n) {
    ctx.s_selIdx = ctx.s_top = 0;
    return;
  }
  if (ctx.s_selIdx >= n)
    ctx.s_selIdx = n - 1;
  if (ctx.s_selIdx < ctx.s_top)
    ctx.s_top = ctx.s_selIdx;
  if (ctx.s_selIdx >= ctx.s_top + ROWS)
    ctx.s_top = ctx.s_selIdx - ROWS + 1;
}

/* пересобрать тексты и подсветку */
static void refresh(void) {
  uint16_t n = LOOT_Size();
  clampSel();

  /* заголовок; на 1.2 с — результат long 5/8/9 */
  if (ctx.s_msg[0] && (int32_t)(ctx.s_msgUntil - millis()) > 0) {
    lv_label_set_text_static(ctx.s_title, ctx.s_msg);
  } else {
    ctx.s_msg[0] = 0;
    snprintf(ctx.s_titleBuf, sizeof(ctx.s_titleBuf), "LOOT %u/%u  by %s %s",
             (unsigned)(n ? ctx.s_selIdx + 1 : 0), (unsigned)n,
             SORT_NAMES[ctx.s_sort], ctx.s_sortRev ? "rev" : "");
    lv_label_set_text_static(ctx.s_title, ctx.s_titleBuf);
  }

  /* список: строка = метка, частота, «сколько назад», длительность */
  char *p = ctx.s_listBuf;
  char *end = ctx.s_listBuf + sizeof(ctx.s_listBuf);
  p[0] = 0;
  if (!n) {
    snprintf(p, end - p, "#808080 empty: scan to find#");
  }
  for (uint16_t r = 0; r < ROWS && ctx.s_top + r < n; ++r) {
    const Loot *it = LOOT_Item(ctx.s_top + r);
    const bool L = gThemeLight;
    const char *col = it->blacklist ? "909090"
                      : it->whitelist ? (L ? "108010" : "40ff40")
                      : it->open      ? (L ? "a08000" : "ffff00")
                                      : (L ? "000000" : "ffffff");
    char ago[5], dur[5];
    fmt_duration(ago, sizeof(ago), LOOT_SecondsAgo(it));
    fmt_duration(dur, sizeof(dur), it->duration);
    int w = snprintf(p, end - p, "#%s %c%4u.%05u %s %s#\n", col,
                     it->blacklist ? '-' : it->whitelist ? '+' : ' ',
                     (unsigned)(it->f / MHZ), (unsigned)(it->f % MHZ), ago,
                     dur);
    if (w < 0 || w >= end - p)
      break;
    p += w;
  }
  lv_label_set_text_static(ctx.s_list, ctx.s_listBuf);

  /* подсветка выбранной строки */
  if (n) {
    lv_obj_clear_flag(ctx.s_sel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(ctx.s_sel, 0, LIST_Y + (ctx.s_selIdx - ctx.s_top) * ROW_H);
  } else {
    lv_obj_add_flag(ctx.s_sel, LV_OBJ_FLAG_HIDDEN);
  }

  /* детали выбранной */
  if (n) {
    const Loot *it = LOOT_Item(ctx.s_selIdx);
    char dur[5];
    fmt_duration(dur, sizeof(dur), it->duration);
    snprintf(ctx.s_detailBuf, sizeof(ctx.s_detailBuf), "%s %s %ddBm dur %s",
             MOD_NAMES[it->modulation & 7],
             RADIO_NAMES[it->radio > 2 ? 2 : it->radio], (int)it->peak - 160,
             dur);
  } else {
    ctx.s_detailBuf[0] = ' ';
    ctx.s_detailBuf[1] = 0;
  }
  lv_label_set_text_static(ctx.s_detail, ctx.s_detailBuf);

  ctx.s_dirty = false;
  ctx.s_lastCount = n;
  ctx.s_lastSec = (uint16_t)(millis() / 1000);
}

static void move(int8_t d) {
  uint16_t n = LOOT_Size();
  if (!n)
    return;
  if (d < 0)
    ctx.s_selIdx = ctx.s_selIdx ? ctx.s_selIdx - 1 : n - 1;
  else
    ctx.s_selIdx = (ctx.s_selIdx + 1 < n) ? ctx.s_selIdx + 1 : 0;
  ctx.s_dirty = true;
  clampSel();
  tuneSel();
}

static void sortBy(SortType t) {
  if (ctx.s_sort == t)
    ctx.s_sortRev = !ctx.s_sortRev;
  else {
    ctx.s_sort = t;
    ctx.s_sortRev = false;
  }
  /* запоминаем выбранную по частоте — после сортировки найдём заново */
  const Loot *cur = LOOT_Item(ctx.s_selIdx);
  uint32_t f = cur ? cur->f : 0;
  LOOT_Sort(SORTERS[t], ctx.s_sortRev);
  if (f) {
    int16_t i = LOOT_IndexOf(LOOT_Get(f));
    ctx.s_selIdx = i < 0 ? 0 : i;
  }
  ctx.s_top = 0;
  ctx.s_dirty = true;
}

class LootListMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("");
    ctx.s_keep = false;
    ctx.s_saved = Vfo_Capture();

    UI_ReleaseLabels();
    UI_ClearScreen(BG());
    Radio_Init();

    ctx.s_scr = lvBox(lv_scr_act(), 0, 0, LCD_WIDTH, LCD_HEIGHT, C_BLACK,
                  LV_OPA_TRANSP);
    ctx.s_sel = lvBox(ctx.s_scr, 0, LIST_Y, LCD_WIDTH, ROW_H, C_BLACK, LV_OPA_COVER);
    if (ctx.s_sel)
      lv_obj_set_style_bg_color(ctx.s_sel, lootSelColor(), LV_PART_MAIN);
    ctx.s_title = lvLabel(ctx.s_scr, F_M, C_YELLOW, 4, TITLE_Y, LCD_WIDTH - 8,
                      LV_TEXT_ALIGN_LEFT);
    ctx.s_list = lvLabel(ctx.s_scr, F_M, C_WHITE, 4, LIST_Y, LCD_WIDTH - 8,
                     LV_TEXT_ALIGN_LEFT);
    if (ctx.s_list)
      lv_label_set_recolor(ctx.s_list, true);
    ctx.s_detail = lvLabel(ctx.s_scr, F_M, C_CYAN, 4, DETAIL_Y, LCD_WIDTH - 8,
                       LV_TEXT_ALIGN_LEFT);
    ctx.s_hint = lvLabel(ctx.s_scr, F_S, C_GRAY, 4, HINT_Y, LCD_WIDTH - 8,
                     LV_TEXT_ALIGN_LEFT);
    if (ctx.s_hint)
      lv_label_set_text_static(
          ctx.s_hint, "1 TIME 2 DUR 3 BL 4 FREQ\nS1 BLK S2 WHT 0 DEL L5/8 CH L9 LOAD");

    if (!ctx.s_scr || !ctx.s_sel || !ctx.s_title || !ctx.s_list || !ctx.s_detail)
      return; /* куча LVGL исчерпана — экран пустой, но без падения */

    clampSel();
    ctx.s_dirty = true;
    refresh();
    if (LOOT_Size())
      tuneSel();
  }

  void exit() override {
    if (ctx.s_scr) {
      lv_obj_del(ctx.s_scr);
      ctx.s_scr = ctx.s_sel = ctx.s_title = ctx.s_list = ctx.s_detail = ctx.s_hint = NULL;
    }
    if (!ctx.s_keep)
      Vfo_Restore(ctx.s_saved, false);
  }

  void update() override { Radio_Update(); /* шумодав/звук выбранной */ }

  void render() override {
    STATUSLINE_render();
    if (!ctx.s_scr || !ctx.s_list)
      return;
    if (ctx.s_msg[0] && (int32_t)(ctx.s_msgUntil - millis()) <= 0)
      ctx.s_dirty = true;
    /* раз в секунду (время «назад») и при смене размера/состояния */
    if (ctx.s_dirty || LOOT_Size() != ctx.s_lastCount ||
        (uint16_t)(millis() / 1000) != ctx.s_lastSec)
      refresh();
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (!ctx.s_scr || !ctx.s_list)
      return false;

    if (state == KEY_LONG_PRESSED) {
      if (k == KEY_0) { /* очистить всё */
        LOOT_Clear();
        ctx.s_selIdx = ctx.s_top = 0;
        ctx.s_dirty = true;
        return true;
      }
      if (k == KEY_5) { /* whitelist → каналы */
        flashMsg("SAVED", CH_SaveWhitelistFromLoot());
        return true;
      }
      if (k == KEY_8) { /* blacklist → каналы с флагом blacklist */
        flashMsg("SAVED", CH_SaveBlacklistFromLoot());
        return true;
      }
      if (k == KEY_9) { /* каналы blacklist → loot, вручную */
        flashMsg("LOADED", CH_LoadBlacklistToLoot());
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
      LOOT_ToggleBlacklist(LOOT_Item(ctx.s_selIdx));
      ctx.s_dirty = true;
      return true;
    case KEY_SIDE2:
      LOOT_ToggleWhitelist(LOOT_Item(ctx.s_selIdx));
      ctx.s_dirty = true;
      return true;
    case KEY_0:
      if (LOOT_Size()) {
        LOOT_Remove(ctx.s_selIdx);
        ctx.s_dirty = true;
        clampSel();
        tuneSel();
      }
      return true;
    case KEY_MENU: /* выбранную — в VFO */
      if (LOOT_Size()) {
        tuneSel();
        ctx.s_keep = true;
        Mode_Switch(MODE_VFO);
      }
      return true;
    case KEY_EXIT:
      Mode_Switch(ctx.s_prev ? *ctx.s_prev : MODE_VFO);
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
  ctx.s_prev = &from;
  Mode_Switch(MODE_LOOTLIST);
}
