/* RADIO — экран SI4732: шкала диапазона и станции на ней.
 *
 * Вход: VFO → длинный 0. Работает только с SI4732: вещательные КВ, СВ/ДВ,
 * FM 64–108 МГц и любительские SSB-диапазоны (модуляцию и шаг берём из
 * PLAN в radio.cpp). Крупно — частота, ниже — шкала полосы; найденные
 * станции отмечаются засечками, высота засечки — SNR.
 *
 * Скан идёт по SNR: станция появляется на шкале сразу, как только канал
 * превысил порог, не дожидаясь конца прохода. Длинный 2 сканирует все
 * вещательные КВ-полосы и показывает их горизонтальными шкалами в столбик:
 * находки — вертикальные цветные полоски по SNR, текущая — стрелка.
 *
 * Клавиши:
 *   длинный 1   — скан текущего диапазона;
 *   длинный 2   — скан всех вещательных КВ (список с названиями);
 *   цифры       — ввод частоты (MENU/# — подтвердить, EXIT — отменить);
 *   UP/DOWN     — станции после скана, иначе шаг по полосе;
 *   SIDE1/SIDE2 — следующий/предыдущий вещательный КВ-диапазон;
 *   длинный 3/9 — BFO в SSB: −/+ (шаг 50 Гц);
 *   MENU        — текущую частоту в VFO и выход;
 *   EXIT        — выход в VFO (во время ввода первый EXIT его отменяет).
 */
#include "modes.h"
#include "vfo_session.h"
#include "../bands.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/si473x.h"
#include "../helper/unit_fmt.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/lv_port.h"
#include "../ui/numval.h"
#include "../ui/statusline.h"
#include "mode.h"
#include <stdio.h>

/* Раскладка (240×320). */
#define RX_FREQ_Y 96
#define RX_BAND_Y 46
#define RX_SCAN_Y 118
#define RX_SCALE_X 12
#define RX_SCALE_W (LCD_WIDTH - 2 * RX_SCALE_X)
#define RX_SCALE_TOP 150
#define RX_SCALE_AXIS_Y 256
#define RX_SCALE_BOTTOM 264 /* ниже оси; подписи начинаются с 270 */
#define RX_SCALE_LABEL_Y 280
#define RX_INFO_Y 300
#define RX_HINT_Y 318
/* Обзор всех полос (скан всех): 14 горизонтальных шкал в столбик. */
#define RX_MULTI_TOP 122
#define RX_MULTI_ROW_H 13
#define RX_MULTI_X 36
#define RX_MULTI_W (LCD_WIDTH - 8 - RX_MULTI_X)

#define RX_SETTLE_MS 40  /* SI4732: пауза после перестройки перед RSQ */
#define RX_SNR_THR 6     /* порог станции по SNR, дБ */
#define RX_SNR_MAX 30    /* верх шкалы SNR для полосок/засечек */
#define RX_MAX_FOUND 128 /* станций на все диапазоны */
#define RX_BFO_STEP 50   /* шаг BFO, Гц */
#define RX_BFO_MAX 1000  /* предел BFO, Гц */

/* Что умеет SI4732 (в единицах 10 Гц). */
#define RX_SI_MIN 15300UL       /* 153 кГц */
#define RX_SI_AM_MAX 3000000UL  /* 30 МГц */
#define RX_SI_FM_MIN 6400000UL  /* 64 МГц */
#define RX_SI_FM_MAX 10800000UL /* 108 МГц */

typedef struct {
  uint32_t f;
  uint8_t snr;
} RxStation;

/* Полоса, которую сейчас рисуем. */
typedef struct {
  uint32_t start, end, step;
  const char *name;
} RxBand;

static struct Ctx {
  bool s_scanning;
  bool s_scanAll;
  bool s_multiMode; /* обзор всех полос (скан всех диапазонов) */
  uint8_t s_scanBand; /* BC_BANDS во время скана всех */
  uint32_t s_scanStartF, s_scanEndF, s_scanStep;
  const char *s_scanName;
  uint16_t s_scanChan, s_scanChannels;
  uint16_t s_scanDone, s_scanTotal;
  int16_t s_candIdx; /* станция, которую сейчас «тянем» */
  uint16_t s_candLast;
  uint32_t s_scanReturnF;
  uint32_t s_nextAt;
  RxStation s_found[RX_MAX_FOUND];
  uint16_t s_foundCount;
  uint16_t s_sel;
  uint8_t s_rssi;
  uint8_t s_snr;
  NumVal_t s_nv;
  VfoSnapshot s_saved;
  bool s_captured;
  bool s_noSi;
  SI47XX_MODE s_siMode = SI47XX_AM;
  bool s_siModeSet;
  int16_t s_bfo;
  bool s_drawnMulti;
  uint32_t s_drawnStart = 0xFFFFFFFF;
  uint32_t s_drawnEnd;
  bool s_scaleDirty = true;
  bool s_scaleScan;
  uint32_t s_scaleStart, s_scaleEnd, s_scaleF;
  uint16_t s_scaleCount, s_scaleSel;
  bool s_multiDirty = true;
  bool s_multiScan;
  uint32_t s_multiF;
  uint16_t s_multiCount;
  uint8_t s_multiBand;
} ctx;


/* Крупная частота — общий редактор, как в VFO. */



/* Ключ предыдущего вида: при смене полосы или обзор↔шкала пул LVGL
 * освобождаем, иначе на экране остаются старые подписи. */

/* Кэш шкалы: при простом прослушивании ничего не меняется — не перерисовываем
 * её каждый кадр (мелькание). */

/* Кэш обзора полос: после скана он статичен, во время скана — живой. */

static uint16_t bandChannels(uint8_t b) {
  uint32_t n = (BC_BANDS[b].end - BC_BANDS[b].start) / BC_CH_STEP;
  return n ? (uint16_t)n : 1;
}

static int8_t bandIndex(uint32_t f) {
  for (uint8_t i = 0; i < BC_BAND_COUNT; i++) {
    if (f >= BC_BANDS[i].start && f < BC_BANDS[i].end)
      return (int8_t)i;
  }
  return -1;
}

static bool siCovers(uint32_t f) {
  return (f >= RX_SI_MIN && f <= RX_SI_AM_MAX) ||
         (f >= RX_SI_FM_MIN && f <= RX_SI_FM_MAX);
}

static bool bandHasFound(uint8_t b) {
  for (uint16_t i = 0; i < ctx.s_foundCount; i++) {
    if (ctx.s_found[i].f >= BC_BANDS[b].start && ctx.s_found[i].f < BC_BANDS[b].end)
      return true;
  }
  return false;
}

static Color snrColor(uint8_t snr) {
  return snr >= 20 ? C_RED : snr >= 10 ? C_YELLOW : C_GREEN;
}

static int16_t multiX(uint32_t f, uint8_t b) {
  const BcBand *band = &BC_BANDS[b];
  if (f <= band->start)
    return RX_MULTI_X;
  if (f >= band->end)
    return RX_MULTI_X + RX_MULTI_W - 1;
  uint32_t span = band->end - band->start;
  uint32_t dx = (uint32_t)(f - band->start) * (RX_MULTI_W - 1) / span;
  return (int16_t)(RX_MULTI_X + dx);
}

static bool ssbActive(void) {
  return ctx.s_siMode == SI47XX_LSB || ctx.s_siMode == SI47XX_USB;
}

static SI47XX_MODE modToSiMode(ModulationType m) {
  switch (m) {
  case MOD_LSB:
    return SI47XX_LSB;
  case MOD_USB:
    return SI47XX_USB;
  case MOD_WFM:
  case MOD_FM:
    return SI47XX_FM;
  default:
    return SI47XX_AM;
  }
}

/* Модуляция/шаг по PLAN + захват нужного режима SI4732. Тяжёлое
 * переключение (FM/патч) делаем только при смене семейства. */
static bool applySiMode(uint32_t f) {
  bool planned = Radio_ApplyBandPlan(f);
  ModulationType m = static_cast<ModulationType>(vfos[currentVfo].modulation);
  if (!planned)
    m = MOD_AM;
  SI47XX_MODE sm = modToSiMode(m);

  if (!ctx.s_siModeSet || sm != ctx.s_siMode) {
    if (!SI47XX_Acquire(sm)) {
      ctx.s_noSi = true;
      ctx.s_scanning = false;
      return false;
    }
    ctx.s_siMode = sm;
    ctx.s_siModeSet = true;
    if (sm == SI47XX_AM) {
      SI47XX_SetAutomaticGainControl(0, 0);
      SI47XX_SetBandwidth(SI47XX_BW_4_kHz, true);
    } else if (sm == SI47XX_LSB || sm == SI47XX_USB) {
      SI47XX_SetAutomaticGainControl(0, 0);
      SI47XX_SetBFO(ctx.s_bfo);
    }
  }
  return true;
}

static void tuneTo(uint32_t f) {
  applySiMode(f);
  vfos[currentVfo].rxF = f;
  SI47XX_TuneTo(f);
  if (NumVal_GetValue(&ctx.s_nv) != f)
    NumVal_SetValue(&ctx.s_nv, f);
}

/* Полоса для шкалы/шага: во время скана — его текущая полоса, иначе по
 * PLAN. Слишком широкие общие полосы (HF) заменяем узким окном ±25 кГц. */
static void currentView(RxBand *v) {
  if (ctx.s_scanning) {
    v->start = ctx.s_scanStartF;
    v->end = ctx.s_scanEndF;
    v->step = ctx.s_scanStep;
    v->name = ctx.s_scanName;
    return;
  }

  uint32_t f = vfos[currentVfo].rxF;
  uint32_t s, e;
  if (Band_Range(f, &s, &e) && !((e - s) > 2000000UL && f < RX_SI_FM_MIN)) {
    v->start = s;
    v->end = e;
    v->step = StepFrequencyTable[vfos[currentVfo].step];
    v->name = Band_Name(f);
    return;
  }

  /* вне плана или общая HF-полоса: узкое окно вокруг частоты */
  v->start = f > 2500 ? f - 2500 : 0;
  v->end = f + 2500;
  v->step = 100; /* 1 кГц */
  v->name = "HF";
}

/* Шаг крупных рисок: 4–7 подписей на ширину шкалы. Значения в 10 Гц. */
static uint32_t niceStep(uint32_t span) {
  static const uint32_t S[] = {1000,   2000,   2500,   5000,   10000,
                               15000,  20000,  25000,  50000,  100000,
                               200000, 500000, 1000000, 2000000, 5000000};
  uint32_t target = span / 6;
  for (uint8_t i = 0; i < sizeof(S) / sizeof(S[0]); i++) {
    if (S[i] >= target)
      return S[i];
  }
  return S[sizeof(S) / sizeof(S[0]) - 1];
}

static int16_t scaleX(uint32_t f, const RxBand *b) {
  if (f <= b->start)
    return RX_SCALE_X;
  if (f >= b->end)
    return RX_SCALE_X + RX_SCALE_W - 1;
  uint32_t span = b->end - b->start;
  uint32_t dx = (uint32_t)(f - b->start) * (RX_SCALE_W - 1) / span;
  return (int16_t)(RX_SCALE_X + dx);
}

static int16_t addFound(uint32_t f, uint8_t snr) {
  if (ctx.s_foundCount >= RX_MAX_FOUND)
    return -1;
  ctx.s_found[ctx.s_foundCount].f = f;
  ctx.s_found[ctx.s_foundCount].snr = snr;
  return (int16_t)ctx.s_foundCount++;
}

static void scanSetBand(uint32_t start, uint32_t end, uint32_t step,
                        const char *name) {
  ctx.s_scanStartF = start;
  ctx.s_scanEndF = end;
  ctx.s_scanStep = step;
  ctx.s_scanName = name;
  uint32_t n = (end - start) / step;
  ctx.s_scanChannels = n ? (uint16_t)(n > 0xFFFF ? 0xFFFF : n) : 1;
  ctx.s_scanChan = 0;
  ctx.s_candIdx = -1;
  ctx.s_candLast = 0;
  tuneTo(start);
  ctx.s_nextAt = millis() + RX_SETTLE_MS;
}

static void startScan(bool all) {
  if (ctx.s_noSi)
    return;
  Radio_SiAudio(false);

  /* Полосу одиночного скана берём до включения ctx.s_scanning, иначе
   * currentView вернёт ещё не заполненную полосу скана. */
  RxBand single;
  if (!all)
    currentView(&single);

  ctx.s_scanReturnF = vfos[currentVfo].rxF;
  ctx.s_scanning = true;
  ctx.s_scanAll = all;
  ctx.s_multiMode = all;
  ctx.s_foundCount = 0;
  ctx.s_sel = 0;
  ctx.s_candIdx = -1;
  ctx.s_scanDone = 0;
  ctx.s_scanTotal = 0;
  ctx.s_rssi = 0;
  ctx.s_snr = 0;
  ctx.s_drawnStart = 0xFFFFFFFF; /* новый список — перерисовать с чистого листа */

  if (all) {
    ctx.s_scanBand = 0;
    for (uint8_t i = 0; i < BC_BAND_COUNT; i++)
      ctx.s_scanTotal += bandChannels(i);
    const BcBand *b = &BC_BANDS[0];
    scanSetBand(b->start, b->end, BC_CH_STEP, Band_Name(b->start));
  } else {
    uint32_t n = (single.end - single.start) / single.step;
    ctx.s_scanTotal = n ? (uint16_t)(n > 0xFFFF ? 0xFFFF : n) : 1;
    scanSetBand(single.start, single.end, single.step, single.name);
  }
}

static void finishScan(void) {
  ctx.s_scanning = false;
  ctx.s_candIdx = -1;
  if (ctx.s_foundCount) {
    ctx.s_sel = 0;
    tuneTo(ctx.s_found[0].f);
    ctx.s_snr = ctx.s_found[0].snr;
    Radio_SiAudio(true);
  } else {
    tuneTo(ctx.s_scanReturnF);
  }
}

/* Один канал: RSQ → станция появляется сразу при SNR выше порога.
 * Допускаем провал в один канал, чтобы станция не распадалась на две. */
static void scanTick(void) {
  uint8_t rssi, snr;
  SI47XX_GetRssiSnr(&rssi, &snr);
  ctx.s_rssi = rssi;
  ctx.s_snr = snr;

  uint32_t f = ctx.s_scanStartF + (uint32_t)ctx.s_scanChan * ctx.s_scanStep;
  if (snr >= RX_SNR_THR) {
    if (ctx.s_candIdx >= 0 && (uint16_t)(ctx.s_scanChan - ctx.s_candLast) <= 2) {
      RxStation *st = &ctx.s_found[ctx.s_candIdx];
      if (snr > st->snr) {
        st->snr = snr;
        st->f = f;
      }
    } else {
      ctx.s_candIdx = addFound(f, snr);
    }
    ctx.s_candLast = ctx.s_scanChan;
  } else if (ctx.s_candIdx >= 0 && (uint16_t)(ctx.s_scanChan - ctx.s_candLast) > 1) {
    ctx.s_candIdx = -1;
  }

  ctx.s_scanDone++;
  ctx.s_scanChan++;

  if (ctx.s_scanChan >= ctx.s_scanChannels) {
    ctx.s_candIdx = -1;
    if (ctx.s_scanAll && ctx.s_scanBand + 1u < BC_BAND_COUNT) {
      ctx.s_scanBand++;
      const BcBand *b = &BC_BANDS[ctx.s_scanBand];
      scanSetBand(b->start, b->end, BC_CH_STEP, Band_Name(b->start));
      return;
    }
    finishScan();
    return;
  }

  tuneTo(ctx.s_scanStartF + (uint32_t)ctx.s_scanChan * ctx.s_scanStep);
  ctx.s_nextAt = millis() + RX_SETTLE_MS;
}

static void stepStation(int8_t dir) {
  if (!ctx.s_foundCount)
    return;
  uint16_t n = ctx.s_foundCount;
  ctx.s_sel = (uint16_t)((ctx.s_sel + n + dir) % n);
  tuneTo(ctx.s_found[ctx.s_sel].f);
  ctx.s_snr = ctx.s_found[ctx.s_sel].snr;
  Radio_SiAudio(true);
}

/* Перестройка без скана — как шаг VFO, по кругу внутри полосы. */
static void tuneStep(int8_t dir) {
  RxBand v;
  currentView(&v);
  uint32_t step = v.step ? v.step : 500;
  uint32_t first = v.start;
  uint32_t last = v.end > step ? v.end - step : v.start;
  int64_t f = (int64_t)vfos[currentVfo].rxF + (int64_t)step * dir;
  if (f < (int64_t)first)
    f = last;
  else if (f > (int64_t)last)
    f = first;
  tuneTo((uint32_t)f);
  Radio_SiAudio(true);
}

static void switchBand(int8_t dir) {
  Radio_SiAudio(false);
  ctx.s_multiMode = false;
  ctx.s_foundCount = 0;
  ctx.s_sel = 0;
  int8_t bi = bandIndex(vfos[currentVfo].rxF);
  uint8_t idx = bi >= 0 ? (uint8_t)bi : 6;
  idx = (uint8_t)((idx + BC_BAND_COUNT + dir) % BC_BAND_COUNT);
  tuneTo(BC_BANDS[idx].start);
}

static void bfoStep(int8_t dir) {
  if (!ssbActive())
    return;
  int32_t v = ctx.s_bfo + dir * RX_BFO_STEP;
  if (v > RX_BFO_MAX)
    v = RX_BFO_MAX;
  if (v < -RX_BFO_MAX)
    v = -RX_BFO_MAX;
  ctx.s_bfo = (int16_t)v;
  SI47XX_SetBFO(ctx.s_bfo);
}

/* Стереть курсор редактора: он рисуется прямым DMA снаружи bbox лейбла и
 * сам не исчезает, когда ввод заканчивается. */
static void clearFreqCursor(void) {
  int16_t tw = (int16_t)ctx.s_nv.cursor * ctx.s_nv.charW;
  int16_t cx = ctx.s_nv.pos == POS_R   ? (int16_t)ctx.s_nv.x
               : ctx.s_nv.pos == POS_C ? (int16_t)ctx.s_nv.x + tw / 2
                                   : (int16_t)ctx.s_nv.x + tw;
  FillRect(cx, (int16_t)ctx.s_nv.y - ctx.s_nv.charH + 1, 2, ctx.s_nv.charH, C_BLACK);
}

static void onFreqEntered(uint32_t f) {
  if (!siCovers(f)) { /* 30–64 МГц SI не принимает */
    NumVal_SetValue(&ctx.s_nv, vfos[currentVfo].rxF);
    return;
  }
  ctx.s_multiMode = false;
  ctx.s_foundCount = 0;
  ctx.s_sel = 0;
  tuneTo(f);
  Radio_SiAudio(true);
}

static void drawScale(const RxBand *b) {
  if (!ctx.s_scaleDirty && b->start == ctx.s_scaleStart && b->end == ctx.s_scaleEnd &&
      vfos[currentVfo].rxF == ctx.s_scaleF && ctx.s_foundCount == ctx.s_scaleCount &&
      ctx.s_sel == ctx.s_scaleSel && ctx.s_scanning == ctx.s_scaleScan)
    return;
  ctx.s_scaleDirty = false;
  ctx.s_scaleStart = b->start;
  ctx.s_scaleEnd = b->end;
  ctx.s_scaleF = vfos[currentVfo].rxF;
  ctx.s_scaleCount = ctx.s_foundCount;
  ctx.s_scaleSel = ctx.s_sel;
  ctx.s_scaleScan = ctx.s_scanning;

  FillRect(0, RX_SCALE_TOP, LCD_WIDTH, RX_SCALE_BOTTOM - RX_SCALE_TOP, C_BLACK);
  DrawHLine(RX_SCALE_X, RX_SCALE_AXIS_Y, RX_SCALE_W, C_GRAY);

  uint32_t span = b->end - b->start;
  uint32_t step = niceStep(span);
  uint32_t minor = step / 5;

  if (minor >= 200) {
    uint32_t f0 = ((b->start + minor - 1) / minor) * minor;
    for (uint32_t f = f0; f < b->end; f += minor) {
      if ((f % step) == 0)
        continue;
      DrawVLine(scaleX(f, b), RX_SCALE_AXIS_Y + 1, 2, C_DARK);
    }
  }

  uint32_t first = ((b->start + step - 1) / step) * step;
  for (uint32_t f = first; f < b->end; f += step) {
    int16_t x = scaleX(f, b);
    DrawVLine(x, RX_SCALE_AXIS_Y - 5, 6, C_GRAY);
    if (x > 18 && x < LCD_WIDTH - 18) {
      char lb[12];
      fmt_value(lb, sizeof(lb), f, UMHZ, UNIT_MHZ);
      PrintfT((uint8_t)x, RX_SCALE_LABEL_Y,
              text_style(C_GRAY, C_BLACK, POS_C, F_S), "%s", lb);
    }
  }

  /* Станции: высота и цвет засечки — SNR (6..30 px). */
  for (uint16_t i = 0; i < ctx.s_foundCount; i++) {
    uint32_t f = ctx.s_found[i].f;
    if (f < b->start || f >= b->end)
      continue;
    uint8_t lvl = ctx.s_found[i].snr > RX_SNR_MAX ? RX_SNR_MAX : ctx.s_found[i].snr;
    uint8_t h = (uint8_t)(6 + (uint16_t)lvl * 24 / RX_SNR_MAX);
    DrawVLine(scaleX(f, b), RX_SCALE_AXIS_Y - h, h,
              snrColor(ctx.s_found[i].snr));
  }

  /* Указатель текущей частоты — одна стрелка под шкалой. */
  int16_t cx = scaleX(vfos[currentVfo].rxF, b);
  for (int8_t k = 0; k < 5; k++)
    DrawHLine(cx - k, RX_SCALE_AXIS_Y + 2 + k, 2 * k + 1, C_CYAN);
}

/* Обзор всех вещательных КВ-полос: 14 горизонтальных шкал в столбик,
 * без подписей частот (только метраж). Найденные станции — вертикальные
 * цветные полоски, высота = SNR; текущая частота — стрелка под шкалой. */
static void drawMulti(void) {
  uint8_t curBand = ctx.s_scanning ? ctx.s_scanBand
                               : (uint8_t)bandIndex(vfos[currentVfo].rxF);
  uint32_t f = vfos[currentVfo].rxF;
  if (!ctx.s_scanning && !ctx.s_multiDirty && !ctx.s_multiScan &&
      ctx.s_multiCount == ctx.s_foundCount && ctx.s_multiF == f &&
      ctx.s_multiBand == curBand)
    return;
  ctx.s_multiDirty = false;
  ctx.s_multiScan = ctx.s_scanning;
  ctx.s_multiCount = ctx.s_foundCount;
  ctx.s_multiF = f;
  ctx.s_multiBand = curBand;

  FillRect(0, RX_MULTI_TOP - 2, LCD_WIDTH,
           RX_HINT_Y - (RX_MULTI_TOP - 2), C_BLACK);

  for (uint8_t b = 0; b < BC_BAND_COUNT; b++) {
    uint16_t rowTop = RX_MULTI_TOP + b * RX_MULTI_ROW_H;
    uint16_t baseY = rowTop + RX_MULTI_ROW_H - 3; /* линия, под ней стрелка */
    bool has = bandHasFound(b);
    bool isCur = (b == curBand);
    Color nameCol = isCur ? C_YELLOW : has ? C_CYAN : C_GRAY;

    PrintfT(4, baseY, text_style(nameCol, C_BLACK, POS_L, F_S), "%s",
            BC_BANDS[b].name);
    DrawHLine(RX_MULTI_X, baseY, RX_MULTI_W, C_GRAY);

    const BcBand *band = &BC_BANDS[b];
    for (uint16_t i = 0; i < ctx.s_foundCount; i++) {
      uint32_t sf = ctx.s_found[i].f;
      if (sf < band->start || sf >= band->end)
        continue;
      uint8_t lvl = ctx.s_found[i].snr > RX_SNR_MAX ? RX_SNR_MAX : ctx.s_found[i].snr;
      uint8_t h = (uint8_t)(2 + (uint16_t)lvl * 6 / RX_SNR_MAX); /* 2..8 */
      DrawVLine(multiX(sf, b), baseY - h, h, snrColor(ctx.s_found[i].snr));
    }

    if (isCur) {
      int16_t cx = multiX(f, b);
      for (int8_t k = 0; k < 3; k++)
        DrawHLine(cx - k, baseY + 1 + k, 2 * k + 1, C_CYAN);
    }
  }
}

class RadioMode : public Mode {
public:
  void enter() override;
  void exit() override;
  void update() override;
  void render() override;
  bool key(KEY_Code_t k, Key_State_t state) override;
};

static RadioMode radioModeInstance;
Mode &MODE_RADIO = radioModeInstance;

void RadioMode::enter() {
  STATUSLINE_SetTitle("BAND");
  lv_port_set_direct_areas(false);
  Radio_Init();

  ctx.s_saved = Vfo_Capture();
  ctx.s_captured = false;
  ctx.s_noSi = false;
  ctx.s_scanning = false;
  ctx.s_scanAll = false;
  ctx.s_multiMode = false;
  ctx.s_foundCount = 0;
  ctx.s_sel = 0;
  ctx.s_rssi = 0;
  ctx.s_snr = 0;
  ctx.s_candIdx = -1;
  ctx.s_scanDone = 0;
  ctx.s_scanTotal = 0;
  ctx.s_bfo = 0;
  ctx.s_siModeSet = false;
  ctx.s_drawnMulti = false;
  ctx.s_drawnStart = 0xFFFFFFFF;
  ctx.s_drawnEnd = 0;
  ctx.s_scaleDirty = true;
  ctx.s_multiDirty = true;

  uint32_t startF = siCovers(ctx.s_saved.freq) ? ctx.s_saved.freq : BC_BANDS[6].start;

  vfos[currentVfo].radio = RADIO_SI4732;
  if (!applySiMode(startF)) {
    Radio_ScanAudio(false);
    return;
  }

  NumVal_Init(&ctx.s_nv, LCD_XCENTER, RX_FREQ_Y, F_XL, 0, 0, UNIT_MHZ, startF,
              RX_SI_MIN, RX_SI_FM_MAX, onFreqEntered, POS_C, nullptr, 0, 0);
  Radio_SiAudio(false);
  Radio_ScanAudio(false);
  tuneTo(startF);
}

void RadioMode::exit() {
  Radio_SiAudio(false);
  if (!ctx.s_captured) {
    Vfo_Restore(ctx.s_saved, false);
  } else {
    Radio_ApplyBandPlan(vfos[currentVfo].rxF);
    Radio_TuneTo(vfos[currentVfo].rxF, true);
  }
  Radio_ScanAudio(false);
  lv_port_set_direct_areas(false);
}

void RadioMode::update() {
  if (ctx.s_noSi)
    return;
  NumVal_Update(&ctx.s_nv);
  if (!ctx.s_scanning)
    return;
  if ((int32_t)(millis() - ctx.s_nextAt) < 0)
    return;
  scanTick();
}

void RadioMode::render() {
  STATUSLINE_render();

  if (ctx.s_noSi) {
    PrintfT(LCD_XCENTER, RX_SCAN_Y,
            text_style(C_RED, C_BLACK, POS_C, F_M), "SI4732 NOT FOUND");
    PrintfT(LCD_XCENTER, RX_INFO_Y,
            text_style(C_GRAY, C_BLACK, POS_C, F_S), "EXIT to return");
    return;
  }

  RxBand v;
  currentView(&v);

  bool keyMulti = ctx.s_multiMode;
  uint32_t keyStart = keyMulti ? 0 : v.start;
  uint32_t keyEnd = keyMulti ? 0 : v.end;
  if (keyMulti != ctx.s_drawnMulti || keyStart != ctx.s_drawnStart ||
      keyEnd != ctx.s_drawnEnd) {
    ctx.s_drawnMulti = keyMulti;
    ctx.s_drawnStart = keyStart;
    ctx.s_drawnEnd = keyEnd;
    ctx.s_scaleDirty = true;
    ctx.s_multiDirty = true;
    UI_ReleaseLabels();
    NumVal_Invalidate(&ctx.s_nv);
    FillRect(0, RX_SCAN_Y, LCD_WIDTH, RX_HINT_Y - RX_SCAN_Y, C_BLACK);
  }

  char b1[12], b2[12], rng[28], fbuf[16], line[48];
  fmt_value(b1, sizeof(b1), v.start, UMHZ, UNIT_MHZ);
  fmt_value(b2, sizeof(b2), v.end - v.step, UMHZ, UNIT_MHZ);
  snprintf(rng, sizeof(rng), "%s-%s", b1, b2);

  PrintfT(8, RX_BAND_Y, text_style(C_CYAN, C_BLACK, POS_L, F_M), "%s",
          v.name);
  PrintfT(LCD_WIDTH - 8, RX_BAND_Y,
          text_style(C_GRAY, C_BLACK, POS_R, F_S), "%s", rng);

  NumVal_Render(&ctx.s_nv);

  if (ctx.s_scanning) {
    uint16_t pct =
        ctx.s_scanTotal ? (uint16_t)((uint32_t)ctx.s_scanDone * 100 / ctx.s_scanTotal) : 0;
    snprintf(line, sizeof(line), "SCAN %u%%  %u/%u", (unsigned)pct,
             (unsigned)ctx.s_scanDone, (unsigned)ctx.s_scanTotal);
    PrintfT(LCD_XCENTER, RX_SCAN_Y,
            text_style(C_GREEN, C_BLACK, POS_C, F_M), "%s", line);
  } else if (ctx.s_foundCount) {
    snprintf(line, sizeof(line), "found %u", (unsigned)ctx.s_foundCount);
    PrintfT(LCD_XCENTER, RX_SCAN_Y,
            text_style(C_YELLOW, C_BLACK, POS_C, F_M), "%s", line);
  } else {
    PrintfT(LCD_XCENTER, RX_SCAN_Y,
            text_style(C_GRAY, C_BLACK, POS_C, F_M), "no scan yet");
  }

  if (ctx.s_multiMode) {
    drawMulti();
  } else {
    drawScale(&v);

    bool selInBand =
        ctx.s_foundCount && ctx.s_found[ctx.s_sel].f >= v.start && ctx.s_found[ctx.s_sel].f < v.end;
    if (ctx.s_scanning) {
      snprintf(line, sizeof(line), "RSSI %u  SNR %u", (unsigned)ctx.s_rssi,
               (unsigned)ctx.s_snr);
    } else if (selInBand) {
      fmt_value(fbuf, sizeof(fbuf), ctx.s_found[ctx.s_sel].f, UMHZ, UNIT_MHZ);
      snprintf(line, sizeof(line), "%u/%u  %s MHz  SNR %u",
               (unsigned)(ctx.s_sel + 1), (unsigned)ctx.s_foundCount, fbuf,
               (unsigned)ctx.s_found[ctx.s_sel].snr);
    } else {
      snprintf(line, sizeof(line), "1 scan band   2 scan all");
    }
    PrintfT(LCD_XCENTER, RX_INFO_Y,
            text_style(ctx.s_scanning ? C_GREEN : selInBand ? C_YELLOW : C_GRAY,
                       C_BLACK, POS_C, F_M),
            "%s", line);
  }

  if (ctx.s_scanning) {
    PrintfT(LCD_XCENTER, RX_HINT_Y,
            text_style(C_GRAY, C_BLACK, POS_C, F_S), "scanning...   EXIT abort");
  } else if (ctx.s_multiMode) {
    PrintfT(LCD_XCENTER, RX_HINT_Y,
            text_style(C_GRAY, C_BLACK, POS_C, F_S),
            "UP/DOWN station   EXIT back");
  } else if (ssbActive()) {
    char bb[28];
    int16_t bv = ctx.s_bfo;
    unsigned av = bv < 0 ? (unsigned)(-bv) : (unsigned)bv;
    snprintf(bb, sizeof(bb), "BFO %c%u  3/9  EXIT", bv < 0 ? '-' : '+', av);
    PrintfT(LCD_XCENTER, RX_HINT_Y,
            text_style(C_YELLOW, C_BLACK, POS_C, F_S), "%s", bb);
  } else {
    PrintfT(LCD_XCENTER, RX_HINT_Y,
            text_style(C_GRAY, C_BLACK, POS_C, F_S),
            "digits freq  UP/DOWN  EXIT");
  }
}

bool RadioMode::key(KEY_Code_t k, Key_State_t state) {
  if (ctx.s_noSi) {
    if (state == KEY_RELEASED && (k == KEY_EXIT || k == KEY_MENU)) {
      Mode_Switch(MODE_VFO);
      return true;
    }
    return false;
  }

  /* Ввод частоты: пока редактор активен, клавиши забирает он. Первый EXIT
   * отменяет ввод и возвращает в режим, повторный — выходит в VFO. */
  if (NumVal_IsEditing(&ctx.s_nv)) {
    if (state == KEY_RELEASED && k == KEY_EXIT) {
      NumVal_Cancel(&ctx.s_nv);
      clearFreqCursor();
      return true;
    }
    NumVal_Key(&ctx.s_nv, k, state);
    if (!NumVal_IsEditing(&ctx.s_nv))
      clearFreqCursor(); /* подтверждение/невалидный ввод закрыли редактор */
    return true;
  }

  /* Короткое нажатие цифры открывает ввод частоты, как в VFO. */
  if (state == KEY_RELEASED && k >= KEY_0 && k <= KEY_9 && !ctx.s_scanning) {
    if (NumVal_Key(&ctx.s_nv, k, state))
      return true;
  }

  /* Стрелки: по найденным станциям, а без скана — шаг по полосе. */
  if ((k == KEY_UP || k == KEY_DOWN) &&
      (state == KEY_PRESSED || state == KEY_LONG_PRESSED_CONT)) {
    if (ctx.s_scanning)
      return true;
    if (ctx.s_foundCount)
      stepStation(k == KEY_UP ? +1 : -1);
    else
      tuneStep(k == KEY_UP ? +1 : -1);
    return true;
  }

  if (state == KEY_LONG_PRESSED) {
    switch (k) {
    case KEY_1:
      startScan(false);
      return true;
    case KEY_2:
      startScan(true);
      return true;
    case KEY_3: /* BFO в SSB: ниже */
      bfoStep(-1);
      return true;
    case KEY_9: /* BFO в SSB: выше */
      bfoStep(+1);
      return true;
    default:
      break;
    }
  }

  if (state != KEY_RELEASED)
    return false;

  switch (k) {
  case KEY_EXIT:
    Mode_Switch(MODE_VFO);
    return true;
  case KEY_MENU:
    ctx.s_captured = true;
    Mode_Switch(MODE_VFO);
    return true;
  case KEY_SIDE1:
    if (!ctx.s_scanning)
      switchBand(+1);
    return true;
  case KEY_SIDE2:
    if (!ctx.s_scanning)
      switchBand(-1);
    return true;
  default:
    break;
  }
  return false;
}
