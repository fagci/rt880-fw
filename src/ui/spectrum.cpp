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

/* Автоуровень, как в k1-fw: низ графика привязан к шумовому полу, а не к
 * абсолютным dBm. Поэтому на любом диапазоне/усилении пол стоит у нижней
 * кромки, а сигналы занимают всю высоту. Единицы — сырой RSSI (0.5 дБ). */
#define SP_MIN_SPAN 40      /* 20 дБ — минимальный размах по вертикали */
#define SP_FLOOR_MARGIN_PX 2 /* пол на 2 px выше низа */
static int levelMin = 100;
static int levelMax = 220;
static bool levelValid;
static uint8_t levelHeight = 52;

static bool ticksRendered = false;
static bool forceRedraw = true;

/* Верх столбца в координатах экрана: 0 — верх спектра, sh — столбца нет.
 * Отдельный массив для пикового удержания и прошлого кадра, чтобы
 * перерисовывать только изменившиеся строки. */
static uint8_t topRow[SP_MAX_POINTS];
static uint8_t peakRow[SP_MAX_POINTS];
static uint8_t drawnTop[SP_MAX_POINTS];
static uint8_t drawnPeak[SP_MAX_POINTS];

/* Оценка шумового пола для пикового удержания: минимум по столбцам.
 * Линии пиков ниже пола + запас не рисуем — иначе шум даёт «пунктир». */
static uint16_t peakFloor;
static bool peakFloorInit;
static bool peakDecayInit;

extern bool gThemeLight; /* graphics.cpp */

/* Плавный градиент водопада: уровень 0..255 интерполируется между
 * опорными цветами прямо при отрисовке. Прежняя палитра из 15 ступеней
 * давала заметные полосы. Таблица в RAM не нужна — 240 вызовов на строку
 * для Cortex-M4 дёшевы. */
typedef struct {
  uint8_t pos; /* 0..255 */
  uint8_t r, g, b;
} GradStop;

/* тёмная тема: пол — тёмно-синий (не чёрный), дальше синий → голубой →
 * зелёный → жёлтый → оранжевый → красный → белый */
static const GradStop DARK_GRADIENT[] = {
    {0, 0, 0, 128},     {20, 0, 0, 220},    {40, 0, 32, 255},
    {80, 0, 180, 255},  {100, 0, 255, 255}, {140, 0, 255, 80},
    {160, 0, 255, 0},   {190, 160, 255, 0}, {210, 255, 255, 0},
    {230, 255, 160, 0}, {245, 255, 40, 0},  {255, 255, 255, 255},
};

/* светлая тема: от белого (шум) через голубой/зелёный к тёмно-красному */
static const GradStop LIGHT_GRADIENT[] = {
    {0, 255, 255, 255},  {24, 220, 240, 255}, {64, 120, 220, 255},
    {100, 0, 220, 220},  {140, 0, 180, 0},    {180, 200, 200, 0},
    {215, 240, 120, 0},  {240, 180, 0, 0},    {255, 90, 0, 0},
};

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint16_t gradientColor(uint8_t lvl) {
  const GradStop *st = gThemeLight ? LIGHT_GRADIENT : DARK_GRADIENT;
  uint8_t n = gThemeLight ? (uint8_t)ARRAY_SIZE(LIGHT_GRADIENT)
                          : (uint8_t)ARRAY_SIZE(DARK_GRADIENT);
  uint8_t seg = 0;
  while (seg + 1 < n && lvl > st[seg + 1].pos)
    seg++;
  const GradStop *a = &st[seg];
  const GradStop *b = &st[seg + 1 < n ? seg + 1 : seg];
  int t = b->pos > a->pos ? (int)(lvl - a->pos) * 255 / (b->pos - a->pos)
                          : 255;
  uint8_t r = (uint8_t)(a->r + ((int)b->r - a->r) * t / 255);
  uint8_t g = (uint8_t)(a->g + ((int)b->g - a->g) * t / 255);
  uint8_t bl = (uint8_t)(a->b + ((int)b->b - a->b) * t / 255);
  return rgb565(r, g, bl);
}

/* Цвет водопада по уровню RSSI (автоуровни — общие со столбцами спектра). */
static uint16_t levelColor(uint16_t rssi) {
  uint8_t lvl = (uint8_t)ConvertDomain(rssi, levelMin, levelMax, 0, 255);
  return gradientColor(lvl);
}

/* Цвет столбца — тот же градиент: высота столбца даёт уровень. В светлой
 * теме низ градиента почти белый, поэтому начинаем выше — иначе низкие
 * столбцы сливаются с фоном. */
static uint16_t barRamp[SP_MAX_POINTS];
static uint8_t barRampH;
static int8_t barRampTheme = -1;
static uint16_t colColor[SP_MAX_POINTS];

static void initBarRamp(uint8_t sh) {
  if (barRampH == sh && barRampTheme == (int8_t)gThemeLight)
    return;
  barRampH = sh;
  barRampTheme = gThemeLight;
  const uint16_t first = gThemeLight ? 64 : 0;
  for (uint8_t h = 0; h <= sh; h++) {
    uint16_t lvl = first;
    if (sh)
      lvl = (uint16_t)(first + (uint16_t)h * (255 - first) / sh);
    barRamp[h] = gradientColor((uint8_t)lvl);
  }
}

static uint16_t peakLineColor(void) {
  return gThemeLight ? rgb565(150, 0, 0) : C_WHITE;
}

static uint16_t v(uint8_t x) { return rssiHistory[x]; }

static uint8_t f2x(uint32_t f) {
  return ConvertDomain(f, range.start, range.end, 0, SP_MAX_POINTS - 1);
}

/* Пересчёт автоуровней по завершённому проходу: низ — минимум истории
 * (шумовой пол) минус два пикселя, верх — на размах не меньше SP_MIN_SPAN.
 * Вызывается из SP_Begin, чтобы водопад не дёргался на каждом неполном
 * свипе. */
static void computeAutoLevel(uint8_t sh) {
  if (filledPoints < 2 || sh == 0)
    return;

  uint16_t mn = UINT16_MAX, mx = 0;
  bool any = false;
  for (uint8_t i = 0; i < filledPoints; i++) {
    uint16_t r = rssiHistory[i];
    if (!r) /* столбец ещё не измерен */
      continue;
    any = true;
    if (r < mn)
      mn = r;
    if (r > mx)
      mx = r;
  }
  if (!any)
    return;

  int span = (int)mx - (int)mn;
  if (span < SP_MIN_SPAN)
    span = SP_MIN_SPAN;
  int lo = (int)mn - SP_FLOOR_MARGIN_PX * span / sh;
  if (lo < 0)
    lo = 0;

  levelMin = lo;
  levelMax = lo + span;
  levelValid = true;
  dBmRange.min = Rssi2DBm((uint16_t)levelMin);
  dBmRange.max = Rssi2DBm((uint16_t)levelMax);
}

void SP_ResetHistory(void) {
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
  levelValid = false;
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

void SP_Begin(void) {
  computeAutoLevel(levelHeight);
  currentStep = 0;
}

void SP_Next(void) {
  if (currentStep < stepsCount - 1)
    currentStep++;
}

void SP_Init(FRange *r, uint32_t stepSize, uint32_t _bw) {
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
  initBarRamp(sh);
  levelHeight = sh;
  /* До первого завершённого прохода уровней ещё нет — оценим по тому, что
   * успели измерить; дальше их пересчитывает SP_Begin. */
  if (!levelValid)
    computeAutoLevel(sh);

  decayPeaks();
  updatePeakFloor();

  for (uint8_t x = 0; x < SP_MAX_POINTS; x++) {
    int hv = ConvertDomain(v(x), levelMin, levelMax, 0, sh);
    uint8_t h = (uint8_t)(hv > sh ? sh : hv);
    topRow[x] = (uint8_t)(sh - h);
    colColor[x] = barRamp[h];

    int pv = ConvertDomain(peakHistory[x], levelMin, levelMax, 0, sh);
    uint8_t ph = (uint8_t)(pv > sh ? sh : pv);
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
  /* Подложка зоны скролла — цвет шумового пола, а не чёрный. Строки
   * приходят по одной за проход, и незаполненная чёрная область долго
   * контрастировала с синим шумом. */
  st7789_fill_rect_dma(0, wy, ST7789_WIDTH, wh, gradientColor(0));
  wfScroll = 0;
  wfScrollInited = true;
}

void WF_Render(uint8_t wy, bool wfDown) {
  uint16_t wh = ST7789_HEIGHT - wy;

  if (!wfScrollInited)
    WF_InitScroll(wy, wh);

  /* Полная горизонтальная детализация: каждый из 240 столбцов получает
   * свой цвет (раньше 120 значений растягивались на пары пикселей). */
  for (uint8_t x = 0; x < SP_MAX_POINTS; ++x)
    wfLine[x] = levelColor(rssiHistory[x]);

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

/* Залить зону водопада цветом шумового пола. Вызывается вместо чёрной
 * заливки, чтобы пустой водопад не давал резкую чёрную область рядом с
 * синим шумом. */
void WF_Clear(uint8_t wy) {
  if (wy >= ST7789_HEIGHT)
    return;
  st7789_fill_rect_dma(0, wy, ST7789_WIDTH, ST7789_HEIGHT - wy,
                       gradientColor(0));
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
