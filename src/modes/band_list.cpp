/* Список диапазонов: просмотр и правка плана, который лежит во флеше.
 *
 * Вход — VFO → длинный 8. Заводской PLAN при первом изменении копируется
 * в пользовательский список (src/bands.cpp) и дальше читается из кольца
 * W25Q32; пустая флеш = заводские диапазоны. Правка применяется сразу,
 * запись — через 2,5 с без изменений.
 *
 * Список:
 *   UP/DOWN — выбор; MENU — правка; длинный 5 — добавить в районе частоты
 *   VFO (до входа); длинный 0 — удалить; STAR — loot; EXIT — в VFO.
 *
 * Форма (8 полей):
 *   UP/DOWN — поле; SIDE1/SIDE2 — изменить значение; MENU — правка поля
 *   (границы: сначала начало, затем конец, как в анализаторе; имя — мульти-
 *   тап 2..9, 1 — символы, 0 — пробел/0; коррекция — цифры, знак ±);
 *   EXIT — назад к списку. Изменения сохраняются после паузы.
 */
#include "modes.h"
#include "vfo_session.h"
#include "../bands.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../helper/names.h"
#include "../helper/unit_fmt.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/band_colors.h"
#include "../ui/graphics.h"
#include "../ui/lv_helpers.h"
#include "../ui/numval.h"
#include "../ui/statusline.h"
#include "lvgl.h"
#include "mode.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TITLE_Y 26
#define LIST_Y 42
#define ROW_H 16 /* line_height шрифта F_M */
#define ROWS 13
#define DETAIL_Y (LIST_Y + ROWS * ROW_H + 3)
#define HINT_Y (DETAIL_Y + 18)
#define VALUE_X 68 /* 4 + 8 символов F_M (8 px) */

#define EDIT_BLINK_MS 500
#define NAME_TAP_MS 900
#define MSG_MS 1200

typedef enum {
  FIELD_RANGE,
  FIELD_NAME,
  FIELD_STEP,
  FIELD_MOD,
  FIELD_SQL,
  FIELD_CORR,
  FIELD_COLOR,
  FIELD_CAT,
  FIELD_COUNT
} BandField;

typedef enum { VIEW_LIST, VIEW_FORM } BandView;
typedef enum { EDIT_NONE, EDIT_START, EDIT_END, EDIT_CORR, EDIT_NAME } EditKind;

static const char *const FIELD_LABELS[FIELD_COUNT] = {
    "Range", "Name", "Step", "Mod", "SQL", "Corr", "Color", "Cat"};

/* Мульти-тап: цифра → группа символов (верхний регистр для новых имён). */
static const char *const TAP_CHARS[10] = {
    " 0",  "-_./", "ABC2", "DEF3", "GHI4", "JKL5",
    "MNO6", "PQRS7", "TUV8", "WXYZ9"};

static struct Ctx {
  lv_obj_t *scr;
  lv_obj_t *sel;
  lv_obj_t *title;
  lv_obj_t *list;
  lv_obj_t *detail;
  lv_obj_t *hint;

  uint8_t selIdx;
  uint8_t top;
  bool dirty;
  BandView view;
  BandField field;
  EditKind edit;
  bool pendingEnd; /* начало введено — открыть редактор конца */

  NumVal_t nv;

  char nameBuf[BAND_NAME_LEN];
  uint8_t nameCur;
  bool nameBlink;
  uint32_t nameBlinkAt;
  uint32_t nameTapAt;
  uint8_t nameTapKey;

  VfoSnapshot saved;
  char msg[16];
  uint32_t msgUntil;

  char text[ROWS * 44];
  char titleBuf[32];
  char detailBuf[52];
} ctx;

static BandRec *rec(void) {
  return ctx.selIdx < Band_Count() ? Band_RecEdit(ctx.selIdx) : nullptr;
}

static void setMsg(const char *m) {
  snprintf(ctx.msg, sizeof(ctx.msg), "%s", m);
  ctx.msgUntil = millis() + MSG_MS;
  ctx.dirty = true;
}

static const char *catName(uint8_t cat) {
  switch (cat) {
  case CAT_BC: return "BC";
  case CAT_HAM: return "HAM";
  case CAT_SVC: return "SVC";
  default: return "-";
  }
}

static void fmtKhz(char *b, uint8_t cap, uint32_t khz) {
  fmt_mhz3(b, cap, khz * 100u);
}

static void fmtCorr(char *b, uint8_t cap, int16_t corr10) {
  if (corr10 < 0)
    snprintf(b, cap, "-%luHz",
             (unsigned long)(-(int32_t)corr10) * 10UL);
  else
    snprintf(b, cap, "+%luHz", (unsigned long)corr10 * 10UL);
}

/* Имя с курсором: '_' на месте вставки, мигает. */
static void nameDisplay(char *b, uint8_t cap) {
  uint8_t n = (uint8_t)strlen(ctx.nameBuf);
  uint8_t cur = ctx.nameCur > n ? n : ctx.nameCur;
  uint8_t o = 0;
  for (uint8_t i = 0; i < n && o + 2 < cap; ++i) {
    if (i == cur)
      b[o++] = ctx.nameBlink ? '_' : ' ';
    b[o++] = ctx.nameBuf[i];
  }
  if (cur >= n && o + 1 < cap)
    b[o++] = ctx.nameBlink ? '_' : ' ';
  b[o] = 0;
}

/* ─── Правка полей ─── */

static uint16_t listY(BandField f) { return (uint16_t)(LIST_Y + f * ROW_H); }

static void onStartDone(uint32_t v10);
static void onEndDone(uint32_t v10);
static void onCorrDone(uint32_t hz);

static void startBoundEdit(bool start) {
  BandRec *r = rec();
  if (!r)
    return;
  uint32_t v = (start ? r->startKhz : r->endKhz) * 100u;
  NumVal_Init(&ctx.nv, VALUE_X, (uint16_t)(listY(FIELD_RANGE) + FontAscent(F_M)),
              F_M, 8, 12, UNIT_MHZ, v, 10000, 134000000, nullptr, POS_L,
              nullptr, 0, 0);
  ctx.nv.onChange = start ? onStartDone : onEndDone;
  ctx.edit = start ? EDIT_START : EDIT_END;
  ctx.pendingEnd = false;
  NumVal_StartEdit(&ctx.nv);
  ctx.dirty = true;
}

static void startCorrEdit(void) {
  BandRec *r = rec();
  if (!r)
    return;
  uint32_t mag = r->corr10 < 0 ? (uint32_t)(-(int32_t)r->corr10)
                               : (uint32_t)r->corr10;
  NumVal_Init(&ctx.nv, VALUE_X, (uint16_t)(listY(FIELD_CORR) + FontAscent(F_M)),
              F_M, 8, 12, UNIT_HZ, mag * 10u, 0, 100000, nullptr, POS_L,
              nullptr, 0, 0);
  ctx.nv.onChange = onCorrDone;
  ctx.edit = EDIT_CORR;
  NumVal_StartEdit(&ctx.nv);
  ctx.dirty = true;
}

static void onStartDone(uint32_t v10) {
  BandRec *r = rec();
  if (!r)
    return;
  uint32_t k = v10 / 100u;
  if (k < 1)
    k = 1;
  r->startKhz = k;
  if (r->endKhz <= k + 1) {
    r->endKhz = k + 100; /* 100 кГц по умолчанию */
    if (r->endKhz > 1340000u)
      r->endKhz = 1340000u;
    if (r->endKhz <= k)
      r->startKhz = r->endKhz - 1;
  }
  Band_Touch();
  ctx.pendingEnd = true; /* конец откроем после возврата из NumVal_Key */
}

static void onEndDone(uint32_t v10) {
  BandRec *r = rec();
  if (!r)
    return;
  uint32_t k = v10 / 100u;
  if (k > r->startKhz + 1) {
    r->endKhz = k;
    Band_Touch();
  }
}

static void onCorrDone(uint32_t hz) {
  BandRec *r = rec();
  if (!r)
    return;
  int8_t sign = r->corr10 < 0 ? -1 : 1;
  int32_t c = (int32_t)(hz / 10u);
  if (c > 32767)
    c = 32767;
  r->corr10 = (int16_t)(sign < 0 ? -c : c);
  Band_Touch();
}

static void startNameEdit(void) {
  BandRec *r = rec();
  if (!r)
    return;
  strncpy(ctx.nameBuf, r->name, BAND_NAME_LEN - 1);
  ctx.nameBuf[BAND_NAME_LEN - 1] = 0;
  ctx.nameCur = (uint8_t)strlen(ctx.nameBuf);
  ctx.nameBlink = true;
  ctx.nameBlinkAt = millis();
  ctx.nameTapKey = KEY_NONE;
  ctx.edit = EDIT_NAME;
  ctx.dirty = true;
}

static void commitName(void) {
  BandRec *r = rec();
  if (!r)
    return;
  for (int i = (int)strlen(ctx.nameBuf) - 1; i >= 0 && ctx.nameBuf[i] == ' '; --i)
    ctx.nameBuf[i] = 0;
  if (!ctx.nameBuf[0])
    snprintf(ctx.nameBuf, sizeof(ctx.nameBuf), "BAND");
  strncpy(r->name, ctx.nameBuf, BAND_NAME_LEN - 1);
  r->name[BAND_NAME_LEN - 1] = 0;
  Band_Touch();
  ctx.edit = EDIT_NONE;
  ctx.dirty = true;
}

static void cancelEdit(void) {
  ctx.pendingEnd = false;
  ctx.edit = EDIT_NONE;
  NumVal_Cancel(&ctx.nv);
  lv_refr_now(NULL);
  FillRectNoInv(0, LIST_Y, LCD_WIDTH, FIELD_COUNT * ROW_H, BG());
  UI_ReleaseLabels();
  if (ctx.sel)
    lv_obj_invalidate(ctx.sel);
  ctx.dirty = true;
}

/* ─── Список / форма ─── */

/* Подсветка выбранной строки: в светлой теме фиксированный голубой,
 * не пропускаем через TC(). */
static lv_color_t selColor(void) {
  if (gThemeLight)
    return lv_color_make(190, 215, 255);
  Color c = (Color)(((0 & 0xF8) << 8) | ((40 & 0xFC) << 3) | (96 >> 3));
  return lvColor(c);
}

static void clampTop(void) {
  if (ctx.selIdx < ctx.top)
    ctx.top = ctx.selIdx;
  if (ctx.selIdx >= ctx.top + ROWS)
    ctx.top = (uint16_t)(ctx.selIdx - ROWS + 1);
  if (ctx.top + ROWS > Band_Count())
    ctx.top = Band_Count() > ROWS ? (uint8_t)(Band_Count() - ROWS) : 0;
}

static char *append(char *p, char *end, const char *fmt, ...) {
  if (p >= end)
    return p;
  va_list a;
  va_start(a, fmt);
  int w = vsnprintf(p, (size_t)(end - p), fmt, a);
  va_end(a);
  if (w < 0)
    return p;
  if (w >= end - p)
    return end;
  return p + w;
}

static void refreshList(void) {
  uint8_t n = Band_Count();
  snprintf(ctx.titleBuf, sizeof(ctx.titleBuf), "BANDS %u/%u",
           (unsigned)(n ? ctx.selIdx + 1 : 0), (unsigned)n);
  lv_label_set_text_static(ctx.title, ctx.titleBuf);

  char *p = ctx.text;
  char *end = ctx.text + sizeof(ctx.text);
  p = append(p, end, "");
  const bool L = gThemeLight;
  for (uint8_t r = 0; r < ROWS && ctx.top + r < n; ++r) {
    const BandRec *b = Band_Rec(ctx.top + r);
    if (!b)
      continue;
    char s[12], e[12];
    fmtKhz(s, sizeof(s), b->startKhz);
    fmtKhz(e, sizeof(e), b->endKhz);
    p = append(p, end, "#%s %-7.7s %s-%s %s#\n", Band_PaletteHex(b->color, L),
               b->name, s, e, MOD_NAMES[b->mod & 7]);
  }
  lv_label_set_text_static(ctx.list, ctx.text);

  if (n) {
    lv_obj_clear_flag(ctx.sel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(ctx.sel, 0, LIST_Y + (ctx.selIdx - ctx.top) * ROW_H);
  } else {
    lv_obj_add_flag(ctx.sel, LV_OBJ_FLAG_HIDDEN);
  }

  const BandRec *b = Band_Rec(ctx.selIdx);
  if (ctx.msg[0]) {
    snprintf(ctx.detailBuf, sizeof(ctx.detailBuf), "%s", ctx.msg);
  } else if (b) {
    char cs[16];
    fmtCorr(cs, sizeof(cs), b->corr10);
    if (b->squelch == BAND_SQL_NONE)
      snprintf(ctx.detailBuf, sizeof(ctx.detailBuf), "S %s SQL KEEP C %s %s",
               b->step < STEP_COUNT ? STEP_NAMES[b->step] : "?", cs,
               catName(b->cat));
    else
      snprintf(ctx.detailBuf, sizeof(ctx.detailBuf), "S %s SQL %u C %s %s",
               b->step < STEP_COUNT ? STEP_NAMES[b->step] : "?",
               (unsigned)b->squelch, cs, catName(b->cat));
  } else {
    ctx.detailBuf[0] = ' ';
    ctx.detailBuf[1] = 0;
  }
  lv_label_set_text_static(ctx.detail, ctx.detailBuf);
  lv_label_set_text_static(ctx.hint,
                           "U/D SEL  M>EDIT  L5 ADD\nL0 DEL  * LOOT  E>VFO");
}

static void refreshForm(void) {
  const BandRec *b = Band_Rec(ctx.selIdx);
  if (!b) {
    refreshList();
    return;
  }
  snprintf(ctx.titleBuf, sizeof(ctx.titleBuf), "EDIT %.12s", b->name);
  lv_label_set_text_static(ctx.title, ctx.titleBuf);

  char *p = ctx.text;
  char *end = ctx.text + sizeof(ctx.text);
  p = append(p, end, "");

  for (uint8_t f = 0; f < FIELD_COUNT; ++f) {
    char v[64];
    v[0] = 0;
    switch (f) {
    case FIELD_RANGE:
      if (!(ctx.edit == EDIT_START || ctx.edit == EDIT_END ||
            ctx.pendingEnd)) {
        char s[12], e[12];
        fmtKhz(s, sizeof(s), b->startKhz);
        fmtKhz(e, sizeof(e), b->endKhz);
        snprintf(v, sizeof(v), "%s-%s", s, e);
      }
      break;
    case FIELD_NAME:
      if (ctx.edit == EDIT_NAME)
        nameDisplay(v, sizeof(v));
      else
        snprintf(v, sizeof(v), "#%s %s#", Band_PaletteHex(b->color, gThemeLight),
                 b->name);
      break;
    case FIELD_STEP:
      snprintf(v, sizeof(v), "%s",
               b->step < STEP_COUNT ? STEP_NAMES[b->step] : "?");
      break;
    case FIELD_MOD:
      snprintf(v, sizeof(v), "%s", MOD_NAMES[b->mod & 7]);
      break;
    case FIELD_SQL:
      if (b->squelch == BAND_SQL_NONE)
        snprintf(v, sizeof(v), "KEEP");
      else
        snprintf(v, sizeof(v), "%u", (unsigned)b->squelch);
      break;
    case FIELD_CORR:
      if (!(ctx.edit == EDIT_CORR))
        fmtCorr(v, sizeof(v), b->corr10);
      break;
    case FIELD_COLOR:
      snprintf(v, sizeof(v), "#%s %s#", Band_PaletteHex(b->color, gThemeLight),
               Band_PaletteName(b->color));
      break;
    case FIELD_CAT:
      snprintf(v, sizeof(v), "%s", catName(b->cat));
      break;
    }
    p = append(p, end, "%-7s %s\n", FIELD_LABELS[f], v);
  }
  lv_label_set_text_static(ctx.list, ctx.text);

  lv_obj_clear_flag(ctx.sel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(ctx.sel, 0, listY(ctx.field));

  if (ctx.msg[0]) {
    snprintf(ctx.detailBuf, sizeof(ctx.detailBuf), "%s", ctx.msg);
  } else {
    ctx.detailBuf[0] = ' ';
    ctx.detailBuf[1] = 0;
  }
  lv_label_set_text_static(ctx.detail, ctx.detailBuf);

  if (ctx.edit == EDIT_NAME)
    lv_label_set_text_static(ctx.hint,
                             "2-9 MULTI 1 SYM 0 SP  U/D CURSOR\n*<X  M/# OK  E>BACK");
  else if (ctx.edit == EDIT_START || ctx.edit == EDIT_END ||
           ctx.edit == EDIT_CORR)
    lv_label_set_text_static(ctx.hint,
                             "DIGITS  M/# OK  E<X  (long E CANCEL)");
  else
    lv_label_set_text_static(ctx.hint,
                             "U/D FIELD  S1/S2 CHG  M>EDIT\nE>BACK  (changes auto-save)");
}

static void moveSel(int8_t d) {
  uint8_t n = Band_Count();
  if (!n)
    return;
  if (d < 0)
    ctx.selIdx = ctx.selIdx ? (uint8_t)(ctx.selIdx - 1) : (uint8_t)(n - 1);
  else
    ctx.selIdx = (uint8_t)((ctx.selIdx + 1) % n);
  clampTop();
  ctx.dirty = true;
}

static void refresh(void) {
  if (ctx.view == VIEW_FORM)
    refreshForm();
  else
    refreshList();
  ctx.dirty = false;
}

/* ─── Действия ─── */

static void openForm(void) {
  if (!Band_Rec(ctx.selIdx))
    return;
  ctx.view = VIEW_FORM;
  ctx.field = FIELD_RANGE;
  ctx.edit = EDIT_NONE;
  ctx.pendingEnd = false;
  ctx.dirty = true;
}

static void closeForm(void) {
  cancelEdit();
  ctx.view = VIEW_LIST;
  ctx.edit = EDIT_NONE;
  clampTop();
  ctx.dirty = true;
}

static void addBand(void) {
  uint32_t f = ctx.saved.freq;
  if (f < 100)
    f = 14550000;
  uint32_t start = f / 100u;
  if (start < 1)
    start = 1;
  uint32_t end = start + 100;
  if (end > 1340000u) {
    end = 1340000u;
    start = end - 100;
  }

  BandRec r;
  memset(&r, 0, sizeof(r));
  r.startKhz = start;
  r.endKhz = end;
  const BandPlan *cur = Band_Find(f);
  r.mod = cur ? cur->mod : ctx.saved.mod;
  r.step = cur ? cur->step : ctx.saved.step;
  r.cat = cur ? cur->cat : (r.mod == MOD_LSB || r.mod == MOD_USB ? CAT_HAM
                            : (r.mod == MOD_AM || r.mod == MOD_WFM ? CAT_BC
                                                                   : CAT_SVC));
  r.color = Band_Count() ? (uint8_t)((Band_Rec(Band_Count() - 1)->color + 1) & 7)
                         : 2;
  r.squelch = BAND_SQL_NONE;
  snprintf(r.name, sizeof(r.name), "NEW");

  int16_t idx = Band_Add(&r);
  if (idx < 0) {
    setMsg("FULL");
    return;
  }
  ctx.selIdx = (uint8_t)idx;
  clampTop();
  openForm();
  setMsg("ADDED");
}

static void deleteBand(void) {
  if (Band_Count() <= 1) {
    setMsg("LAST");
    return;
  }
  if (!Band_Delete(ctx.selIdx)) {
    setMsg("ERR");
    return;
  }
  if (ctx.selIdx >= Band_Count())
    ctx.selIdx = Band_Count() ? (uint8_t)(Band_Count() - 1) : 0;
  clampTop();
  setMsg("DELETED");
}

static void changeField(int8_t dir) {
  BandRec *b = rec();
  if (!b)
    return;
  switch (ctx.field) {
  case FIELD_STEP:
    b->step = (uint8_t)((b->step + STEP_COUNT + dir) % STEP_COUNT);
    break;
  case FIELD_MOD:
    b->mod = (uint8_t)((b->mod + (MOD_WFM + 1) + dir) % (MOD_WFM + 1));
    break;
  case FIELD_SQL: {
    /* KEEP → 0..10 → KEEP */
    int v = b->squelch == BAND_SQL_NONE ? -1 : (int)b->squelch;
    v += dir;
    if (v < 0)
      v = 10;
    if (v > 10)
      v = -1;
    b->squelch = v < 0 ? BAND_SQL_NONE : (uint8_t)v;
    break;
  }
  case FIELD_CORR: {
    int32_t c = (int32_t)b->corr10 + (int32_t)dir * 10; /* ±100 Гц */
    if (c > 32767)
      c = 32767;
    if (c < -32767)
      c = -32767;
    b->corr10 = (int16_t)c;
    break;
  }
  case FIELD_COLOR:
    b->color = (uint8_t)((b->color + BAND_COLOR_COUNT + dir) % BAND_COLOR_COUNT);
    break;
  case FIELD_CAT: {
    /* BC → HAM → SVC → NONE → BC */
    int v;
    if (b->cat == CAT_NONE)
      v = CAT_COUNT;
    else
      v = b->cat;
    v = (v + CAT_COUNT + 1 + dir) % (CAT_COUNT + 1);
    b->cat = v >= CAT_COUNT ? CAT_NONE : (uint8_t)v;
    break;
  }
  default:
    return;
  }
  Band_Touch();
  ctx.dirty = true;
}

/* ─── Имя ─── */

static bool nameKey(KEY_Code_t k, Key_State_t state) {
  if (state == KEY_LONG_PRESSED_CONT)
    return true;
  if (state != KEY_RELEASED)
    return true;

  if (k >= KEY_0 && k <= KEY_9) {
    uint8_t ki = (uint8_t)(k - KEY_0);
    const char *g = TAP_CHARS[ki];
    size_t gl = strlen(g);
    if (ctx.nameTapKey == (uint8_t)k && millis() - ctx.nameTapAt < NAME_TAP_MS &&
        ctx.nameCur > 0) {
      char *q = strchr(g, ctx.nameBuf[ctx.nameCur - 1]);
      char next = q && q[1] ? q[1] : g[0];
      ctx.nameBuf[ctx.nameCur - 1] = next;
    } else if (ctx.nameCur < BAND_NAME_LEN - 1) {
      ctx.nameBuf[ctx.nameCur++] = gl ? g[0] : ' ';
      ctx.nameBuf[ctx.nameCur] = 0;
    }
    ctx.nameTapKey = (uint8_t)k;
    ctx.nameTapAt = millis();
    ctx.nameBlink = true;
    ctx.nameBlinkAt = millis();
    ctx.dirty = true;
    return true;
  }

  switch (k) {
  case KEY_UP:
    if (ctx.nameCur > 0)
      ctx.nameCur--;
    ctx.dirty = true;
    return true;
  case KEY_DOWN:
    if (ctx.nameCur < (uint8_t)strlen(ctx.nameBuf))
      ctx.nameCur++;
    ctx.dirty = true;
    return true;
  case KEY_STAR: /* backspace */
    if (ctx.nameCur > 0) {
      memmove(&ctx.nameBuf[ctx.nameCur - 1], &ctx.nameBuf[ctx.nameCur],
              strlen(&ctx.nameBuf[ctx.nameCur]) + 1);
      ctx.nameCur--;
      ctx.dirty = true;
    }
    return true;
  case KEY_MENU:
  case KEY_HASH:
  case KEY_PTT:
    commitName();
    return true;
  case KEY_EXIT:
    ctx.edit = EDIT_NONE;
    ctx.dirty = true;
    return true;
  default:
    return true;
  }
}

/* ─── Режим ─── */

class BandListMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("BANDS");
    ctx.saved = Vfo_Capture();
    ctx.view = VIEW_LIST;
    ctx.edit = EDIT_NONE;
    ctx.pendingEnd = false;
    ctx.msg[0] = 0;
    ctx.selIdx = 0;
    ctx.top = 0;

    UI_ReleaseLabels();
    UI_ClearScreen(BG());

    ctx.scr = lvBox(lv_scr_act(), 0, 0, LCD_WIDTH, LCD_HEIGHT, C_BLACK,
                    LV_OPA_TRANSP);
    ctx.sel = lvBox(ctx.scr, 0, LIST_Y, LCD_WIDTH, ROW_H, C_BLACK,
                    LV_OPA_COVER);
    if (ctx.sel)
      lv_obj_set_style_bg_color(ctx.sel, selColor(), LV_PART_MAIN);
    ctx.title = lvLabel(ctx.scr, F_M, C_YELLOW, 4, TITLE_Y, LCD_WIDTH - 8,
                        LV_TEXT_ALIGN_LEFT);
    ctx.list = lvLabel(ctx.scr, F_M, C_WHITE, 4, LIST_Y, LCD_WIDTH - 8,
                       LV_TEXT_ALIGN_LEFT);
    if (ctx.list)
      lv_label_set_recolor(ctx.list, true);
    ctx.detail = lvLabel(ctx.scr, F_M, C_CYAN, 4, DETAIL_Y, LCD_WIDTH - 8,
                         LV_TEXT_ALIGN_LEFT);
    ctx.hint = lvLabel(ctx.scr, F_S, C_GRAY, 4, HINT_Y, LCD_WIDTH - 8,
                       LV_TEXT_ALIGN_LEFT);

    if (!ctx.scr || !ctx.sel || !ctx.title || !ctx.list || !ctx.detail ||
        !ctx.hint)
      return;

    clampTop();
    refresh();
  }

  void exit() override {
    cancelEdit();
    if (ctx.scr) {
      lv_obj_del(ctx.scr);
      ctx.scr = ctx.sel = ctx.title = ctx.list = ctx.detail = ctx.hint = NULL;
    }
  }

  void update() override {
    if (ctx.msg[0] && (int32_t)(ctx.msgUntil - millis()) <= 0) {
      ctx.msg[0] = 0;
      ctx.dirty = true;
    }
    if (ctx.edit == EDIT_NAME &&
        millis() - ctx.nameBlinkAt >= EDIT_BLINK_MS) {
      ctx.nameBlink = !ctx.nameBlink;
      ctx.nameBlinkAt = millis();
      ctx.dirty = true;
    }
    if (ctx.edit == EDIT_START || ctx.edit == EDIT_END ||
        ctx.edit == EDIT_CORR)
      NumVal_Update(&ctx.nv);
  }

  void render() override {
    STATUSLINE_render();
    if (!ctx.scr || !ctx.list)
      return;
    if (ctx.dirty)
      refresh();
    if (ctx.edit == EDIT_START || ctx.edit == EDIT_END ||
        ctx.edit == EDIT_CORR)
      NumVal_Render(&ctx.nv);
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (!ctx.scr || !ctx.list)
      return false;

    /* Имя забирает все клавиши, пока редактор открыт. */
    if (ctx.edit == EDIT_NAME)
      return nameKey(k, state);

    /* Числовой ввод NumVal: цифры/точка/подтверждение/отмена. */
    if (NumVal_IsEditing(&ctx.nv) || ctx.pendingEnd) {
      if (ctx.pendingEnd) {
        /* предыдущее поле подтверждено — открываем конец */
        startBoundEdit(false);
        if (state == KEY_RELEASED && k >= KEY_0 && k <= KEY_9) {
          NumVal_Key(&ctx.nv, k, state);
          return true;
        }
        return true;
      }
      if (NumVal_Key(&ctx.nv, k, state)) {
        if (!NumVal_IsEditing(&ctx.nv)) {
          if (ctx.pendingEnd)
            startBoundEdit(false);
          else
            cancelEdit(); /* затереть курсор и старый NumVal-лейбл */
        }
        return true;
      }
      return true;
    }

    if (ctx.view == VIEW_FORM) {
      if (state == KEY_LONG_PRESSED_CONT) {
        if (k == KEY_UP || k == KEY_DOWN) {
          ctx.field = (BandField)((ctx.field + FIELD_COUNT +
                                   (k == KEY_DOWN ? 1 : FIELD_COUNT - 1)) %
                                  FIELD_COUNT);
          ctx.dirty = true;
          return true;
        }
        if (k == KEY_SIDE1 || k == KEY_SIDE2) {
          changeField(k == KEY_SIDE1 ? +1 : -1);
          return true;
        }
        return true;
      }
      if (state != KEY_RELEASED)
        return true;

      switch (k) {
      case KEY_UP:
        ctx.field = (BandField)((ctx.field + FIELD_COUNT - 1) % FIELD_COUNT);
        ctx.dirty = true;
        return true;
      case KEY_DOWN:
        ctx.field = (BandField)((ctx.field + 1) % FIELD_COUNT);
        ctx.dirty = true;
        return true;
      case KEY_SIDE1:
        changeField(+1);
        return true;
      case KEY_SIDE2:
        changeField(-1);
        return true;
      case KEY_MENU:
        if (ctx.field == FIELD_RANGE) {
          startBoundEdit(true);
        } else if (ctx.field == FIELD_NAME) {
          startNameEdit();
        } else if (ctx.field == FIELD_CORR) {
          startCorrEdit();
        } else {
          changeField(+1);
        }
        return true;
      case KEY_0:
      case KEY_1:
      case KEY_2:
      case KEY_3:
      case KEY_4:
      case KEY_5:
      case KEY_6:
      case KEY_7:
      case KEY_8:
      case KEY_9:
        if (ctx.field == FIELD_RANGE) {
          startBoundEdit(true);
          NumVal_Key(&ctx.nv, k, state);
        } else if (ctx.field == FIELD_CORR) {
          startCorrEdit();
          NumVal_Key(&ctx.nv, k, state);
        } else if (ctx.field == FIELD_NAME) {
          startNameEdit();
          nameKey(k, state);
        }
        return true;
      case KEY_EXIT:
        closeForm();
        return true;
      default:
        return true;
      }
    }

    /* Список */
    if (state == KEY_LONG_PRESSED_CONT) {
      if (k == KEY_UP || k == KEY_DOWN) {
        moveSel(k == KEY_UP ? -1 : +1);
        return true;
      }
      return false;
    }

    if (state == KEY_LONG_PRESSED) {
      switch (k) {
      case KEY_5:
        addBand();
        return true;
      case KEY_0:
        deleteBand();
        return true;
      default:
        break;
      }
      return false;
    }

    if (state != KEY_RELEASED)
      return false;

    switch (k) {
    case KEY_UP:
      moveSel(-1);
      return true;
    case KEY_DOWN:
      moveSel(+1);
      return true;
    case KEY_MENU:
      openForm();
      return true;
    case KEY_STAR:
      LOOTLIST_OpenFrom(MODE_BANDLIST);
      return true;
    case KEY_EXIT:
      Mode_Switch(MODE_VFO);
      return true;
    default:
      return false;
    }
  }
};

static BandListMode bandListInstance;
Mode &MODE_BANDLIST = bandListInstance;
