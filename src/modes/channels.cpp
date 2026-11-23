/* Канальный режим: список сохранённых каналов и скан по ним.
 *
 * Вход — VFO → длинный 7. Каналы лежат в W25Q32 (src/helper/channels.cpp),
 * в RAM только индексы видимого списка. Экран — нативный LVGL, как в
 * loot_screen.cpp: заголовок, один многострочный label с recolor,
 * подложка выбранной строки, детали и подсказка.
 *
 * Клавиши (список / пауза):
 *   UP/DOWN   — выбор канала (приёмник настраивается на него);
 *   1..8, 0   — сканлист: биты 1..8, 0 — ALL;
 *   MENU      — выбранный канал в VFO и выход;
 *   длинный MENU — включить скан;
 *   SIDE1     — blacklist вкл/выкл у выбранного;
 *   длинный 5 — добавить частоту VFO (на момент входа) в каналы;
 *   длинный 0 — удалить выбранный;
 *   длинный 9 — чёрный список каналов в loot (вручную);
 *   EXIT      — в VFO, снимок VFO восстанавливается.
 *
 * В скане:
 *   длинный MENU — остановить скан; MENU — текущий канал в VFO и выход;
 *   UP/DOWN — пауза и ручной переход; 1..8/0 — сменить сканлист;
 *   SIDE1 — текущий канал в blacklist и дальше; EXIT — в VFO.
 */
#include "modes.h"
#include "vfo_session.h"
#include "../bands.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/si473x.h"
#include "../helper/channels.h"
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
#define ROW_H 16 /* line_height шрифта F_M */
#define ROWS 13
#define DETAIL_Y (LIST_Y + ROWS * ROW_H + 3)
#define HINT_Y (DETAIL_Y + 18)

#define SETTLE_MS 40     /* пауза после перестройки (как RX_SETTLE_MS в BAND) */
#define SQL_DELAY_MS 30  /* подтверждение открытия повторным замером */
#define SQL_MARGIN 10    /* ~5 дБ над шумом (RSSI: 2 ед. = 1 дБ) */
#define SQL_HYST 4
#define LISTEN_TIMEOUT_MS 5000
#define RX_SNR_THR 6 /* порог станции SI, дБ */

typedef enum { PH_IDLE, PH_SETTLE, PH_CONFIRM, PH_HOLD } ScanPhase;

static struct Ctx {
  lv_obj_t *s_scr;
  lv_obj_t *s_sel;
  lv_obj_t *s_title;
  lv_obj_t *s_list;
  lv_obj_t *s_detail;
  lv_obj_t *s_hint;
  uint8_t s_view[CH_MAX]; /* индексы хранения видимого списка */
  uint16_t s_viewCount;
  uint16_t s_selIdx;
  uint16_t s_top;
  bool s_dirty;
  bool s_keep;     /* MENU: оставить выбранный канал в VFO */
  bool s_menuLong; /* длинный MENU уже отработал — отпускание игнорируем */
  bool s_scanning;
  bool s_hold;
  bool s_hintScan;
  ScanPhase s_phase;
  uint32_t s_nextAt;
  uint32_t s_holdUntil;
  uint16_t s_floorRssi;
  uint16_t s_confirmThr;
  uint16_t s_calibMin;
  uint16_t s_calibSeen;
  uint16_t s_scanCount;
  bool s_calib;
  char s_msg[12];
  uint32_t s_msgUntil;
  VfoSnapshot s_saved;
  char s_listBuf[ROWS * 32];
  char s_titleBuf[40];
  char s_detailBuf[48];
} ctx;

/* ─── Список ─── */

static void maskStr(char *b, uint8_t cap, uint16_t mask) {
  if (!mask) {
    snprintf(b, cap, "ALL");
    return;
  }
  char *p = b;
  char *end = b + cap;
  p[0] = 0;
  for (uint8_t i = 0; i < CH_SCANLIST_COUNT && p < end; ++i) {
    if (!(mask & (1u << i)))
      continue;
    int w = snprintf(p, end - p, "%s%u", p == b ? "" : ".", (unsigned)(i + 1));
    if (w < 0 || w >= end - p)
      break;
    p += w;
  }
}

static const char *stateStr(void) {
  if (!ctx.s_scanning)
    return "PAUSE";
  return ctx.s_hold ? "HOLD" : "SCAN";
}

/* Подсветка выбранной строки: в светлой теме фиксированный голубой,
 * не пропускаем через TC(). */
static lv_color_t selColor(void) {
  if (gThemeLight)
    return lv_color_make(190, 215, 255);
  Color c = (Color)(((0 & 0xF8) << 8) | ((40 & 0xFC) << 3) | (96 >> 3));
  return lvColor(c);
}

static void clampTop(void) {
  if (ctx.s_selIdx < ctx.s_top)
    ctx.s_top = ctx.s_selIdx;
  if (ctx.s_selIdx >= ctx.s_top + ROWS)
    ctx.s_top = (uint16_t)(ctx.s_selIdx - ROWS + 1);
}

static void rebuildView(void) {
  ctx.s_viewCount = CH_BuildList(CH_GetScanlistMask(), ctx.s_view, CH_MAX);
  if (ctx.s_selIdx >= ctx.s_viewCount)
    ctx.s_selIdx = ctx.s_viewCount ? (uint16_t)(ctx.s_viewCount - 1) : 0;
  clampTop();
}

static bool readSel(CH *ch) {
  if (ctx.s_selIdx >= ctx.s_viewCount)
    return false;
  return CH_Read(ctx.s_view[ctx.s_selIdx], ch);
}

/* Сколько каналов реально участвует в скане (без blacklist). */
static uint16_t countScannable(void) {
  uint16_t n = 0;
  for (uint16_t i = 0; i < ctx.s_viewCount; ++i) {
    CH ch;
    if (CH_Read(ctx.s_view[i], &ch) && !ch.blacklist)
      n++;
  }
  return n;
}

/* Настройка VFO на канал. scan=true — скан: звук закрыт сразу после
 * precise-настройки (она его приоткрывает). */
static void applyCh(const CH *ch, bool scan) {
  VFO *v = &vfos[currentVfo];
  Radio r = static_cast<Radio>(ch->radio & 3);
  if (r == RADIO_BK4819B)
    r = RADIO_BK4819A;
  v->modulation = static_cast<ModulationType>(ch->modulation & 7);
  v->bw = ch->bw & 7;
  v->radio = r;
  v->radioManual = true;
  const BandPlan *b = Band_Find(ch->f);
  if (b)
    v->step = static_cast<Step>(b->step);
  Radio_TuneTo(ch->f, true);
  if (scan)
    Radio_ScanAudio(false);
}

static void clearAudio(void) { Radio_ScanAudio(false); }

static void stopScan(void) {
  ctx.s_scanning = false;
  ctx.s_hold = false;
  ctx.s_phase = PH_IDLE;
  clearAudio();
  ctx.s_dirty = true;
}

static void advanceScan(void) {
  if (!ctx.s_scanning)
    return;
  if (!ctx.s_viewCount) {
    stopScan();
    return;
  }
  for (uint16_t k = 1; k <= ctx.s_viewCount; ++k) {
    uint16_t idx = (uint16_t)((ctx.s_selIdx + k) % ctx.s_viewCount);
    CH ch;
    if (!CH_Read(ctx.s_view[idx], &ch))
      continue;
    if (ch.blacklist)
      continue; /* blacklist в списке есть, но в скане пропускается */
    ctx.s_selIdx = idx;
    clampTop();
    applyCh(&ch, true);
    ctx.s_phase = PH_SETTLE;
    ctx.s_nextAt = millis() + SETTLE_MS;
    ctx.s_dirty = true;
    return;
  }
  stopScan(); /* остались только blacklist-каналы */
}

static void startScan(void) {
  if (!ctx.s_viewCount) {
    stopScan();
    return;
  }
  ctx.s_scanning = true;
  ctx.s_hold = false;
  ctx.s_phase = PH_SETTLE;
  ctx.s_calib = true; /* первый проход — шумовой пол, без остановок */
  ctx.s_calibMin = 0xFFFF;
  ctx.s_calibSeen = 0;
  ctx.s_scanCount = countScannable();
  ctx.s_floorRssi = 0;
  ctx.s_selIdx = 0;
  clampTop();
  CH ch;
  if (readSel(&ch)) {
    applyCh(&ch, true);
    ctx.s_nextAt = millis() + SETTLE_MS;
  }
  if (CH_Read(ctx.s_view[ctx.s_selIdx], &ch) && ch.blacklist)
    advanceScan();
  ctx.s_dirty = true;
}

/* Фактический приёмник после перестройки: applyVfo мог откатить на BK
 * (или на SI), если сохранённый чип не покрывает частоту. */
static bool curIsSi(void) { return vfos[currentVfo].radio == RADIO_SI4732; }

static void beginHold(void) {
  bool si = curIsSi();
  if (si)
    Radio_SiAudio(true);
  else
    Radio_ScanAudio(true);
  ctx.s_hold = true;
  ctx.s_phase = PH_HOLD;
  ctx.s_holdUntil = millis() + LISTEN_TIMEOUT_MS;
  ctx.s_dirty = true;
}

static void endHold(void) {
  clearAudio();
  ctx.s_hold = false;
  advanceScan();
}

/* Неблокирующий шаг скана: settle → подтверждение → HOLD. */
static void scanTick(void) {
  uint32_t now = millis();
  CH ch;
  bool ok = readSel(&ch);

  if (ctx.s_hold) {
    if (!ok) {
      ctx.s_hold = false;
      advanceScan();
      return;
    }
    if ((int32_t)(now - ctx.s_holdUntil) >= 0) {
      endHold();
      return;
    }
    /* У SI шумодава нет, а I2C-опрос при прослушивании щёлкает: просто
     * держим HOLD по таймеру. BK проверяем по RSSI, как сканер. */
    if (!curIsSi()) {
      uint16_t thr = (uint16_t)(ctx.s_floorRssi + SQL_MARGIN);
      uint16_t rssi = BK4819_GetRSSI();
      if (rssi + SQL_HYST < thr)
        endHold();
    }
    return;
  }

  if ((int32_t)(now - ctx.s_nextAt) < 0)
    return;
  if (!ok) {
    advanceScan();
    return;
  }

  if (curIsSi()) {
    uint8_t rssi, snr;
    SI47XX_GetRssiSnr(&rssi, &snr);
    if (ctx.s_calib) {
      if (++ctx.s_calibSeen >= ctx.s_scanCount)
        ctx.s_calib = false;
      advanceScan();
      return;
    }
    if (snr >= RX_SNR_THR)
      beginHold();
    else
      advanceScan();
    return;
  }

  uint16_t rssi = BK4819_GetRSSI();

  if (ctx.s_phase == PH_CONFIRM) {
    if (rssi >= ctx.s_confirmThr) {
      beginHold();
      return;
    }
    if (ctx.s_floorRssi < 0xFFFF - SQL_MARGIN)
      ctx.s_floorRssi++; /* ложное срабатывание — порог выше */
    advanceScan();
    return;
  }

  /* Калибровка: берём минимум RSSI за первый проход (у каналов нет
   * непрерывного шумового участка, среднее завышали бы сигналы). */
  if (ctx.s_calib) {
    if (rssi < ctx.s_calibMin)
      ctx.s_calibMin = rssi;
    if (++ctx.s_calibSeen >= ctx.s_scanCount) {
      ctx.s_floorRssi = ctx.s_calibMin == 0xFFFF ? 0 : ctx.s_calibMin;
      ctx.s_calib = false;
    }
    advanceScan();
    return;
  }

  if (!ctx.s_floorRssi)
    ctx.s_floorRssi = rssi;
  uint16_t thr = (uint16_t)(ctx.s_floorRssi + SQL_MARGIN);
  bool open = rssi >= thr;
  if (ch.f % 1300000u == 0)
    open = false; /* гармоники ПЧ */

  if (open) {
    ctx.s_confirmThr = thr;
    ctx.s_phase = PH_CONFIRM;
    ctx.s_nextAt = now + SQL_DELAY_MS;
    return;
  }
  ctx.s_floorRssi = (uint16_t)(((uint32_t)ctx.s_floorRssi * 15 + rssi + 8) / 16);
  advanceScan();
}

/* ─── Клавиши списка ─── */

static void moveSel(int8_t d) {
  if (!ctx.s_viewCount)
    return;
  if (d < 0)
    ctx.s_selIdx = ctx.s_selIdx ? (uint16_t)(ctx.s_selIdx - 1)
                                : (uint16_t)(ctx.s_viewCount - 1);
  else
    ctx.s_selIdx = (uint16_t)((ctx.s_selIdx + 1) % ctx.s_viewCount);
  clampTop();
  CH ch;
  if (readSel(&ch))
    applyCh(&ch, false);
  ctx.s_dirty = true;
}

static void changeMask(uint8_t bit) {
  uint16_t m = CH_GetScanlistMask();
  if (bit >= CH_SCANLIST_COUNT)
    m = CH_SCANLIST_ALL;
  else {
    uint16_t b = (uint16_t)(1u << bit);
    if (m == 0)
      m = b; /* из ALL первый выбор задаёт список */
    else
      m ^= b;
  }
  CH_SetScanlistMask(m);
  CH_MarkDirty();
  ctx.s_selIdx = 0;
  ctx.s_top = 0;
  rebuildView();
  if (ctx.s_scanning)
    startScan();
  else
    ctx.s_dirty = true;
}

static void setMsg(const char *m) {
  snprintf(ctx.s_msg, sizeof(ctx.s_msg), "%s", m);
  ctx.s_msgUntil = millis() + 1200;
  ctx.s_dirty = true;
}

/* Добавить в каналы частоту VFO, с которой вошли в режим. Совпадение по
 * частоте обновляет существующий канал. */
static void addFromVfo(void) {
  CH ch;
  memset(&ch, 0, sizeof(ch));
  ch.f = ctx.s_saved.freq;
  ch.modulation = (uint8_t)(ctx.s_saved.mod & 7);
  ch.bw = (uint8_t)(ctx.s_saved.bw & 7);
  ch.radio = (uint8_t)(ctx.s_saved.radio & 3);
  ch.scanlists = CH_GetScanlistMask();
  if (!CH_Add(&ch)) {
    setMsg("ADD ERR");
    return;
  }
  rebuildView();
  for (uint16_t i = 0; i < ctx.s_viewCount; ++i) {
    CH sel;
    if (CH_Read(ctx.s_view[i], &sel) && sel.f == ch.f) {
      ctx.s_selIdx = i;
      clampTop();
      if (!ctx.s_scanning)
        applyCh(&sel, false);
      break;
    }
  }
  setMsg("SAVED");
}

static void refresh(void) {
  uint16_t n = ctx.s_viewCount;
  char ms[16];
  maskStr(ms, sizeof(ms), CH_GetScanlistMask());
  snprintf(ctx.s_titleBuf, sizeof(ctx.s_titleBuf), "CH %u/%u SL %s %s",
           (unsigned)(n ? ctx.s_selIdx + 1 : 0), (unsigned)n, ms, stateStr());
  lv_label_set_text_static(ctx.s_title, ctx.s_titleBuf);

  char *p = ctx.s_listBuf;
  char *end = ctx.s_listBuf + sizeof(ctx.s_listBuf);
  p[0] = 0;
  if (!n) {
    snprintf(p, (size_t)(end - p), "#808080 no channels: L5 ADD from VFO#");
  }
  const bool L = gThemeLight;
  for (uint16_t r = 0; r < ROWS && ctx.s_top + r < n; ++r) {
    CH ch;
    if (!CH_Read(ctx.s_view[ctx.s_top + r], &ch))
      continue;
    const char *col = ch.blacklist ? (L ? "606060" : "909090")
                                   : (L ? "000000" : "ffffff");
    int w = snprintf(p, (size_t)(end - p), "#%s %c%4u.%05u %s %s#\n", col,
                     ch.blacklist ? '-' : ' ', (unsigned)(ch.f / MHZ),
                     (unsigned)(ch.f % MHZ), MOD_NAMES[ch.modulation & 7],
                     RADIO_NAMES[ch.radio > 2 ? 2 : ch.radio]);
    if (w < 0 || w >= end - p)
      break;
    p += w;
  }
  lv_label_set_text_static(ctx.s_list, ctx.s_listBuf);

  if (n) {
    lv_obj_clear_flag(ctx.s_sel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(ctx.s_sel, 0, LIST_Y + (ctx.s_selIdx - ctx.s_top) * ROW_H);
  } else {
    lv_obj_add_flag(ctx.s_sel, LV_OBJ_FLAG_HIDDEN);
  }

  CH ch;
  if (ctx.s_msg[0]) {
    snprintf(ctx.s_detailBuf, sizeof(ctx.s_detailBuf), "%s", ctx.s_msg);
  } else if (n && readSel(&ch)) {
    char ms2[16];
    maskStr(ms2, sizeof(ms2), ch.scanlists);
    snprintf(ctx.s_detailBuf, sizeof(ctx.s_detailBuf), "#%u %s %s %s %s",
             (unsigned)(ctx.s_view[ctx.s_selIdx] + 1), ms2,
             MOD_NAMES[ch.modulation & 7],
             RADIO_NAMES[ch.radio > 2 ? 2 : ch.radio], stateStr());
  } else {
    ctx.s_detailBuf[0] = ' ';
    ctx.s_detailBuf[1] = 0;
  }
  lv_label_set_text_static(ctx.s_detail, ctx.s_detailBuf);

  if (ctx.s_hintScan != ctx.s_scanning) {
    ctx.s_hintScan = ctx.s_scanning;
    lv_label_set_text_static(
        ctx.s_hint, ctx.s_scanning
                        ? "1-8/0 SLIST S1 BLACK L5 ADD\nUP/DN PAUSE M>STOP M>VFO E>VFO"
                        : "1-8/0 SLIST S1 BLACK L5 ADD\n0>DEL 9>BL M>SCAN M>VFO E>VFO");
  }

  ctx.s_dirty = false;
}

class ChannelsMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("CHAN");
    ctx.s_keep = false;
    ctx.s_menuLong = false;
    ctx.s_scanning = false;
    ctx.s_hold = false;
    ctx.s_phase = PH_IDLE;
    ctx.s_msg[0] = 0;
    ctx.s_hintScan = !ctx.s_scanning; /* заставит обновить подсказку */
    ctx.s_saved = Vfo_Capture();

    UI_ReleaseLabels();
    UI_ClearScreen(BG());
    Radio_Init();

    ctx.s_scr = lvBox(lv_scr_act(), 0, 0, LCD_WIDTH, LCD_HEIGHT, C_BLACK,
                      LV_OPA_TRANSP);
    ctx.s_sel = lvBox(ctx.s_scr, 0, LIST_Y, LCD_WIDTH, ROW_H, C_BLACK,
                      LV_OPA_COVER);
    if (ctx.s_sel)
      lv_obj_set_style_bg_color(ctx.s_sel, selColor(), LV_PART_MAIN);
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
      lv_label_set_text_static(ctx.s_hint, "");

    if (!ctx.s_scr || !ctx.s_sel || !ctx.s_title || !ctx.s_list ||
        !ctx.s_detail)
      return;

    rebuildView();
    ctx.s_selIdx = 0;
    ctx.s_top = 0;
    ctx.s_dirty = true;
    refresh();
    CH ch;
    if (readSel(&ch))
      applyCh(&ch, false);
  }

  void exit() override {
    clearAudio();
    ctx.s_scanning = false;
    ctx.s_hold = false;
    if (ctx.s_scr) {
      lv_obj_del(ctx.s_scr);
      ctx.s_scr = ctx.s_sel = ctx.s_title = ctx.s_list = ctx.s_detail =
          ctx.s_hint = NULL;
    }
    if (!ctx.s_keep)
      Vfo_Restore(ctx.s_saved, false);
  }

  void update() override {
    if (ctx.s_msg[0] && (int32_t)(ctx.s_msgUntil - millis()) <= 0) {
      ctx.s_msg[0] = 0;
      ctx.s_dirty = true;
    }
    if (ctx.s_scanning)
      scanTick();
    else
      Radio_Update(); /* шумодав/звук выбранного канала */
  }

  void render() override {
    STATUSLINE_render();
    if (!ctx.s_scr || !ctx.s_list)
      return;
    if (ctx.s_dirty)
      refresh();
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (!ctx.s_scr || !ctx.s_list)
      return false;

    if (state == KEY_LONG_PRESSED) {
      switch (k) {
      case KEY_MENU: /* скан вкл/выкл */
        ctx.s_menuLong = true;
        if (ctx.s_scanning)
          stopScan();
        else if (countScannable())
          startScan();
        else
          setMsg("NO CHANNELS");
        return true;
      case KEY_5: /* в каналы частоту VFO до входа в режим */
        addFromVfo();
        return true;
      case KEY_0: /* удалить выбранный (в скане — не удаляем) */
        if (!ctx.s_scanning && ctx.s_viewCount) {
          if (CH_Delete(ctx.s_view[ctx.s_selIdx])) {
            rebuildView();
            CH ch;
            if (readSel(&ch))
              applyCh(&ch, false);
            ctx.s_dirty = true;
          }
        }
        return true;
      case KEY_9: /* чёрный список каналов → loot, вручную */
        CH_LoadBlacklistToLoot();
        return true;
      default:
        break;
      }
      return false;
    }

    if (state == KEY_LONG_PRESSED_CONT && (k == KEY_UP || k == KEY_DOWN)) {
      if (ctx.s_scanning) {
        stopScan();
        CH ch;
        if (readSel(&ch))
          applyCh(&ch, false);
      }
      moveSel(k == KEY_UP ? -1 : +1);
      return true;
    }

    if (state != KEY_RELEASED)
      return false;

    if (ctx.s_menuLong) { /* отпускание после длинного MENU — не короткое */
      ctx.s_menuLong = false;
      return true;
    }

    switch (k) {
    case KEY_UP:
    case KEY_DOWN:
      if (ctx.s_scanning) {
        stopScan();
        CH ch;
        if (readSel(&ch))
          applyCh(&ch, false);
      }
      moveSel(k == KEY_UP ? -1 : +1);
      return true;
    case KEY_1:
    case KEY_2:
    case KEY_3:
    case KEY_4:
    case KEY_5:
    case KEY_6:
    case KEY_7:
    case KEY_8:
      changeMask((uint8_t)(k - KEY_1));
      return true;
    case KEY_0:
      changeMask(CH_SCANLIST_COUNT); /* ALL */
      return true;
    case KEY_SIDE1: {
      if (!ctx.s_viewCount)
        return true;
      bool wasScan = ctx.s_scanning;
      if (wasScan) {
        clearAudio();
        ctx.s_hold = false;
      }
      if (CH_ToggleBlacklist(ctx.s_view[ctx.s_selIdx])) {
        rebuildView();
        ctx.s_dirty = true;
        if (wasScan) {
          /* Канал ушёл в конец списка; на его месте — следующий. */
          CH nch;
          if (readSel(&nch)) {
            if (nch.blacklist)
              advanceScan();
            else {
              applyCh(&nch, true);
              ctx.s_phase = PH_SETTLE;
              ctx.s_nextAt = millis() + SETTLE_MS;
            }
          } else {
            advanceScan();
          }
        } else {
          CH ch;
          if (readSel(&ch))
            applyCh(&ch, false);
        }
      }
      return true;
    }
    case KEY_MENU: { /* выбранный канал в VFO */
      CH ch;
      if (readSel(&ch)) {
        clearAudio();
        ctx.s_scanning = false;
        ctx.s_hold = false;
        applyCh(&ch, false);
        ctx.s_keep = true;
        Mode_Switch(MODE_VFO);
      }
      return true;
    }
    case KEY_EXIT:
      Mode_Switch(MODE_VFO);
      return true;
    default:
      break;
    }
    return false;
  }
};

static ChannelsMode channelsInstance;
Mode &MODE_CHANNELS = channelsInstance;
