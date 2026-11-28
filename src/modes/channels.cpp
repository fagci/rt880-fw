/* Канальный режим: список сохранённых каналов и скан по ним.
 *
 * Вход — VFO → длинный 7. Каналы лежат в W25Q32 (src/helper/channels.cpp),
 * в RAM только индексы видимого списка. Экран списка — та же сетка, что у
 * сканера частот (screen_layout.h): сканлист, крупная частота (F_XL), строка
 * деталей (F_S), список каналов и график уровня в HOLD. Текст — нативные
 * LVGL-лейблы, подложка выбранной строки — lvBox; форма правки — отдельная
 * страница с подсказками внизу.
 *
 * Клавиши (список / пауза):
 *   UP/DOWN   — выбор канала (приёмник настраивается на него);
 *   1..8, 0   — сканлист: биты 1..8, 0 — ALL;
 *   #         — форма правки выбранного канала;
 *   MENU      — выбранный канал в VFO и выход;
 *   длинный MENU — включить скан;
 *   SIDE1     — blacklist вкл/выкл у выбранного;
 *   длинный SIDE2 — закрепить/снять канал за VFO, из которого вошли
 *                (частота VFO не меняется, канал слушает multiwatch);
 *   длинный 5 — добавить частоту VFO (на момент входа) в каналы;
 *   длинный 3 — сделать выбранный канал приоритетным (повтор — снять);
 *   длинный 0 — удалить выбранный;
 *   длинный 9 — чёрный список каналов в loot (вручную);
 *   STAR      — список находок;
 *   EXIT      — в VFO, снимок VFO восстанавливается.
 *
 * Форма (8 полей): имя, частота, модуляция, полоса, субтон, приёмник,
 * сканлисты, blacklist. UP/DOWN — поле, SIDE1/SIDE2 — значение, MENU —
 * ввод частоты или имени, # — сохранить и назад, EXIT — сохранить и назад.
 * Имя — мульти-тап 2..9, 1 — символы, 0 — пробел/0, STAR — backspace. На
 * поле сканлистов 1..8 переключают биты, 0 — ALL. На поле субтона STAR
 * включает/выключает обязательное совпадение тона (в списке — `*`).
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
#include "../helper/settings.h"
#include "../helper/store.h"
#include "../helper/tone.h"
#include "../helper/unit_fmt.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/level_graph.h"
#include "../ui/lv_helpers.h"
#include "../ui/lv_port.h"
#include "../ui/numval.h"
#include "../ui/screen_layout.h"
#include "../ui/statusline.h"
#include "lvgl.h"
#include "mode.h"
#include "printf.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LIST_Y 42
#define ROW_H 16 /* line_height шрифта F_M */
#define ROWS 13
#define DETAIL_Y (LIST_Y + ROWS * ROW_H + 3)
#define HINT_Y (DETAIL_Y + 18)
#define VALUE_X 68 /* 4 + 8 символов F_M (8 px) */

/* Экран списка — та же сетка, что у сканера частот (screen_layout.h):
 * верхняя строка — сканлист, крупная частота (F_XL), строка деталей (F_S),
 * список каналов от UI_LOOT_Y и график уровня внизу в HOLD. */
#define TOP_Y (UI_RANGE_BASE - 13)          /* F_M: baseline 38 */
#define BIGFREQ_Y (UI_SCAN_FREQ_BASE - 32)  /* F_XL: baseline 74 */
#define INFO_Y (UI_SCAN_INFO_BASE - 10)     /* F_S: baseline 94 */
#define LIST_VIEW_Y UI_LOOT_Y
#define LIST_VIEW_ROWS UI_LOOT_ROWS
#define SCAN_LEVEL_MS 250 /* шаг истории уровня в HOLD (как в сканере) */

#define EDIT_BLINK_MS 500
#define NAME_TAP_MS 900

#define SETTLE_MS 40     /* пауза после перестройки (как RX_SETTLE_MS в BAND) */
#define SQL_DELAY_MS 30  /* подтверждение открытия повторным замером */
#define SQL_MARGIN 10    /* ~5 дБ над шумом (RSSI: 2 ед. = 1 дБ) */
#define SQL_HYST 4
#define RX_SNR_THR 6   /* порог станции SI, дБ */
#define SI_HOLD_MS 5000 /* у SI нет шумодава — HOLD всё равно по таймеру */
#define PRIO_PERIOD 8   /* priority: канал вклинивается каждые N шагов скана */
/* Scan mode: TO — HOLD ровно 3 с; SE в каналах деградирует до CO, кроме
 * более строгого повторного подтверждения. */
#define SCAN_TO_MS 3000
#define SCAN_MODE_TO 1
#define SCAN_MODE_SE 2

typedef enum { PH_IDLE, PH_SETTLE, PH_CONFIRM, PH_HOLD } ScanPhase;
typedef enum { VIEW_LIST, VIEW_FORM } ChView;
typedef enum {
  FIELD_NAME,
  FIELD_FREQ,
  FIELD_MOD,
  FIELD_BW,
  FIELD_TONE,
  FIELD_RADIO,
  FIELD_SLOTS,
  FIELD_BLACK,
  FIELD_COUNT
} ChField;
typedef enum { EDIT_NONE, EDIT_FREQ, EDIT_NAME } ChEditKind;

static const char *const FIELD_LABELS[FIELD_COUNT] = {
    "Name", "Freq", "Mod", "BW", "Tone", "Radio", "Slots", "Black"};

/* Мульти-тап: цифра → группа символов (верхний регистр для новых имён). */
static const char *const TAP_CHARS[10] = {
    " 0",  "-_./", "ABC2", "DEF3", "GHI4", "JKL5",
    "MNO6", "PQRS7", "TUV8", "WXYZ9"};

static struct Ctx {
  lv_obj_t *s_scr;
  lv_obj_t *s_sel;    /* подсветка поля в форме */
  lv_obj_t *s_cursor; /* мелкий курсор выбранной строки в списке */
  lv_obj_t *s_title;
  lv_obj_t *s_list;
  lv_obj_t *s_detail;
  lv_obj_t *s_hint;
  lv_obj_t *s_bigFreq; /* крупная частота выбранного канала (F_XL) */
  lv_obj_t *s_infoL;
  lv_obj_t *s_infoR;
  char s_bigBuf[16];
  char s_infoBuf[32];
  char s_infoRBuf[20];
  const char *s_statusTitle;
  uint8_t s_view[CH_MAX]; /* индексы хранения видимого списка */
  uint16_t s_viewCount;
  uint16_t s_selIdx;
  uint16_t s_top;
  /* Кэш текста списка: во время скана меняется только курсор, а не строки.
   * Без него lv_label_set_text_static перерисовывает все 10 строк на каждом
   * шаге (240x160 px SPI) и скан идёт заметно медленнее. */
  uint16_t s_listVer;
  uint16_t s_listTopShown;
  uint16_t s_listVerShown;
  uint16_t s_maskShown;
  bool s_dirty;
  bool s_keep;     /* MENU: оставить выбранный канал в VFO */
  bool s_menuLong; /* длинный MENU уже отработал — отпускание игнорируем */
  bool s_scanning;
  bool s_hold;
  bool s_graphOn; /* зона графика уровня активна (не в форме) */
  uint16_t s_lastRssi;
  uint32_t s_lastLevelMs;
  ScanPhase s_phase;
  uint32_t s_nextAt;
  uint32_t s_holdUntil;
  uint32_t s_closedAt; /* момент закрытия шумодава (0 — сигнал есть) */
  uint32_t s_toneAt;   /* 0 — субтон не проверяем/уже проверен */
  uint16_t s_prioIdx;  /* индекс приоритетного канала в s_view, 0xFFFF — нет */
  uint8_t s_prioSteps; /* счётчик шагов до вклинивания priority */
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

  /* Форма правки канала. */
  ChView s_page;
  ChField s_field;
  ChEditKind s_edit;
  bool s_formChanged;
  CH s_draft;
  uint16_t s_draftStore;
  NumVal_t s_nv;
  char s_nameBuf[CH_NAME_LEN];
  uint8_t s_nameCur;
  bool s_nameBlink;
  uint32_t s_nameBlinkAt;
  uint32_t s_nameTapAt;
  uint8_t s_nameTapKey;
} ctx;

/* Нижняя зона: график уровня в списке, подсказки в форме. */
static void graphShow(void);
static void graphHide(void);

/* ─── Список ─── */

static const char *stateStr(void) {
  if (!ctx.s_scanning)
    return "PAUSE";
  return ctx.s_hold ? "HOLD" : "SCAN";
}

static uint16_t listY(ChField f) { return (uint16_t)(LIST_Y + f * ROW_H); }

/* Экран списка — сетка сканера частот: 10 строк до графика уровня. */
static uint16_t viewRows(void) { return LIST_VIEW_ROWS; }

/* Заголовок статуслайна: в списке — состояние, в форме — CHAN. */
static const char *statusTitle(void) {
  return ctx.s_page == VIEW_FORM ? "CHAN" : stateStr();
}

/* HIDDEN трогаем только при реальной смене: lv_obj_add/clear_flag
 * инвалидируют объект даже без изменения, а скан обновляется часто. */
static void setHidden(lv_obj_t *o, bool hidden) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden)
    return;
  if (hidden)
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
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

/* Имя с курсором: '_' на месте вставки, мигает. */
static void nameDisplay(char *b, uint8_t cap) {
  uint8_t n = (uint8_t)strlen(ctx.s_nameBuf);
  uint8_t cur = ctx.s_nameCur > n ? n : ctx.s_nameCur;
  uint8_t o = 0;
  for (uint8_t i = 0; i < n && o + 2 < cap; ++i) {
    if (i == cur)
      b[o++] = ctx.s_nameBlink ? '_' : ' ';
    b[o++] = ctx.s_nameBuf[i];
  }
  if (cur >= n && o + 1 < cap)
    b[o++] = ctx.s_nameBlink ? '_' : ' ';
  b[o] = 0;
}

static const char *radioName(uint8_t r) {
  return RADIO_NAMES[r > RADIO_SI4732 ? RADIO_SI4732 : r];
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
  uint16_t rows = viewRows();
  if (ctx.s_selIdx < ctx.s_top)
    ctx.s_top = ctx.s_selIdx;
  if (ctx.s_selIdx >= ctx.s_top + rows)
    ctx.s_top = (uint16_t)(ctx.s_selIdx - rows + 1);
}

static void rebuildView(void) {
  ctx.s_viewCount = CH_BuildList(CH_GetScanlistMask(), ctx.s_view, CH_MAX);
  ctx.s_listVer++;
  if (ctx.s_selIdx >= ctx.s_viewCount)
    ctx.s_selIdx = ctx.s_viewCount ? (uint16_t)(ctx.s_viewCount - 1) : 0;
  /* Приоритет хранится частотой (индексы хранения плывут при правках). */
  ctx.s_prioIdx = 0xFFFF;
  int32_t pf = Settings_Get(SET_PRIORITY);
  if (pf > 0) {
    for (uint16_t i = 0; i < ctx.s_viewCount; ++i) {
      CH ch;
      if (CH_Read(ctx.s_view[i], &ch) && (int32_t)ch.f == pf) {
        ctx.s_prioIdx = i;
        break;
      }
    }
  }
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
  v->bw = ch->bw & 15;
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
  ctx.s_closedAt = 0;
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
  /* Priority: раз в PRIO_PERIOD шагов проверяем приоритетный канал; если
   * шумодав откроется, обычный HOLD остановит скан на нём. */
  if (++ctx.s_prioSteps >= PRIO_PERIOD) {
    ctx.s_prioSteps = 0;
    uint16_t pi = ctx.s_prioIdx;
    if (pi < ctx.s_viewCount && pi != ctx.s_selIdx) {
      CH pch;
      if (CH_Read(ctx.s_view[pi], &pch) && !pch.blacklist) {
        ctx.s_selIdx = pi;
        clampTop();
        applyCh(&pch, true);
        ctx.s_phase = PH_SETTLE;
        ctx.s_nextAt = millis() + SETTLE_MS;
        ctx.s_dirty = true;
        return;
      }
    }
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
  ctx.s_closedAt = 0;
  ctx.s_phase = PH_SETTLE;
  ctx.s_calib = true; /* первый проход — шумовой пол, без остановок */
  ctx.s_calibMin = 0xFFFF;
  ctx.s_calibSeen = 0;
  ctx.s_scanCount = countScannable();
  ctx.s_floorRssi = 0;
  ctx.s_prioSteps = 0;
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
  ctx.s_closedAt = 0;
  ctx.s_toneAt = 0;
  int32_t mode = Settings_Get(SET_SCANMODE);
  uint32_t tmo;
  if (mode == SCAN_MODE_TO) {
    tmo = SCAN_TO_MS; /* TO: HOLD ровно 3 с */
  } else {
    tmo = Settings_ScanTimeoutMs(SET_SQOPENEDTIMEOUT);
    /* Listen t/o = None означает «до закрытия шумодава», но у SI его нет. */
    if (tmo == 0xFFFFFFFFu && si)
      tmo = SI_HOLD_MS;
  }
  ctx.s_holdUntil = tmo == 0xFFFFFFFFu ? tmo : millis() + tmo;
  ctx.s_dirty = true;
}

static void endHold(void) {
  clearAudio();
  Tone_Update(false, 0, 0, false);
  ctx.s_hold = false;
  ctx.s_closedAt = 0;
  ctx.s_toneAt = 0;
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
    if (ctx.s_holdUntil != 0xFFFFFFFFu &&
        (int32_t)(now - ctx.s_holdUntil) >= 0) {
      endHold();
      return;
    }
    /* Тон-фильтр канала: CTCSS/DCS читаем неблокирующе (~650 мс) уже
     * после открытия шумодава, поэтому цена — только на остановке, не
     * 650 мс на каждый канал. Чужой субтон — сразу дальше. */
    if (!curIsSi() && ch.toneSql && ch.tone != CH_TONE_NONE) {
      uint8_t chip = vfos[currentVfo].radio == RADIO_BK4819B ? 1 : 0;
      Tone_Update(true, chip, vfos[currentVfo].rxF, true);
      if (!ctx.s_toneAt)
        ctx.s_toneAt = now ? now : 1;
      else if ((int32_t)(now - ctx.s_toneAt) >= 800) {
        uint8_t got = Tone_GetCode();
        if (got != TONE_CODE_NONE && got != ch.tone) {
          endHold();
          return;
        }
        ctx.s_toneAt = 0;
      }
    }
    /* У SI шумодава нет, а I2C-опрос при прослушивании щёлкает: просто
     * держим HOLD по таймеру. BK проверяем по RSSI, как сканер. */
    if (!curIsSi()) {
      uint16_t thr = (uint16_t)(ctx.s_floorRssi + SQL_MARGIN);
      uint16_t rssi = BK4819_GetRSSI();
      ctx.s_lastRssi = rssi;
      if (rssi + SQL_HYST < thr) {
        /* Stay time: сигнал пропал, но короткая пауза в речи — не повод
         * ехать дальше. Звук выключаем, HOLD держим до таймаута. */
        uint32_t stay = Settings_ScanTimeoutMs(SET_SQCLOSEDTIMEOUT);
        if (!ctx.s_closedAt) {
          ctx.s_closedAt = now ? now : 1; /* 0 — «ещё не закрыт» */
          clearAudio();
        }
        if (stay != 0xFFFFFFFFu &&
            (int32_t)(now - ctx.s_closedAt) >= (int32_t)stay) {
          endHold();
          return;
        }
      } else if (ctx.s_closedAt) {
        /* сигнал вернулся — снова слушаем */
        ctx.s_closedAt = 0;
        Radio_ScanAudio(true);
      }
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
  ctx.s_lastRssi = rssi;

  if (ctx.s_phase == PH_CONFIRM) {
    /* SE: строже повторное подтверждение (выше порога на SQL_MARGIN). */
    uint16_t need = ctx.s_confirmThr;
    if (Settings_Get(SET_SCANMODE) == SCAN_MODE_SE)
      need = (uint16_t)(need + SQL_MARGIN);
    if (rssi >= need) {
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

/* Стереть прямой ввод NumVal/пул-лейблы перед возвратом формы/списка:
 * курсор рисуется DMA и сам не исчезает. */
static void clearArea(uint16_t h) {
  lv_refr_now(NULL);
  FillRectNoInv(0, LIST_Y, LCD_WIDTH, h, BG());
  UI_ReleaseLabels();
  if (ctx.s_sel)
    lv_obj_invalidate(ctx.s_sel);
}

/* ─── Форма правки ─── */

static void onFreqDone(uint32_t v10) {
  ctx.s_draft.f = v10;
  ctx.s_formChanged = true;
  ctx.s_dirty = true;
}

static void startFreqEdit(void) {
  NumVal_Init(&ctx.s_nv, VALUE_X, (uint16_t)(listY(FIELD_FREQ) + FontAscent(F_M)),
              F_M, 8, 12, UNIT_MHZ, ctx.s_draft.f, 1530, 134000000, onFreqDone,
              POS_L, nullptr, 0, 0);
  ctx.s_edit = EDIT_FREQ;
  NumVal_StartEdit(&ctx.s_nv);
  ctx.s_dirty = true;
}

static const uint16_t SLOT_STATES[CH_SCANLIST_COUNT + 1] = {
    0, 1, 2, 4, 8, 16, 32, 64, 128};

static void changeField(int8_t dir) {
  CH *c = &ctx.s_draft;
  switch (ctx.s_field) {
  case FIELD_FREQ: {
    uint32_t step = 1250; /* 12.5 кГц, если канала нет в плане */
    const BandPlan *b = Band_Find(c->f);
    if (b && b->step < STEP_COUNT)
      step = StepFrequencyTable[b->step];
    int32_t nf = (int32_t)c->f + (int32_t)dir * (int32_t)step;
    if (nf < 1530)
      nf = 1530;
    if (nf > 134000000)
      nf = 134000000;
    c->f = (uint32_t)nf;
    break;
  }
  case FIELD_MOD:
    c->modulation = (uint8_t)((c->modulation + 8 + dir) % 8);
    break;
  case FIELD_BW:
    c->bw = (uint8_t)((c->bw + 10 + dir) % 10);
    break;
  case FIELD_TONE: {
    /* Кольцо: OFF, 50 CTCSS, 104 DCS. Выбор тона включает его фильтр. */
    const int total = 1 + 50 + 104;
    int cur = 0;
    if (c->tone == CH_TONE_NONE)
      cur = 0;
    else if (c->tone & TONE_CODE_DCS)
      cur = 51 + (c->tone & 0x7F);
    else
      cur = 1 + c->tone;
    cur = (cur + total + dir) % total;
    if (cur == 0)
      c->tone = CH_TONE_NONE;
    else if (cur <= 50)
      c->tone = (uint8_t)(cur - 1);
    else
      c->tone = (uint8_t)(TONE_CODE_DCS | (cur - 51));
    c->toneSql = c->tone != CH_TONE_NONE;
    break;
  }
  case FIELD_RADIO:
    if (dir > 0)
      c->radio = c->radio == RADIO_BK4819A ? RADIO_SI4732 : RADIO_BK4819A;
    else
      c->radio = c->radio == RADIO_SI4732 ? RADIO_BK4819A : RADIO_SI4732;
    break;
  case FIELD_SLOTS: {
    int cur = -1;
    for (uint8_t i = 0; i <= CH_SCANLIST_COUNT; ++i)
      if (SLOT_STATES[i] == c->scanlists) {
        cur = (int)i;
        break;
      }
    if (cur < 0)
      cur = 0;
    cur += dir > 0 ? 1 : -1;
    if (cur < 0)
      cur = CH_SCANLIST_COUNT;
    if (cur > CH_SCANLIST_COUNT)
      cur = 0;
    c->scanlists = SLOT_STATES[cur];
    break;
  }
  case FIELD_BLACK:
    c->blacklist = c->blacklist ? 0 : 1;
    break;
  default:
    return;
  }
  ctx.s_formChanged = true;
  ctx.s_dirty = true;
}

static void toggleSlotBit(uint8_t bit) {
  if (bit >= CH_SCANLIST_COUNT) {
    ctx.s_draft.scanlists = CH_SCANLIST_ALL;
  } else {
    uint16_t b = (uint16_t)(1u << bit);
    uint16_t m = ctx.s_draft.scanlists;
    if (!m)
      m = b; /* из ALL первый выбор задаёт список */
    else
      m ^= b;
    ctx.s_draft.scanlists = m;
  }
  ctx.s_formChanged = true;
  ctx.s_dirty = true;
}

static void startNameEdit(void) {
  strncpy(ctx.s_nameBuf, ctx.s_draft.name, CH_NAME_LEN - 1);
  ctx.s_nameBuf[CH_NAME_LEN - 1] = 0;
  ctx.s_nameCur = (uint8_t)strlen(ctx.s_nameBuf);
  ctx.s_nameBlink = true;
  ctx.s_nameBlinkAt = millis();
  ctx.s_nameTapKey = KEY_NONE;
  ctx.s_edit = EDIT_NAME;
  ctx.s_dirty = true;
}

static void commitName(void) {
  for (int i = (int)strlen(ctx.s_nameBuf) - 1; i >= 0 && ctx.s_nameBuf[i] == ' ';
       --i)
    ctx.s_nameBuf[i] = 0;
  strncpy(ctx.s_draft.name, ctx.s_nameBuf, CH_NAME_LEN - 1);
  ctx.s_draft.name[CH_NAME_LEN - 1] = 0;
  ctx.s_formChanged = true;
  ctx.s_edit = EDIT_NONE;
  ctx.s_dirty = true;
}

/* Имя забирает все клавиши, пока редактор открыт. */
static bool nameKey(KEY_Code_t k, Key_State_t state) {
  if (state == KEY_LONG_PRESSED_CONT)
    return true;
  if (state != KEY_RELEASED)
    return true;

  if (k >= KEY_0 && k <= KEY_9) {
    uint8_t ki = (uint8_t)(k - KEY_0);
    const char *g = TAP_CHARS[ki];
    size_t gl = strlen(g);
    if (ctx.s_nameTapKey == (uint8_t)k &&
        millis() - ctx.s_nameTapAt < NAME_TAP_MS && ctx.s_nameCur > 0) {
      char *q = strchr(g, ctx.s_nameBuf[ctx.s_nameCur - 1]);
      char next = q && q[1] ? q[1] : g[0];
      ctx.s_nameBuf[ctx.s_nameCur - 1] = next;
    } else if (ctx.s_nameCur < CH_NAME_LEN - 1) {
      ctx.s_nameBuf[ctx.s_nameCur++] = gl ? g[0] : ' ';
      ctx.s_nameBuf[ctx.s_nameCur] = 0;
    }
    ctx.s_nameTapKey = (uint8_t)k;
    ctx.s_nameTapAt = millis();
    ctx.s_nameBlink = true;
    ctx.s_nameBlinkAt = millis();
    ctx.s_dirty = true;
    return true;
  }

  switch (k) {
  case KEY_UP:
    if (ctx.s_nameCur > 0)
      ctx.s_nameCur--;
    ctx.s_dirty = true;
    return true;
  case KEY_DOWN:
    if (ctx.s_nameCur < (uint8_t)strlen(ctx.s_nameBuf))
      ctx.s_nameCur++;
    ctx.s_dirty = true;
    return true;
  case KEY_STAR: /* backspace */
    if (ctx.s_nameCur > 0) {
      memmove(&ctx.s_nameBuf[ctx.s_nameCur - 1], &ctx.s_nameBuf[ctx.s_nameCur],
              strlen(&ctx.s_nameBuf[ctx.s_nameCur]) + 1);
      ctx.s_nameCur--;
      ctx.s_dirty = true;
    }
    return true;
  case KEY_MENU:
  case KEY_HASH:
  case KEY_PTT:
    commitName();
    return true;
  case KEY_EXIT:
    ctx.s_edit = EDIT_NONE;
    ctx.s_dirty = true;
    return true;
  default:
    return true;
  }
}

static void openForm(void) {
  CH ch;
  if (!readSel(&ch))
    return;
  if (ctx.s_scanning)
    stopScan();
  ctx.s_draft = ch;
  ctx.s_draftStore = ctx.s_view[ctx.s_selIdx];
  ctx.s_page = VIEW_FORM;
  ctx.s_field = FIELD_NAME;
  ctx.s_edit = EDIT_NONE;
  ctx.s_formChanged = false;
  /* Форма переиспользует s_list и его буфер — после возврата список нужно
   * собрать заново. */
  ctx.s_listVerShown = 0xFFFF;
  ctx.s_maskShown = 0xFFFF;
  graphHide();
  lv_obj_invalidate(lv_scr_act());
  ctx.s_dirty = true;
}

static void closeForm(void) {
  if (ctx.s_edit == EDIT_NAME)
    ctx.s_edit = EDIT_NONE;
  NumVal_Cancel(&ctx.s_nv);
  uint16_t store = ctx.s_draftStore;
  bool changed = ctx.s_formChanged;
  ctx.s_page = VIEW_LIST;
  ctx.s_field = FIELD_NAME;
  ctx.s_edit = EDIT_NONE;
  ctx.s_formChanged = false;
  if (changed) {
    if (CH_Update(store, &ctx.s_draft))
      setMsg("SAVED");
    else
      setMsg("SAVE ERR");
    rebuildView();
    clearArea(ROWS * ROW_H);
    bool found = false;
    for (uint16_t i = 0; i < ctx.s_viewCount; ++i)
      if (ctx.s_view[i] == store) {
        ctx.s_selIdx = i;
        found = true;
        break;
      }
    if (!found && ctx.s_viewCount && ctx.s_selIdx >= ctx.s_viewCount)
      ctx.s_selIdx = (uint16_t)(ctx.s_viewCount - 1);
    clampTop();
    CH ch;
    if (readSel(&ch))
      applyCh(&ch, false);
  }
  ctx.s_listVerShown = 0xFFFF;
  ctx.s_maskShown = 0xFFFF;
  lv_obj_invalidate(lv_scr_act());
  ctx.s_dirty = true;
}

/* Добавить в каналы частоту VFO, с которой вошли в режим. Совпадение по
 * частоте обновляет существующий канал. */
static void addFromVfo(void) {
  CH ch;
  memset(&ch, 0, sizeof(ch));
  ch.f = ctx.s_saved.freq;
  ch.modulation = (uint8_t)(ctx.s_saved.mod & 7);
  ch.bw = (uint8_t)(ctx.s_saved.bw & 15);
  ch.radio = (uint8_t)(ctx.s_saved.radio & 3);
  ch.tone = CH_TONE_NONE;
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

/* Нижняя полоса — график уровня как в сканере частот. Прямой DMA в зоне
 * UI_LEVEL_Y..; LVGL пишет только выше (textBottom), иначе flush затирает
 * столбцы графика. В форме зона отдаётся подсказке, график стирается. */
static void graphShow(void) {
  if (ctx.s_graphOn)
    return;
  ctx.s_graphOn = true;
  lv_port_set_text_bottom(UI_LEVEL_Y - 1);
  /* LVGL-flush ниже UI_LEVEL_Y обрезается — очищаем полосу сами, включая
   * 2 px под рамкой графика. */
  FillRectNoInv(0, UI_LEVEL_Y, LCD_WIDTH, LCD_HEIGHT - UI_LEVEL_Y, BG());
  LevelGraph_Reset(UI_LEVEL_Y, UI_LEVEL_H);
  ctx.s_lastLevelMs = millis() - SCAN_LEVEL_MS;
}

static void graphHide(void) {
  if (!ctx.s_graphOn)
    return;
  ctx.s_graphOn = false;
  lv_port_set_text_bottom(LCD_HEIGHT - 1);
  LevelGraph_Hide();
  FillRectNoInv(0, UI_LEVEL_Y, LCD_WIDTH, LCD_HEIGHT - UI_LEVEL_Y, BG());
}

/* Очередная выборка истории уровня: только в HOLD и только на BK (у SI
 * при прослушивании RSSI не читаем — I2C щёлкает). */
static void levelTick(uint32_t now) {
  if (!ctx.s_graphOn || !ctx.s_hold || curIsSi())
    return;
  if (now - ctx.s_lastLevelMs < SCAN_LEVEL_MS)
    return;
  ctx.s_lastLevelMs = now;
  LevelGraph_Add(UI_LEVEL_Y, ctx.s_lastRssi, UI_LEVEL_H);
}

/* Экран списка в стиле сканера частот: сканлист сверху, крупная частота,
 * строка деталей, список каналов и график уровня. */
static void refreshList(void) {
  graphShow();

  uint16_t n = ctx.s_viewCount;
  CH ch;
  bool have = n && readSel(&ch);

  uint16_t mask = CH_GetScanlistMask();
  if (mask != ctx.s_maskShown) {
    ctx.s_maskShown = mask;
    char ms[16];
    CH_MaskString(ms, sizeof(ms), mask);
    snprintf(ctx.s_titleBuf, sizeof(ctx.s_titleBuf), "SL %s", ms);
    lv_obj_set_style_text_align(ctx.s_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text_static(ctx.s_title, ctx.s_titleBuf);
  }
  setHidden(ctx.s_title, false);

  if (have) {
    fmt_mhz5(ctx.s_bigBuf, sizeof(ctx.s_bigBuf), ch.f);
    lv_label_set_text_static(ctx.s_bigFreq, ctx.s_bigBuf);
    setHidden(ctx.s_bigFreq, false);

    if (ctx.s_msg[0]) {
      snprintf(ctx.s_infoBuf, sizeof(ctx.s_infoBuf), "%s", ctx.s_msg);
    } else if (ch.name[0]) {
      snprintf(ctx.s_infoBuf, sizeof(ctx.s_infoBuf), "%s %s %s", ch.name,
               MOD_NAMES[ch.modulation & 7], radioName(ch.radio));
    } else {
      snprintf(ctx.s_infoBuf, sizeof(ctx.s_infoBuf), "CH %u %s %s",
               (unsigned)(ctx.s_view[ctx.s_selIdx] + 1),
               MOD_NAMES[ch.modulation & 7], radioName(ch.radio));
    }
    lv_obj_set_style_text_color(ctx.s_infoL,
                                lvColor(ch.blacklist ? C_GRAY : C_WHITE),
                                LV_PART_MAIN);
    lv_label_set_text_static(ctx.s_infoL, ctx.s_infoBuf);
    setHidden(ctx.s_infoL, false);

    snprintf(ctx.s_infoRBuf, sizeof(ctx.s_infoRBuf), "CH %u/%u",
             (unsigned)(ctx.s_selIdx + 1), (unsigned)n);
    lv_label_set_text_static(ctx.s_infoR, ctx.s_infoRBuf);
    setHidden(ctx.s_infoR, false);
  } else {
    setHidden(ctx.s_bigFreq, true);
    setHidden(ctx.s_infoL, true);
    setHidden(ctx.s_infoR, true);
  }

  setHidden(ctx.s_detail, true);
  setHidden(ctx.s_hint, true);
  setHidden(ctx.s_sel, true);
  lv_obj_set_pos(ctx.s_list, 0, LIST_VIEW_Y);
  setHidden(ctx.s_list, false);
  clampTop();

  /* Строки пересобираем только при смене окна или содержимого: на каждом
   * шаге скана меняется только курсор, и полная перерисовка списка бьёт по
   * скорости (SPI-flush). */
  if (ctx.s_top != ctx.s_listTopShown ||
      ctx.s_listVer != ctx.s_listVerShown) {
    char *p = ctx.s_listBuf;
    char *end = ctx.s_listBuf + sizeof(ctx.s_listBuf);
    p[0] = 0;
    if (!n) {
      snprintf(p, (size_t)(end - p), "#808080 no channels: L5 ADD from VFO#");
    }
    const bool L = gThemeLight;
    for (uint16_t r = 0; r < LIST_VIEW_ROWS && ctx.s_top + r < n; ++r) {
      CH row;
      if (!CH_Read(ctx.s_view[ctx.s_top + r], &row))
        continue;
      const char *col = row.blacklist ? (L ? "606060" : "909090")
                                      : (L ? "000000" : "ffffff");
      int w =
          snprintf(p, (size_t)(end - p), "#%s %c%-7.7s %4u.%05u %s %s#\n", col,
                   row.blacklist ? '-' : ' ', row.name, (unsigned)(row.f / MHZ),
                   (unsigned)(row.f % MHZ), MOD_NAMES[row.modulation & 7],
                   radioName(row.radio));
      if (w < 0 || w >= end - p)
        break;
      p += w;
    }
    lv_label_set_text_static(ctx.s_list, ctx.s_listBuf);
    ctx.s_listTopShown = ctx.s_top;
    ctx.s_listVerShown = ctx.s_listVer;
  }

  if (n) {
    setHidden(ctx.s_cursor, false);
    lv_obj_set_pos(ctx.s_cursor, 0,
                   LIST_VIEW_Y + (ctx.s_selIdx - ctx.s_top) * ROW_H);
  } else {
    setHidden(ctx.s_cursor, true);
  }

  ctx.s_dirty = false;
}

static void refreshForm(void) {
  graphHide();
  const CH *c = &ctx.s_draft;
  char ms[16];
  snprintf(ctx.s_titleBuf, sizeof(ctx.s_titleBuf), "EDIT %-7.7s #%u",
           c->name[0] ? c->name : "CH", (unsigned)(ctx.s_draftStore + 1));
  lv_obj_set_style_text_align(ctx.s_title, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_label_set_text_static(ctx.s_title, ctx.s_titleBuf);
  setHidden(ctx.s_title, false);
  setHidden(ctx.s_bigFreq, true);
  setHidden(ctx.s_infoL, true);
  setHidden(ctx.s_infoR, true);
  setHidden(ctx.s_cursor, true);
  lv_obj_set_pos(ctx.s_list, 4, LIST_Y);
  setHidden(ctx.s_list, false);
  setHidden(ctx.s_detail, false);
  setHidden(ctx.s_hint, false);

  char *p = ctx.s_listBuf;
  char *end = ctx.s_listBuf + sizeof(ctx.s_listBuf);
  p[0] = 0;
  for (uint8_t f = 0; f < FIELD_COUNT; ++f) {
    char v[64];
    v[0] = 0;
    switch (f) {
    case FIELD_NAME:
      if (ctx.s_edit == EDIT_NAME)
        nameDisplay(v, sizeof(v));
      else
        snprintf(v, sizeof(v), "%s", c->name);
      break;
    case FIELD_FREQ:
      if (ctx.s_edit != EDIT_FREQ)
        fmt_mhz5(v, sizeof(v), c->f);
      break;
    case FIELD_MOD:
      snprintf(v, sizeof(v), "%s", MOD_NAMES[c->modulation & 7]);
      break;
    case FIELD_BW:
      snprintf(v, sizeof(v), "%s", BW_NAMES[c->bw % 10]);
      break;
    case FIELD_TONE:
      Tone_CodeString(c->tone, v, sizeof(v));
      if (!v[0])
        snprintf(v, sizeof(v), "OFF");
      else if (c->toneSql)
        strncat(v, " *", sizeof(v) - strlen(v) - 1);
      break;
    case FIELD_RADIO:
      snprintf(v, sizeof(v), "%s", radioName(c->radio));
      break;
    case FIELD_SLOTS:
      CH_MaskString(v, sizeof(v), c->scanlists);
      break;
    case FIELD_BLACK:
      snprintf(v, sizeof(v), "%s", c->blacklist ? "ON" : "OFF");
      break;
    }
    p = append(p, end, "%-7s %s\n", FIELD_LABELS[f], v);
  }
  lv_label_set_text_static(ctx.s_list, ctx.s_listBuf);

  setHidden(ctx.s_sel, false);
  lv_obj_set_pos(ctx.s_sel, 0, listY(ctx.s_field));

  if (ctx.s_msg[0]) {
    snprintf(ctx.s_detailBuf, sizeof(ctx.s_detailBuf), "%s", ctx.s_msg);
  } else {
    CH_MaskString(ms, sizeof(ms), c->scanlists);
    snprintf(ctx.s_detailBuf, sizeof(ctx.s_detailBuf), "#%u SL %s",
             (unsigned)(ctx.s_draftStore + 1), ms);
  }
  lv_label_set_text_static(ctx.s_detail, ctx.s_detailBuf);

  if (ctx.s_edit == EDIT_NAME)
    lv_label_set_text_static(ctx.s_hint,
                             "2-9 MULTI 1 SYM 0 SP U/D CURSOR\n*<X  M/# OK  E>BACK");
  else if (ctx.s_edit == EDIT_FREQ)
    lv_label_set_text_static(ctx.s_hint,
                             "DIGITS  M/# OK  E<X\n(long E CANCEL)");
  else
    lv_label_set_text_static(ctx.s_hint,
                             "U/D FIELD S1/S2 CHG M>EDIT\n1-8 SLOT 0 ALL  #/E SAVE");

  ctx.s_dirty = false;
}

static void refresh(void) {
  if (ctx.s_page == VIEW_FORM)
    refreshForm();
  else
    refreshList();
}

static void moveField(int8_t d) {
  ctx.s_field = (ChField)((ctx.s_field + FIELD_COUNT + d) % FIELD_COUNT);
  ctx.s_dirty = true;
}

/* Клавиши формы канала. */
static bool formKey(KEY_Code_t k, Key_State_t state) {
  if (state == KEY_LONG_PRESSED_CONT) {
    if (k == KEY_UP || k == KEY_DOWN)
      moveField(k == KEY_DOWN ? +1 : -1);
    else if (k == KEY_SIDE1 || k == KEY_SIDE2)
      changeField(k == KEY_SIDE1 ? +1 : -1);
    return true;
  }
  if (state != KEY_RELEASED)
    return true;

  switch (k) {
  case KEY_UP:
    moveField(-1);
    return true;
  case KEY_DOWN:
    moveField(+1);
    return true;
  case KEY_SIDE1:
    changeField(+1);
    return true;
  case KEY_SIDE2:
    changeField(-1);
    return true;
  case KEY_MENU:
    if (ctx.s_field == FIELD_NAME)
      startNameEdit();
    else if (ctx.s_field == FIELD_FREQ)
      startFreqEdit();
    else
      changeField(+1);
    return true;
  case KEY_HASH:
    closeForm();
    return true;
  case KEY_STAR:
    /* На поле тона — вкл/выкл обязательное совпадение субтона. */
    if (ctx.s_field == FIELD_TONE && ctx.s_draft.tone != CH_TONE_NONE) {
      ctx.s_draft.toneSql = ctx.s_draft.toneSql ? 0 : 1;
      ctx.s_formChanged = true;
      ctx.s_dirty = true;
    }
    return true;
  case KEY_1:
  case KEY_2:
  case KEY_3:
  case KEY_4:
  case KEY_5:
  case KEY_6:
  case KEY_7:
  case KEY_8:
  case KEY_9:
    if (ctx.s_field == FIELD_FREQ) {
      startFreqEdit();
      NumVal_Key(&ctx.s_nv, k, state);
    } else if (ctx.s_field == FIELD_NAME) {
      startNameEdit();
      nameKey(k, state);
    } else if (ctx.s_field == FIELD_SLOTS) {
      toggleSlotBit((uint8_t)(k - KEY_1));
    }
    return true;
  case KEY_0:
    if (ctx.s_field == FIELD_FREQ) {
      startFreqEdit();
      NumVal_Key(&ctx.s_nv, k, state);
    } else if (ctx.s_field == FIELD_NAME) {
      startNameEdit();
      nameKey(k, state);
    } else if (ctx.s_field == FIELD_SLOTS) {
      toggleSlotBit(CH_SCANLIST_COUNT);
    }
    return true;
  case KEY_EXIT:
    closeForm();
    return true;
  default:
    return true;
  }
}

class ChannelsMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("CHAN");
    lv_port_set_direct_areas(true);
    lv_port_set_spectrum_hidden(true);
    ctx.s_keep = false;
    ctx.s_menuLong = false;
    ctx.s_scanning = false;
    ctx.s_hold = false;
    ctx.s_closedAt = 0;
    ctx.s_phase = PH_IDLE;
    ctx.s_msg[0] = 0;
    ctx.s_page = VIEW_LIST;
    ctx.s_field = FIELD_NAME;
    ctx.s_edit = EDIT_NONE;
    ctx.s_formChanged = false;
    ctx.s_graphOn = false;
    ctx.s_lastRssi = 0;
    NumVal_Cancel(&ctx.s_nv);
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
    ctx.s_title = lvLabel(ctx.s_scr, F_M, C_YELLOW, UI_LEFT_X, TOP_Y,
                          LCD_WIDTH - 2 * UI_LEFT_X, LV_TEXT_ALIGN_CENTER);
    ctx.s_list =
        lvLabel(ctx.s_scr, F_M, C_WHITE, 0, LIST_VIEW_Y, LCD_WIDTH,
                LV_TEXT_ALIGN_LEFT);
    if (ctx.s_list)
      lv_label_set_recolor(ctx.s_list, true);
    /* Мелкий курсор выбранной строки — вместо полноширинной подсветки:
     * двигается на 8x16 px, а не на 240x16 px, и скан не упирается в SPI. */
    ctx.s_cursor = lvLabel(ctx.s_scr, F_M, C_YELLOW, 0, LIST_VIEW_Y,
                           FontW(F_M), LV_TEXT_ALIGN_LEFT);
    if (ctx.s_cursor)
      lv_label_set_text_static(ctx.s_cursor, ">");
    ctx.s_detail = lvLabel(ctx.s_scr, F_M, C_CYAN, 4, DETAIL_Y, LCD_WIDTH - 8,
                           LV_TEXT_ALIGN_LEFT);
    ctx.s_hint = lvLabel(ctx.s_scr, F_S, C_GRAY, 4, HINT_Y, LCD_WIDTH - 8,
                         LV_TEXT_ALIGN_LEFT);
    if (ctx.s_hint)
      lv_label_set_text_static(ctx.s_hint, "");
    ctx.s_bigFreq = lvLabel(ctx.s_scr, F_XL, C_WHITE, 0, BIGFREQ_Y, LCD_WIDTH,
                            LV_TEXT_ALIGN_CENTER);
    ctx.s_infoL =
        lvLabel(ctx.s_scr, F_S, C_WHITE, UI_LEFT_X, INFO_Y,
                LCD_WIDTH - 2 * UI_LEFT_X, LV_TEXT_ALIGN_LEFT);
    ctx.s_infoR =
        lvLabel(ctx.s_scr, F_S, C_GRAY, UI_LEFT_X, INFO_Y,
                LCD_WIDTH - 2 * UI_LEFT_X, LV_TEXT_ALIGN_RIGHT);

    if (!ctx.s_scr || !ctx.s_sel || !ctx.s_cursor || !ctx.s_title || !ctx.s_list ||
        !ctx.s_detail || !ctx.s_hint || !ctx.s_bigFreq || !ctx.s_infoL ||
        !ctx.s_infoR)
      return;

    ctx.s_listVerShown = 0xFFFF;
    ctx.s_listTopShown = 0xFFFF;
    ctx.s_maskShown = 0xFFFF;
    ctx.s_statusTitle = statusTitle();
    STATUSLINE_SetTitle(ctx.s_statusTitle);
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
    NumVal_Cancel(&ctx.s_nv);
    ctx.s_page = VIEW_LIST;
    ctx.s_edit = EDIT_NONE;
    ctx.s_scanning = false;
    ctx.s_hold = false;
    ctx.s_closedAt = 0;
    LevelGraph_Hide();
    ctx.s_graphOn = false;
    lv_port_set_direct_areas(false);
    if (ctx.s_scr) {
      lv_obj_del(ctx.s_scr);
      ctx.s_scr = ctx.s_sel = ctx.s_cursor = ctx.s_title = ctx.s_list =
          ctx.s_detail = ctx.s_hint = ctx.s_bigFreq = ctx.s_infoL = ctx.s_infoR =
              NULL;
    }
    if (!ctx.s_keep)
      Vfo_Restore(ctx.s_saved, false);
  }

  void update() override {
    if (ctx.s_msg[0] && (int32_t)(ctx.s_msgUntil - millis()) <= 0) {
      ctx.s_msg[0] = 0;
      ctx.s_dirty = true;
    }
    if (ctx.s_edit == EDIT_NAME &&
        millis() - ctx.s_nameBlinkAt >= EDIT_BLINK_MS) {
      ctx.s_nameBlink = !ctx.s_nameBlink;
      ctx.s_nameBlinkAt = millis();
      ctx.s_dirty = true;
    }
    if (NumVal_IsEditing(&ctx.s_nv))
      NumVal_Update(&ctx.s_nv);
    if (ctx.s_scanning)
      scanTick();
    else
      Radio_Update(); /* шумодав/звук выбранного канала */
  }

  void render() override {
    const char *title = statusTitle();
    if (title != ctx.s_statusTitle) {
      ctx.s_statusTitle = title;
      STATUSLINE_SetTitle(title);
    }
    STATUSLINE_render();
    if (!ctx.s_scr || !ctx.s_list)
      return;
    if (ctx.s_dirty)
      refresh();
    levelTick(millis());
    if (NumVal_IsEditing(&ctx.s_nv))
      NumVal_Render(&ctx.s_nv);
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (!ctx.s_scr || !ctx.s_list)
      return false;

    if (ctx.s_edit == EDIT_NAME)
      return nameKey(k, state);

    if (NumVal_IsEditing(&ctx.s_nv)) {
      NumVal_Key(&ctx.s_nv, k, state);
      if (!NumVal_IsEditing(&ctx.s_nv)) {
        ctx.s_edit = EDIT_NONE;
        clearArea(FIELD_COUNT * ROW_H);
        ctx.s_dirty = true;
      }
      return true;
    }

    if (ctx.s_page == VIEW_FORM)
      return formKey(k, state);

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
      case KEY_3: { /* priority: выбранный канал вклинивается в скан */
        CH ch;
        if (readSel(&ch)) {
          bool on = Settings_Get(SET_PRIORITY) == (int32_t)ch.f;
          Settings_Set(SET_PRIORITY, on ? 0 : (int32_t)ch.f);
          rebuildView();
          setMsg(on ? "PRIO CLR" : "PRIO SET");
          ctx.s_dirty = true;
        }
        return true;
      }
      case KEY_SIDE2: { /* закрепить канал за VFO, из которого вошли */
        CH ch;
        if (!readSel(&ch))
          return true;
        uint8_t target = ctx.s_saved.vfo;
        const VFOChannel *cur = VFO_GetChannel(target);
        bool wasMode = VFO_ChannelMode(target);
        if (cur && cur->f == ch.f) {
          VFO_ClearChannel(target);
          /* Снимок VFO после выхода из канального режима — уже частотный. */
          if (wasMode)
            ctx.s_saved = Vfo_Capture(target);
          setMsg("VFO CH CLR");
        } else {
          VFO_AssignChannel(target, ch.f, ch.modulation, ch.bw, ch.radio,
                            ch.name);
          /* В канальном режиме VFO уже настроен на новый канал. */
          if (VFO_ChannelMode(target))
            ctx.s_saved = Vfo_Capture(target);
          setMsg("VFO CH SET");
        }
        Store_MarkDirty();
        return true;
      }
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
        /* MENU — копия канала в частотный VFO, а не переключение режима. */
        if (VFO_ChannelMode(currentVfo))
          VFO_SetChannelMode(currentVfo, false, false);
        applyCh(&ch, false);
        ctx.s_keep = true;
        Store_MarkDirty();
        Mode_Switch(MODE_VFO);
      }
      return true;
    }
    case KEY_HASH: /* форма правки выбранного канала */
      openForm();
      return true;
    case KEY_STAR: /* список находок */
      LOOTLIST_OpenFrom(MODE_CHANNELS);
      return true;
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
