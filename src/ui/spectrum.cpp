#include "spectrum.h"
#include "driver/board.h"
#include "driver/st7789.h"
#include "helper/measurements.h"
#include <string.h>

static uint16_t rssiHistory[SP_MAX_POINTS] = {0};
static uint16_t peakHistory[SP_MAX_POINTS] = {0};
static uint16_t noiseHistory[SP_MAX_POINTS] = {0};
static bool markers[SP_MAX_POINTS] = {0};
static uint8_t lastX = 255;
static uint8_t filledPoints;

static uint32_t stepsCount;
static uint32_t currentStep;
static uint32_t step;
static uint32_t bw;

static FRange range;

static DBmRange dBmRange = {-110, -50};

static bool ticksRendered = false;
static bool forceRedraw = true;

/* Верх столбца в координатах экрана: 0 — верх спектра, sh — столбца нет.
 * Отдельный массив для пикового удержания и прошлого кадра, чтобы
 * перерисовывать только изменившиеся строки. */
static uint8_t topRow[SP_MAX_POINTS];
static uint8_t peakRow[SP_MAX_POINTS];
static uint8_t drawnTop[SP_MAX_POINTS];
static uint8_t drawnPeak[SP_MAX_POINTS];

static uint8_t wf[WF_YN][WF_XN] = {0};

/* Оценка шумового пола для пикового удержания: минимум по столбцам.
 * Линии пиков ниже пола + запас не рисуем — иначе шум даёт «пунктир». */
static uint16_t peakFloor;
static bool peakFloorInit;
static bool peakDecayInit;

extern bool gThemeLight; /* graphics.cpp */

/* Палитра водопада — без чёрного: в тёмной теме от тёмно-синего через
 * голубой/зелёный/жёлтый к красному и белому; в светлой — «инверсия»:
 * от белого, красный/коричневый сверху. */
static uint16_t GRADIENT_PALETTE[15];
static int8_t palTheme = -1;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void init_palette(void) {
  if (palTheme == (int8_t)gThemeLight)
    return;
  palTheme = gThemeLight;
  for (uint8_t i = 0; i < 15; i++) {
    /* базовая гамма: синий → голубой → зелёный → жёлтый → красный */
    uint8_t t = (i * 255) / 14, r, g, b;
    if (t < 85)       { r = 0;               g = t * 3;           b = 255; }
    else if (t < 170) { r = (t - 85) * 3;    g = 255;             b = 255 - (t - 85) * 3; }
    else              { r = 255;             g = 255 - (t - 170) * 3; b = 0; }
    if (!gThemeLight) {
      if (i == 0)  { GRADIENT_PALETTE[i] = rgb565(0, 0, 56); continue; }
      if (i == 14) { GRADIENT_PALETTE[i] = 0xFFFF; continue; }
      GRADIENT_PALETTE[i] = rgb565(r, g, b);
    } else {
      /* от белого (шум) постепенно в цвет, в конце — тёмно-красный */
      uint8_t w = i < 4 ? 255 - i * 64 : 0; /* доля белого */
      if (i == 14) { GRADIENT_PALETTE[i] = rgb565(110, 0, 0); continue; }
      r = r + ((255 - r) * w) / 255;
      g = g + ((255 - g) * w) / 255;
      b = b + ((255 - b) * w) / 255;
      GRADIENT_PALETTE[i] = rgb565(r, g, b);
    }
  }
}

/* Цвет столбца — та же палитра, что у водопада: высота столбца (уровень)
 * даёт индекс в GRADIENT_PALETTE. В светлой теме младшие индексы почти
 * белые и сливались бы с фоном, поэтому начинаем с 4-го. */
static uint16_t barRamp[SP_MAX_POINTS];
static uint8_t barRampH;
static int8_t barRampTheme = -1;
static uint16_t colColor[SP_MAX_POINTS];

static void initBarRamp(uint8_t sh) {
  if (barRampH == sh && barRampTheme == (int8_t)gThemeLight)
    return;
  barRampH = sh;
  barRampTheme = gThemeLight;
  const uint8_t first = gThemeLight ? 4 : 1;
  const uint8_t span = 14 - first;
  for (uint8_t h = 0; h <= sh; h++) {
    uint8_t idx = first;
    if (sh)
      idx = (uint8_t)(first + (uint16_t)h * span / sh);
    barRamp[h] = GRADIENT_PALETTE[idx];
  }
}

static uint16_t peakLineColor(void) {
  return gThemeLight ? rgb565(150, 0, 0) : C_WHITE;
}

/* Адаптивные уровни водопада (единицы RSSI, 0.5 дБ): низ — медленно
 * следящий пол шума, верх — медленно спадающий максимум, минимум размаха
 * 24 дБ. Так картинка не «уходит в ноль» и не зашкаливает при смене
 * диапазона/усиления. */
static int32_t lvlLo, lvlHi;
static bool lvlInit;
#define LVL_MIN_SPAN 48

static void update_levels(void) {
  int32_t mn = INT32_MAX, mx = 0;
  for (uint16_t x = 0; x < SP_MAX_POINTS; ++x) {
    int32_t r = rssiHistory[x];
    if (!r) continue; /* ещё не измерено */
    if (r < mn) mn = r;
    if (r > mx) mx = r;
  }
  if (mx == 0) return;
  if (!lvlInit) {
    lvlLo = mn;
    lvlHi = mx;
    lvlInit = true;
    return;
  }
  /* медленно в обе стороны: один выброс не должен менять яркость всей
   * строки (горизонтальные полосы в водопаде) */
  lvlLo += (mn - lvlLo) / (mn < lvlLo ? 8 : 32);
  lvlHi += (mx - lvlHi) / (mx > lvlHi ? 8 : 64);
}

static uint8_t getPalIndex(uint16_t rssi) {
  int32_t lo = lvlLo - 4; /* шум чуть выше нуля палитры */
  int32_t span = lvlHi - lo;
  if (span < LVL_MIN_SPAN) span = LVL_MIN_SPAN;
  int32_t i = ((int32_t)rssi - lo) * 14 / span;
  return i < 0 ? 0 : i > 14 ? 14 : (uint8_t)i;
}

static uint16_t v(uint8_t x) { return rssiHistory[x]; }

static uint8_t f2x(uint32_t f) {
  return ConvertDomain(f, range.start, range.end, 0, SP_MAX_POINTS - 1);
}

void SP_ResetHistory(void) {
  for (uint8_t y = 0; y < ARRAY_SIZE(wf); ++y)
    memset(wf[y], 0, ARRAY_SIZE(wf[0]));
  for (uint8_t i = 0; i < SP_MAX_POINTS; ++i) {
    rssiHistory[i] = peakHistory[i] = 0;
    noiseHistory[i] = UINT16_MAX;
    markers[i] = false;
    topRow[i] = drawnTop[i] = 0xFF;
    peakRow[i] = drawnPeak[i] = 0xFF;
  }
  lastX = 255;
  filledPoints = 0;
  currentStep = 0;
  lvlInit = false;
  peakFloorInit = false;
  peakDecayInit = false;
  forceRedraw = true;
}

/* Пометить спектр «неактуальным»: следующий SP_Render перерисует всю
 * область целиком — сотрёт всё, что рисовали поверх (курсор, рамку). */
void SP_Invalidate(uint8_t sh) {
  (void)sh;
  forceRedraw = true;
}

void SP_ResetRender(void) {
  ticksRendered = false;
  forceRedraw = true;
}

void SP_Begin(void) { currentStep = 0; }

void SP_Next(void) {
  if (currentStep < stepsCount - 1)
    currentStep++;
}

void SP_Init(FRange *r, uint32_t stepSize, uint32_t _bw) {
  init_palette();
  filledPoints = 0;
  step = stepSize;
  bw = _bw;
  range = *r;
  stepsCount = (r->end - r->start) / stepSize + 1;
  SP_ResetHistory();
  SP_Begin();
  ticksRendered = false;
}

void SP_AddPoint(Measurement *msm) {
  uint8_t x = f2x(msm->f);
  if (lastX != x) {
    lastX = x;
    rssiHistory[x] = markers[x] = 0;
    noiseHistory[x] = UINT16_MAX;
  }
  if (msm->rssi > rssiHistory[x])
    rssiHistory[x] = msm->rssi;
  if (msm->rssi > peakHistory[x])
    peakHistory[x] = msm->rssi;
  if (msm->noise < noiseHistory[x])
    noiseHistory[x] = msm->noise;
  if (markers[x] == false && msm->open)
    markers[x] = msm->open;
  /* последняя точка диапазона закрашивает хвост до края (иначе правый
   * столбец никогда не заполнялся — чёрная полоска в водопаде) */
  uint8_t XL = (msm->f + step >= range.end) ? SP_MAX_POINTS
                                            : f2x(msm->f + step);
  for (uint8_t nx = x + 1; nx < XL; ++nx) {
    rssiHistory[nx] = rssiHistory[x];
    noiseHistory[nx] = noiseHistory[x];
    markers[nx] = markers[x];
    if (peakHistory[nx] < rssiHistory[x])
      peakHistory[nx] = rssiHistory[x];
  }
  if (x > filledPoints && x < SP_MAX_POINTS)
    filledPoints = x + 1;
}

static void drawTicks(uint8_t y, uint32_t fs, uint32_t fe, uint32_t div,
                      uint8_t h, uint16_t c) {
  const uint8_t th = 4;

  for (uint32_t f = fs - (fs % div) + div; f < fe; f += div) {
    uint8_t tx = f2x(f);
    st7789_fill_rect_dma(tx, y + h - th, 1, th, c);
  }
}

static void SP_DrawTicks(uint8_t y, uint8_t h, FRange *p) {
  uint32_t fs = p->start;
  uint32_t fe = p->end;
  uint32_t bw = fe - fs;

  for (uint32_t p = 100000000; p >= 10; p /= 10) {
    if (p < bw) {
      drawTicks(y, fs, fe, p, h, C_GRAY);
      return;
    }
  }
}

/* Столбцы уже содержат максимум по всем точкам скана (SP_AddPoint), так что
 * кадр строится за 240 шагов независимо от числа точек в диапазоне. */
static void renderWfColumn(uint8_t x, uint16_t val) {
  uint8_t pal = getPalIndex(val);
  uint8_t bx = x >> 1;
  if (bx >= WF_XN)
    return;
  if ((x & 1) == 0) {
    wf[0][bx] &= 0xF0;
    wf[0][bx] |= pal;
  } else {
    wf[0][bx] &= 0x0F;
    wf[0][bx] |= pal << 4;
  }
}

// 2 × 240 × 2 = 960 байт — буферы уже в big-endian для SPI
#define SP_BYTES (SP_MAX_POINTS * 2)
static uint8_t sp_buf[2][SP_BYTES];

static void fill_sp_row(uint8_t *buf, uint8_t row) {
  const uint16_t bg = gThemeLight ? C_WHITE : C_BLACK;
  const uint16_t peak = peakLineColor();
  for (uint8_t x = 0; x < SP_MAX_POINTS; x++) {
    uint16_t c = bg;
    if (peakRow[x] == row)
      c = peak;
    else if (topRow[x] <= row)
      c = colColor[x];
    buf[x * 2] = (uint8_t)(c >> 8);
    buf[x * 2 + 1] = (uint8_t)(c);
  }
}

/* Пиковое удержание спадает очень медленно: 0.5 дБ (1 единица RSSI) за
 * 2 секунды. Пик на 40 дБ живёт больше двух минут; сбросить его можно
 * вручную (SP_ResetPeaks), иначе он висел бы до смены диапазона. */
#define PEAK_DECAY_MS 2000
#define PEAK_DECAY_STEP 1
static uint32_t lastPeakDecay;

static void decayPeaks(void) {
  uint32_t now = millis();
  if (!peakDecayInit) {
    lastPeakDecay = now;
    peakDecayInit = true;
    return;
  }
  uint32_t elapsed = now - lastPeakDecay;
  if (elapsed < PEAK_DECAY_MS)
    return;
  uint32_t steps = elapsed / PEAK_DECAY_MS;
  lastPeakDecay = now;
  if (steps > 8)
    steps = 8;
  steps *= PEAK_DECAY_STEP;
  for (uint8_t x = 0; x < SP_MAX_POINTS; x++) {
    uint16_t cur = rssiHistory[x];
    if (peakHistory[x] > cur + steps)
      peakHistory[x] -= steps;
    else
      peakHistory[x] = cur;
  }
}

/* Сброс пикового удержания: оставляем только текущий кадр. Вызывается
 * вручную (длинная 5) — иначе накопленные пики висят до смены диапазона. */
void SP_ResetPeaks(void) {
  for (uint8_t x = 0; x < SP_MAX_POINTS; x++)
    peakHistory[x] = rssiHistory[x];
  peakDecayInit = false;
  peakFloorInit = false;
  forceRedraw = true;
}

static void updatePeakFloor(void) {
  uint16_t mn = UINT16_MAX;
  for (uint8_t x = 0; x < SP_MAX_POINTS; x++)
    if (rssiHistory[x] && rssiHistory[x] < mn)
      mn = rssiHistory[x];
  if (mn == UINT16_MAX)
    return;
  if (!peakFloorInit) {
    peakFloor = mn;
    peakFloorInit = true;
  } else if (mn < peakFloor) {
    peakFloor = mn;
  } else {
    peakFloor += (mn - peakFloor) / 32;
  }
}

void SP_Render(FRange *p, uint8_t sy, uint8_t sh) {
  init_palette();
  initBarRamp(sh);
  const uint16_t vMin = (dBmRange.min + 160) * 2;
  const uint16_t vMax = (dBmRange.max + 160) * 2;
  dBmRange.min = Rssi2DBm(vMin);
  dBmRange.max = Rssi2DBm(vMax);

  decayPeaks();
  updatePeakFloor();

  for (uint8_t x = 0; x < SP_MAX_POINTS; x++) {
    uint16_t hv = ConvertDomain(v(x) * 2, vMin * 2, vMax * 2, 0, sh);
    uint8_t h = hv > sh ? sh : hv;
    topRow[x] = (uint8_t)(sh - h);
    colColor[x] = barRamp[h];

    uint16_t pv =
        ConvertDomain(peakHistory[x] * 2, vMin * 2, vMax * 2, 0, sh);
    uint8_t ph = pv > sh ? sh : pv;
    /* Держим только заметно ушедший пик (≥4 дБ и ≥3 px над столбцом):
     * иначе шумовые выбросы рисуют «пунктир» по всему низу спектра. */
    uint16_t d = peakHistory[x] > rssiHistory[x]
                     ? peakHistory[x] - rssiHistory[x]
                     : 0;
    bool held = peakFloorInit && peakHistory[x] >= peakFloor + 14;
    peakRow[x] = (held && d >= 8 && ph >= h + 3) ? (uint8_t)(sh - ph) : 0xFF;
  }

  bool full = forceRedraw;
  forceRedraw = false;

  uint8_t first = sh, last = 0;
  bool any = full;
  if (full) {
    first = 0;
    last = sh - 1;
  } else {
    for (uint8_t x = 0; x < SP_MAX_POINTS; x++) {
      if (topRow[x] == drawnTop[x] && peakRow[x] == drawnPeak[x])
        continue;

      uint8_t lo = 0xFF, hi = 0;
      if (topRow[x] < sh) {
        lo = topRow[x];
        hi = sh - 1;
      }
      if (drawnTop[x] < sh) {
        if (drawnTop[x] < lo)
          lo = drawnTop[x];
        if (sh - 1 > hi)
          hi = sh - 1;
      }
      if (peakRow[x] != 0xFF) {
        if (peakRow[x] < lo)
          lo = peakRow[x];
        if (peakRow[x] > hi)
          hi = peakRow[x];
      }
      if (drawnPeak[x] != 0xFF) {
        if (drawnPeak[x] < lo)
          lo = drawnPeak[x];
        if (drawnPeak[x] > hi)
          hi = drawnPeak[x];
      }
      if (lo == 0xFF)
        continue;
      if (lo < first)
        first = lo;
      if (hi > last)
        last = hi;
      any = true;
    }
  }

  if (!any) {
    if (!ticksRendered) {
      SP_DrawTicks(sy, sh, p);
      ticksRendered = true;
    }
    return;
  }
  if (last >= sh)
    last = sh - 1;

  uint8_t rows = last - first + 1;
  st7789_cs_low();
  st7789_set_addr_window_raw(0, sy + first, SP_MAX_POINTS, rows);

  // Первую строку готовим заранее и сразу стартуем DMA
  uint8_t cur = 0;
  fill_sp_row(sp_buf[cur], first);
  st7789_dma_start(sp_buf[cur], SP_BYTES);

  // Со второй строки: CPU заполняет buf[cur^1] пока DMA гонит buf[cur]
  for (uint8_t row = first + 1; row <= last; row++) {
    cur ^= 1;
    fill_sp_row(sp_buf[cur], row);
    st7789_dma_wait();
    st7789_dma_start(sp_buf[cur], SP_BYTES);
  }

  st7789_dma_wait();
  st7789_cs_high();

  memcpy(drawnTop, topRow, sizeof(topRow));
  memcpy(drawnPeak, peakRow, sizeof(peakRow));

  /* Тики лежат в нижних 4 строках: если их не перерисовали — они целы. */
  if (full || last + 4 >= sh) {
    SP_DrawTicks(sy, sh, p);
    ticksRendered = true;
  }
}

static uint16_t wfLine[SP_MAX_POINTS];
static uint16_t wfScroll = 0;
static bool wfScrollInited = false;

static void WF_InitScroll(uint8_t wy, uint8_t wh) {
  st7789_set_vscroll_area(wy, wh, ST7789_HEIGHT - wy - wh);
  st7789_set_vscroll_start(wy);
  wfScroll = 0;
  wfScrollInited = true;
}

void WF_Render(uint8_t wy, bool wfDown) {
  uint16_t wh = ST7789_HEIGHT - wy;

  if (!wfScrollInited)
    WF_InitScroll(wy, wh);

  init_palette();
  update_levels();
  memset(wf[0], 0, WF_XN);
  for (uint8_t x = 0; x < SP_MAX_POINTS; ++x)
    renderWfColumn(x, rssiHistory[x]);

  for (uint8_t bx = 0; bx < WF_XN; ++bx) {
    uint8_t v = wf[0][bx];
    wfLine[bx * 2 + 0] = GRADIENT_PALETTE[v & 0x0F];
    wfLine[bx * 2 + 1] = GRADIENT_PALETTE[(v >> 4) & 0x0F];
  }

  if (wfDown) {
    wfScroll = (wfScroll == 0) ? (wh - 1) : (wfScroll - 1);
  } else {
    wfScroll = (wfScroll + 1) % wh;
  }

  uint16_t drawY = wy + wfScroll;
  st7789_cs_low();
  st7789_set_addr_window_raw(0, drawY, SP_MAX_POINTS, 1);
  st7789_write_pixels_dma(wfLine, SP_MAX_POINTS);
  st7789_cs_high();

  // Новая строка = wfScroll, она должна быть снизу
  // Верхняя видимая = следующая строка после неё
  // wfDown: новая строка сверху (индексы убывают); иначе снизу
  uint16_t vsp = wfDown ? (wy + wfScroll) : (wy + (wfScroll + 1) % wh);
  st7789_set_vscroll_start(vsp);
}

void WF_Reset(void) {
  wfScrollInited = false;
  wfScroll = 0;
  // Выключить scroll до следующего WF_InitScroll
  st7789_scroll_reset();
}

void SP_RenderArrow(FRange *p, uint32_t f, uint8_t sx, uint8_t sy, uint8_t sh) {
  (void)sx;
  (void)sh;
  uint8_t cx = ConvertDomain(f, p->start, p->end, 0, SP_MAX_POINTS - 1);
  if (cx >= SP_MAX_POINTS)
    return;
  st7789_fill_rect_dma(cx, sy, 1, 4, C_GRAY);
  st7789_fill_rect_dma(cx - 2, sy, 5, 2, C_GRAY);
}

DBmRange SP_GetGradientRange(void) { return dBmRange; }

uint16_t SP_GetNoiseFloor(void) { return Std(rssiHistory, filledPoints); }

uint16_t SP_GetNoiseMax(void) { return Max(noiseHistory, filledPoints); }

const uint16_t *SP_GetHistory(void) { return rssiHistory; }
