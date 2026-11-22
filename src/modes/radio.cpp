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
#include "radio.h"
#include "bc_bands.h"
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
#include "vfo.h"
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

static bool s_scanning;
static bool s_scanAll;
static bool s_multiMode; /* обзор всех полос (скан всех диапазонов) */
static uint8_t s_scanBand; /* BC_BANDS во время скана всех */
static uint32_t s_scanStartF, s_scanEndF, s_scanStep;
static const char *s_scanName;
static uint16_t s_scanChan, s_scanChannels;
static uint16_t s_scanDone, s_scanTotal;
static int16_t s_candIdx; /* станция, которую сейчас «тянем» */
static uint16_t s_candLast;
static uint32_t s_scanReturnF;
static uint32_t s_nextAt;

static RxStation s_found[RX_MAX_FOUND];
static uint16_t s_foundCount;
static uint16_t s_sel;
static uint8_t s_rssi;
static uint8_t s_snr;

/* Крупная частота — общий редактор, как в VFO. */
static NumVal_t s_nv;

static uint32_t s_savedF;
static uint8_t s_savedMod;
static uint8_t s_savedStep;
static bool s_captured;
static bool s_noSi;

static SI47XX_MODE s_siMode = SI47XX_AM;
static bool s_siModeSet;
static int16_t s_bfo;

/* Ключ предыдущего вида: при смене полосы или обзор↔шкала пул LVGL
 * освобождаем, иначе на экране остаются старые подписи. */
static bool s_drawnMulti;
static uint32_t s_drawnStart = 0xFFFFFFFF;
static uint32_t s_drawnEnd;

/* Кэш шкалы: при простом прослушивании ничего не меняется — не перерисовываем
 * её каждый кадр (мелькание). */
static bool s_scaleDirty = true;
static bool s_scaleScan;
static uint32_t s_scaleStart, s_scaleEnd, s_scaleF;
static uint16_t s_scaleCount, s_scaleSel;

/* Кэш обзора полос: после скана он статичен, во время скана — живой. */
static bool s_multiDirty = true;
static bool s_multiScan;
static uint32_t s_multiF;
static uint16_t s_multiCount;
static uint8_t s_multiBand;

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
  for (uint16_t i = 0; i < s_foundCount; i++) {
    if (s_found[i].f >= BC_BANDS[b].start && s_found[i].f < BC_BANDS[b].end)
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
  return s_siMode == SI47XX_LSB || s_siMode == SI47XX_USB;
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

  if (!s_siModeSet || sm != s_siMode) {
    if (!SI47XX_Acquire(sm)) {
      s_noSi = true;
      s_scanning = false;
      return false;
    }
    s_siMode = sm;
    s_siModeSet = true;
    if (sm == SI47XX_AM) {
      SI47XX_SetAutomaticGainControl(0, 0);
      SI47XX_SetBandwidth(SI47XX_BW_4_kHz, true);
    } else if (sm == SI47XX_LSB || sm == SI47XX_USB) {
      SI47XX_SetAutomaticGainControl(0, 0);
      SI47XX_SetBFO(s_bfo);
    }
  }
  return true;
}

static void tuneTo(uint32_t f) {
  applySiMode(f);
  vfos[currentVfo].rxF = f;
  SI47XX_TuneTo(f);
  if (NumVal_GetValue(&s_nv) != f)
    NumVal_SetValue(&s_nv, f);
}

/* Полоса для шкалы/шага: во время скана — его текущая полоса, иначе по
 * PLAN. Слишком широкие общие полосы (HF) заменяем узким окном ±25 кГц. */
static void currentView(RxBand *v) {
  if (s_scanning) {
    v->start = s_scanStartF;
    v->end = s_scanEndF;
    v->step = s_scanStep;
    v->name = s_scanName;
    return;
  }

  uint32_t f = vfos[currentVfo].rxF;
  uint32_t s, e;
  if (Radio_BandRange(f, &s, &e) && !((e - s) > 2000000UL && f < RX_SI_FM_MIN)) {
    v->start = s;
    v->end = e;
    v->step = StepFrequencyTable[vfos[currentVfo].step];
    v->name = Radio_BandName(f);
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
  if (s_foundCount >= RX_MAX_FOUND)
    return -1;
  s_found[s_foundCount].f = f;
  s_found[s_foundCount].snr = snr;
  return (int16_t)s_foundCount++;
}

static void scanSetBand(uint32_t start, uint32_t end, uint32_t step,
                        const char *name) {
  s_scanStartF = start;
  s_scanEndF = end;
  s_scanStep = step;
  s_scanName = name;
  uint32_t n = (end - start) / step;
  s_scanChannels = n ? (uint16_t)(n > 0xFFFF ? 0xFFFF : n) : 1;
  s_scanChan = 0;
  s_candIdx = -1;
  s_candLast = 0;
  tuneTo(start);
  s_nextAt = millis() + RX_SETTLE_MS;
}

static void startScan(bool all) {
  if (s_noSi)
    return;
  Radio_SiAudio(false);

  /* Полосу одиночного скана берём до включения s_scanning, иначе
   * currentView вернёт ещё не заполненную полосу скана. */
  RxBand single;
  if (!all)
    currentView(&single);

  s_scanReturnF = vfos[currentVfo].rxF;
  s_scanning = true;
  s_scanAll = all;
  s_multiMode = all;
  s_foundCount = 0;
  s_sel = 0;
  s_candIdx = -1;
  s_scanDone = 0;
  s_scanTotal = 0;
  s_rssi = 0;
  s_snr = 0;
  s_drawnStart = 0xFFFFFFFF; /* новый список — перерисовать с чистого листа */

  if (all) {
    s_scanBand = 0;
    for (uint8_t i = 0; i < BC_BAND_COUNT; i++)
      s_scanTotal += bandChannels(i);
    const BcBand *b = &BC_BANDS[0];
    scanSetBand(b->start, b->end, BC_CH_STEP, Radio_BandName(b->start));
  } else {
    uint32_t n = (single.end - single.start) / single.step;
    s_scanTotal = n ? (uint16_t)(n > 0xFFFF ? 0xFFFF : n) : 1;
    scanSetBand(single.start, single.end, single.step, single.name);
  }
}

static void finishScan(void) {
  s_scanning = false;
  s_candIdx = -1;
  if (s_foundCount) {
    s_sel = 0;
    tuneTo(s_found[0].f);
    s_snr = s_found[0].snr;
    Radio_SiAudio(true);
  } else {
    tuneTo(s_scanReturnF);
  }
}

/* Один канал: RSQ → станция появляется сразу при SNR выше порога.
 * Допускаем провал в один канал, чтобы станция не распадалась на две. */
static void scanTick(void) {
  uint8_t rssi, snr;
  SI47XX_GetRssiSnr(&rssi, &snr);
  s_rssi = rssi;
  s_snr = snr;

  uint32_t f = s_scanStartF + (uint32_t)s_scanChan * s_scanStep;
  if (snr >= RX_SNR_THR) {
    if (s_candIdx >= 0 && (uint16_t)(s_scanChan - s_candLast) <= 2) {
      RxStation *st = &s_found[s_candIdx];
      if (snr > st->snr) {
        st->snr = snr;
        st->f = f;
      }
    } else {
      s_candIdx = addFound(f, snr);
    }
    s_candLast = s_scanChan;
  } else if (s_candIdx >= 0 && (uint16_t)(s_scanChan - s_candLast) > 1) {
    s_candIdx = -1;
  }

  s_scanDone++;
  s_scanChan++;

  if (s_scanChan >= s_scanChannels) {
    s_candIdx = -1;
    if (s_scanAll && s_scanBand + 1u < BC_BAND_COUNT) {
      s_scanBand++;
      const BcBand *b = &BC_BANDS[s_scanBand];
      scanSetBand(b->start, b->end, BC_CH_STEP, Radio_BandName(b->start));
      return;
    }
    finishScan();
    return;
  }

  tuneTo(s_scanStartF + (uint32_t)s_scanChan * s_scanStep);
  s_nextAt = millis() + RX_SETTLE_MS;
}

static void stepStation(int8_t dir) {
  if (!s_foundCount)
    return;
  uint16_t n = s_foundCount;
  s_sel = (uint16_t)((s_sel + n + dir) % n);
  tuneTo(s_found[s_sel].f);
  s_snr = s_found[s_sel].snr;
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
  s_multiMode = false;
  s_foundCount = 0;
  s_sel = 0;
  int8_t bi = bandIndex(vfos[currentVfo].rxF);
  uint8_t idx = bi >= 0 ? (uint8_t)bi : 6;
  idx = (uint8_t)((idx + BC_BAND_COUNT + dir) % BC_BAND_COUNT);
  tuneTo(BC_BANDS[idx].start);
}

static void bfoStep(int8_t dir) {
  if (!ssbActive())
    return;
  int32_t v = s_bfo + dir * RX_BFO_STEP;
  if (v > RX_BFO_MAX)
    v = RX_BFO_MAX;
  if (v < -RX_BFO_MAX)
    v = -RX_BFO_MAX;
  s_bfo = (int16_t)v;
  SI47XX_SetBFO(s_bfo);
}

/* Стереть курсор редактора: он рисуется прямым DMA снаружи bbox лейбла и
 * сам не исчезает, когда ввод заканчивается. */
static void clearFreqCursor(void) {
  int16_t tw = (int16_t)s_nv.cursor * s_nv.charW;
  int16_t cx = s_nv.pos == POS_R   ? (int16_t)s_nv.x
               : s_nv.pos == POS_C ? (int16_t)s_nv.x + tw / 2
                                   : (int16_t)s_nv.x + tw;
  FillRect(cx, (int16_t)s_nv.y - s_nv.charH + 1, 2, s_nv.charH, C_BLACK);
}

static void onFreqEntered(uint32_t f) {
  if (!siCovers(f)) { /* 30–64 МГц SI не принимает */
    NumVal_SetValue(&s_nv, vfos[currentVfo].rxF);
    return;
  }
  s_multiMode = false;
  s_foundCount = 0;
  s_sel = 0;
  tuneTo(f);
  Radio_SiAudio(true);
}

static void drawScale(const RxBand *b) {
  if (!s_scaleDirty && b->start == s_scaleStart && b->end == s_scaleEnd &&
      vfos[currentVfo].rxF == s_scaleF && s_foundCount == s_scaleCount &&
      s_sel == s_scaleSel && s_scanning == s_scaleScan)
    return;
  s_scaleDirty = false;
  s_scaleStart = b->start;
  s_scaleEnd = b->end;
  s_scaleF = vfos[currentVfo].rxF;
  s_scaleCount = s_foundCount;
  s_scaleSel = s_sel;
  s_scaleScan = s_scanning;

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
  for (uint16_t i = 0; i < s_foundCount; i++) {
    uint32_t f = s_found[i].f;
    if (f < b->start || f >= b->end)
      continue;
    uint8_t lvl = s_found[i].snr > RX_SNR_MAX ? RX_SNR_MAX : s_found[i].snr;
    uint8_t h = (uint8_t)(6 + (uint16_t)lvl * 24 / RX_SNR_MAX);
    DrawVLine(scaleX(f, b), RX_SCALE_AXIS_Y - h, h,
              snrColor(s_found[i].snr));
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
  uint8_t curBand = s_scanning ? s_scanBand
                               : (uint8_t)bandIndex(vfos[currentVfo].rxF);
  uint32_t f = vfos[currentVfo].rxF;
  if (!s_scanning && !s_multiDirty && !s_multiScan &&
      s_multiCount == s_foundCount && s_multiF == f &&
      s_multiBand == curBand)
    return;
  s_multiDirty = false;
  s_multiScan = s_scanning;
  s_multiCount = s_foundCount;
  s_multiF = f;
  s_multiBand = curBand;

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
    for (uint16_t i = 0; i < s_foundCount; i++) {
      uint32_t sf = s_found[i].f;
      if (sf < band->start || sf >= band->end)
        continue;
      uint8_t lvl = s_found[i].snr > RX_SNR_MAX ? RX_SNR_MAX : s_found[i].snr;
      uint8_t h = (uint8_t)(2 + (uint16_t)lvl * 6 / RX_SNR_MAX); /* 2..8 */
      DrawVLine(multiX(sf, b), baseY - h, h, snrColor(s_found[i].snr));
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

  s_savedF = vfos[currentVfo].rxF;
  s_savedMod = vfos[currentVfo].modulation;
  s_savedStep = vfos[currentVfo].step;
  s_captured = false;
  s_noSi = false;
  s_scanning = false;
  s_scanAll = false;
  s_multiMode = false;
  s_foundCount = 0;
  s_sel = 0;
  s_rssi = 0;
  s_snr = 0;
  s_candIdx = -1;
  s_scanDone = 0;
  s_scanTotal = 0;
  s_bfo = 0;
  s_siModeSet = false;
  s_drawnMulti = false;
  s_drawnStart = 0xFFFFFFFF;
  s_drawnEnd = 0;
  s_scaleDirty = true;
  s_multiDirty = true;

  uint32_t startF = siCovers(s_savedF) ? s_savedF : BC_BANDS[6].start;

  vfos[currentVfo].radio = RADIO_SI4732;
  if (!applySiMode(startF)) {
    Radio_ScanAudio(false);
    return;
  }

  NumVal_Init(&s_nv, LCD_XCENTER, RX_FREQ_Y, F_XL, 0, 0, UNIT_MHZ, startF,
              RX_SI_MIN, RX_SI_FM_MAX, onFreqEntered, POS_C, nullptr, 0, 0);
  Radio_SiAudio(false);
  Radio_ScanAudio(false);
  tuneTo(startF);
}

void RadioMode::exit() {
  Radio_SiAudio(false);
  if (!s_captured) {
    vfos[currentVfo].rxF = s_savedF;
    vfos[currentVfo].modulation = static_cast<ModulationType>(s_savedMod);
    vfos[currentVfo].step = static_cast<Step>(s_savedStep);
    Radio_TuneTo(s_savedF, true);
  } else {
    Radio_ApplyBandPlan(vfos[currentVfo].rxF);
    Radio_TuneTo(vfos[currentVfo].rxF, true);
  }
  Radio_ScanAudio(false);
  lv_port_set_direct_areas(false);
}

void RadioMode::update() {
  if (s_noSi)
    return;
  NumVal_Update(&s_nv);
  if (!s_scanning)
    return;
  if ((int32_t)(millis() - s_nextAt) < 0)
    return;
  scanTick();
}

void RadioMode::render() {
  STATUSLINE_render();

  if (s_noSi) {
    PrintfT(LCD_XCENTER, RX_SCAN_Y,
            text_style(C_RED, C_BLACK, POS_C, F_M), "SI4732 NOT FOUND");
    PrintfT(LCD_XCENTER, RX_INFO_Y,
            text_style(C_GRAY, C_BLACK, POS_C, F_S), "EXIT to return");
    return;
  }

  RxBand v;
  currentView(&v);

  bool keyMulti = s_multiMode;
  uint32_t keyStart = keyMulti ? 0 : v.start;
  uint32_t keyEnd = keyMulti ? 0 : v.end;
  if (keyMulti != s_drawnMulti || keyStart != s_drawnStart ||
      keyEnd != s_drawnEnd) {
    s_drawnMulti = keyMulti;
    s_drawnStart = keyStart;
    s_drawnEnd = keyEnd;
    s_scaleDirty = true;
    s_multiDirty = true;
    UI_ReleaseLabels();
    NumVal_Invalidate(&s_nv);
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

  NumVal_Render(&s_nv);

  if (s_scanning) {
    uint16_t pct =
        s_scanTotal ? (uint16_t)((uint32_t)s_scanDone * 100 / s_scanTotal) : 0;
    snprintf(line, sizeof(line), "SCAN %u%%  %u/%u", (unsigned)pct,
             (unsigned)s_scanDone, (unsigned)s_scanTotal);
    PrintfT(LCD_XCENTER, RX_SCAN_Y,
            text_style(C_GREEN, C_BLACK, POS_C, F_M), "%s", line);
  } else if (s_foundCount) {
    snprintf(line, sizeof(line), "found %u", (unsigned)s_foundCount);
    PrintfT(LCD_XCENTER, RX_SCAN_Y,
            text_style(C_YELLOW, C_BLACK, POS_C, F_M), "%s", line);
  } else {
    PrintfT(LCD_XCENTER, RX_SCAN_Y,
            text_style(C_GRAY, C_BLACK, POS_C, F_M), "no scan yet");
  }

  if (s_multiMode) {
    drawMulti();
  } else {
    drawScale(&v);

    bool selInBand =
        s_foundCount && s_found[s_sel].f >= v.start && s_found[s_sel].f < v.end;
    if (s_scanning) {
      snprintf(line, sizeof(line), "RSSI %u  SNR %u", (unsigned)s_rssi,
               (unsigned)s_snr);
    } else if (selInBand) {
      fmt_value(fbuf, sizeof(fbuf), s_found[s_sel].f, UMHZ, UNIT_MHZ);
      snprintf(line, sizeof(line), "%u/%u  %s MHz  SNR %u",
               (unsigned)(s_sel + 1), (unsigned)s_foundCount, fbuf,
               (unsigned)s_found[s_sel].snr);
    } else {
      snprintf(line, sizeof(line), "1 scan band   2 scan all");
    }
    PrintfT(LCD_XCENTER, RX_INFO_Y,
            text_style(s_scanning ? C_GREEN : selInBand ? C_YELLOW : C_GRAY,
                       C_BLACK, POS_C, F_M),
            "%s", line);
  }

  if (s_scanning) {
    PrintfT(LCD_XCENTER, RX_HINT_Y,
            text_style(C_GRAY, C_BLACK, POS_C, F_S), "scanning...   EXIT abort");
  } else if (s_multiMode) {
    PrintfT(LCD_XCENTER, RX_HINT_Y,
            text_style(C_GRAY, C_BLACK, POS_C, F_S),
            "UP/DOWN station   EXIT back");
  } else if (ssbActive()) {
    char bb[28];
    int16_t bv = s_bfo;
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
  if (s_noSi) {
    if (state == KEY_RELEASED && (k == KEY_EXIT || k == KEY_MENU)) {
      Mode_Switch(MODE_VFO);
      return true;
    }
    return false;
  }

  /* Ввод частоты: пока редактор активен, клавиши забирает он. Первый EXIT
   * отменяет ввод и возвращает в режим, повторный — выходит в VFO. */
  if (NumVal_IsEditing(&s_nv)) {
    if (state == KEY_RELEASED && k == KEY_EXIT) {
      NumVal_Cancel(&s_nv);
      clearFreqCursor();
      return true;
    }
    NumVal_Key(&s_nv, k, state);
    if (!NumVal_IsEditing(&s_nv))
      clearFreqCursor(); /* подтверждение/невалидный ввод закрыли редактор */
    return true;
  }

  /* Короткое нажатие цифры открывает ввод частоты, как в VFO. */
  if (state == KEY_RELEASED && k >= KEY_0 && k <= KEY_9 && !s_scanning) {
    if (NumVal_Key(&s_nv, k, state))
      return true;
  }

  /* Стрелки: по найденным станциям, а без скана — шаг по полосе. */
  if ((k == KEY_UP || k == KEY_DOWN) &&
      (state == KEY_PRESSED || state == KEY_LONG_PRESSED_CONT)) {
    if (s_scanning)
      return true;
    if (s_foundCount)
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
    s_captured = true;
    Mode_Switch(MODE_VFO);
    return true;
  case KEY_SIDE1:
    if (!s_scanning)
      switchBand(+1);
    return true;
  case KEY_SIDE2:
    if (!s_scanning)
      switchBand(-1);
    return true;
  default:
    break;
  }
  return false;
}
