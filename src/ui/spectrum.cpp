#include "spectrum.h"
#include "driver/board.h"
#include "driver/st7789.h"
#include "helper/measurements.h"
#include "screen_layout.h"
#include <string.h>

static uint16_t rssiHistory[SP_MAX_POINTS] = {0};
static uint16_t peakHistory[SP_MAX_POINTS] = {0};
static uint16_t noiseHistory[SP_MAX_POINTS] = {0};
static bool markers[SP_MAX_POINTS] = {0};
static uint8_t lastX = 255;
static uint8_t filledPoints;

/* Накопитель текущего пикселя: пока скан идёт по его частотам, в историю
 * ничего не пишем. Иначе на первом замере столбец обнуляется, и если сильный
 * замер в этом пикселе не первый, пик «проваливается» и возвращается. */
static uint16_t curRssi;
static uint16_t curNoise;
static bool curOpen;
static void flushPixel(void);

static uint32_t stepsCount;
static uint32_t currentStep;
static uint32_t step;
static uint32_t bw;

static FRange range;

static DBmRange dBmRange = {-110, -50};

/* Стабильный уровень: низ шкалы — опора по шумовому полу, верх — на
 * фиксированном размахе. Максимум прохода больше не задаёт размах, а опора
 * пересчитывается только при заметном уходе пола. Иначе региональный подъём
 * шума на общем виде выглядит «сигналом», а после зума (когда опора уезжает
 * на локальный минимум) — пропадает. Единицы — сырой RSSI (0.5 дБ). */
#define SP_LEVEL_MIN_SPAN 60  /* не меньше 30 дБ по вертикали */
#define SP_LEVEL_MAX_SPAN 160 /* не больше 80 дБ */
#define SP_LEVEL_HEADROOM 40  /* 20 дБ над самым сильным пиком */
#define SP_FLOOR_OFFSET 6     /* 3 дБ под шумовым полом */
static int levelMin = 100;
static int levelMax = 100 + 80;
static bool levelValid;

/* Минимум сырых RSSI за проход. Пиксельная история хранит максимум по
 * нескольким замерам, и её минимум зависит от того, сколько замеров попало в
 * столбец: на широком диапазоне он выше, чем на узком. Уровень же должен
 * опираться на реальный шумовой пол и не зависеть от зума. */
static uint16_t sweepMin = UINT16_MAX;

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
 * опорными цветами прямо при отрисовке. Таблица в RAM не нужна — 240
 * вызовов на строку для Cortex-M4 дёшевы. Опорные точки взяты из палитр
 * ../dsp (SDRangel/SDR++). */
typedef struct {
  uint8_t pos; /* 0..255 */
  uint8_t r, g, b;
} GradStop;

typedef struct {
  const char *name;
  const GradStop *stops;
  uint8_t count;
} Palette;

/* GQRX: чёрный → синий → голубой → зелёный → жёлтый → красный → белый. */
static const GradStop GQRX_GRADIENT[] = {
    {0, 0x00, 0x00, 0x00},   {16, 0x00, 0x00, 0x00},
    {32, 0x00, 0x00, 0x21},  {48, 0x00, 0x00, 0x4E},
    {64, 0x00, 0x00, 0x7B},  {80, 0x14, 0x29, 0xB2},
    {96, 0x34, 0x6C, 0xEF},  {112, 0x6A, 0x9C, 0xC2},
    {128, 0xA9, 0xC5, 0x71}, {144, 0xE7, 0xEF, 0x1F},
    {160, 0xFF, 0xE6, 0x00}, {176, 0xFF, 0xBD, 0x00},
    {192, 0xFF, 0x94, 0x00}, {208, 0xFF, 0x6C, 0x00},
    {224, 0xFF, 0x43, 0x00}, {240, 0xFF, 0x1A, 0x00},
    {255, 0xFF, 0xFF, 0xFF},
};

static const GradStop VIRIDIS_GRADIENT[] = {
    {0, 0x44, 0x01, 0x54},   {25, 0x48, 0x28, 0x78},
    {50, 0x3E, 0x49, 0x89},  {75, 0x31, 0x68, 0x8E},
    {100, 0x26, 0x82, 0x8E}, {125, 0x1F, 0x9E, 0x89},
    {150, 0x35, 0xB7, 0x79}, {175, 0x6E, 0xCE, 0x58},
    {205, 0xB5, 0xDE, 0x2B}, {255, 0xFD, 0xE7, 0x25},
};

static const GradStop MAGMA_GRADIENT[] = {
    {0, 0x00, 0x00, 0x04},   {30, 0x1C, 0x10, 0x44},
    {55, 0x4F, 0x12, 0x7B},  {85, 0x81, 0x25, 0x81},
    {115, 0xBC, 0x39, 0x78}, {145, 0xE7, 0x52, 0x63},
    {175, 0xF8, 0x76, 0x5C}, {210, 0xFE, 0xBA, 0x7A},
    {255, 0xFC, 0xFD, 0xBF},
};

static const GradStop TURBO_GRADIENT[] = {
    {0, 0x30, 0x12, 0x3B},   {32, 0x41, 0x45, 0xAB},
    {56, 0x46, 0x75, 0xED},  {80, 0x39, 0xA2, 0xFC},
    {105, 0x1B, 0xCF, 0xD4}, {130, 0x24, 0xEC, 0xA6},
    {155, 0x61, 0xFC, 0x6C}, {180, 0xA4, 0xFC, 0x3B},
    {205, 0xD1, 0xE8, 0x34}, {228, 0xF9, 0xBA, 0x38},
    {243, 0xFE, 0x9B, 0x2D}, {250, 0xF5, 0x69, 0x1B},
    {253, 0xD9, 0x38, 0x06}, {255, 0x7A, 0x04, 0x03},
};

static const GradStop GRAY_GRADIENT[] = {
    {0, 0x00, 0x00, 0x00},
    {255, 0xFF, 0xFF, 0xFF},
};

static const Palette PALETTES[SP_PALETTE_COUNT] = {
    {"Gqrx", GQRX_GRADIENT, ARRAY_SIZE(GQRX_GRADIENT)},
    {"Viridis", VIRIDIS_GRADIENT, ARRAY_SIZE(VIRIDIS_GRADIENT)},
    {"Magma", MAGMA_GRADIENT, ARRAY_SIZE(MAGMA_GRADIENT)},
    {"Turbo", TURBO_GRADIENT, ARRAY_SIZE(TURBO_GRADIENT)},
    {"Gray", GRAY_GRADIENT, ARRAY_SIZE(GRAY_GRADIENT)},
};

uint8_t gWaterfallPalette;

const char *SP_PaletteName(uint8_t idx) {
  return PALETTES[idx < SP_PALETTE_COUNT ? idx : 0].name;
}

void SP_SetPalette(uint8_t idx) {
  gWaterfallPalette = idx < SP_PALETTE_COUNT ? idx : 0;
}

void SP_NextPalette(int8_t dir) {
  if (!dir)
    return;
  gWaterfallPalette =
      (uint8_t)((gWaterfallPalette + SP_PALETTE_COUNT + dir) % SP_PALETTE_COUNT);
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint16_t gradientColor(uint8_t lvl) {
  const Palette *p = &PALETTES[gWaterfallPalette];
  const GradStop *st = p->stops;
  uint8_t n = p->count;
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
  if (gThemeLight) { /* на белом фоне — инверсия с уходом шума в белый */
    uint16_t k = lvl;
    r = (uint8_t)(255u - (uint16_t)r * k / 255u);
    g = (uint8_t)(255u - (uint16_t)g * k / 255u);
    bl = (uint8_t)(255u - (uint16_t)bl * k / 255u);
  }
  return rgb565(r, g, bl);
}

/* Цвет водопада по уровню RSSI (автоуровни — общие со столбцами спектра). */
static uint16_t levelColor(uint16_t rssi) {
  uint8_t lvl = (uint8_t)ConvertDomain(rssi, levelMin, levelMax, 0, 255);
  return gradientColor(lvl);
}

/* Столбцы спектра — один спокойный цвет, без «радуги» по высоте. */
static uint16_t barColor(void) {
  return gThemeLight ? rgb565(40, 110, 88) : C_GREEN;
}

static uint16_t colColor[SP_MAX_POINTS];

static uint16_t peakLineColor(void) {
  return gThemeLight ? rgb565(150, 0, 0) : C_YELLOW;
}

static uint16_t v(uint8_t x) { return rssiHistory[x]; }

static uint8_t f2x(uint32_t f) {
  return ConvertDomain(f, range.start, range.end, 0, SP_MAX_POINTS - 1);
}

static void applyLevelRssi(int lo, int hi) {
  if (lo < 0)
    lo = 0;
  if (hi > 320) /* 0 дБм */
    hi = 320;
  if (hi < lo + SP_LEVEL_MIN_SPAN)
    hi = lo + SP_LEVEL_MIN_SPAN;
  if (hi > 320) {
    hi = 320;
    if (lo > hi - SP_LEVEL_MIN_SPAN)
      lo = hi - SP_LEVEL_MIN_SPAN;
  }
  if (lo < 0)
    lo = 0;
  levelMin = lo;
  levelMax = hi;
  levelValid = true;
  dBmRange.min = Rssi2DBm((uint16_t)levelMin);
  dBmRange.max = Rssi2DBm((uint16_t)levelMax);
}

static int dbmToRssi(int dbm) {
  if (dbm < -160)
    dbm = -160;
  if (dbm > 0)
    dbm = 0;
  return (int)DBm2Rssi((int16_t)dbm);
}

/* Auto как в SA ../dsp: один раз подобрать окно под текущий кадр — дно чуть
 * ниже шума, верх на 20 дБ выше самого сильного пика, размах не меньше
 * 30 дБ. Дальше окно живёт, пока пользователь не нажмёт Auto снова. */
static void fitLevelFromHistory(void) {
  uint16_t mn = UINT16_MAX;
  uint16_t mx = 0;
  for (uint8_t i = 0; i < SP_MAX_POINTS; i++) {
    uint16_t r = rssiHistory[i];
    if (!r)
      continue;
    if (r < mn)
      mn = r;
    if (peakHistory[i] > mx)
      mx = peakHistory[i];
  }
  if (mn == UINT16_MAX) {
    if (sweepMin == UINT16_MAX)
      return;
    mn = sweepMin;
    mx = sweepMin;
  }
  if (mx < mn)
    mx = mn;
  int lo = (int)mn - SP_FLOOR_OFFSET;
  int hi = (int)mx + SP_LEVEL_HEADROOM;
  if (hi - lo < SP_LEVEL_MIN_SPAN)
    hi = lo + SP_LEVEL_MIN_SPAN;
  if (hi - lo > SP_LEVEL_MAX_SPAN)
    hi = lo + SP_LEVEL_MAX_SPAN;
  applyLevelRssi(lo, hi);
}

static void computeAutoLevel(void) {
  if (!levelValid)
    fitLevelFromHistory();
}

int16_t SP_LevelMin(void) { return Rssi2DBm((uint16_t)levelMin); }
int16_t SP_LevelMax(void) { return Rssi2DBm((uint16_t)levelMax); }

void SP_SetLevelMin(int16_t dbm) {
  int lo = dbmToRssi(dbm);
  int hi = levelMax;
  if (lo > hi - SP_LEVEL_MIN_SPAN)
    lo = hi - SP_LEVEL_MIN_SPAN;
  applyLevelRssi(lo, hi);
}

void SP_SetLevelMax(int16_t dbm) {
  int lo = levelMin;
  int hi = dbmToRssi(dbm);
  if (hi < lo + SP_LEVEL_MIN_SPAN)
    hi = lo + SP_LEVEL_MIN_SPAN;
  applyLevelRssi(lo, hi);
}

void SP_LevelAutoFit(void) { fitLevelFromHistory(); }

/* Сбросить опору уровня. Нужно при входе в режим: у BK и SI разные шкалы
 * RSSI, и унаследованная опора сделала бы спектр пустым. Зум/смена шага
 * внутри режима опору не сбрасывают. */
void SP_ResetLevel(void) {
  levelValid = false;
  sweepMin = UINT16_MAX;
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
  curRssi = 0;
  curNoise = UINT16_MAX;
  curOpen = false;
  sweepMin = UINT16_MAX;
  filledPoints = 0;
  currentStep = 0;
  /* levelValid не сбрасываем: зум/смена шага не должны пересчитывать шкалу
   * с нуля — иначе уровень «прыгает» и пики меняют высоту. Вниз опора
   * опускается сразу, вверх — только при заметном уходе. */
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

void SP_Begin(void) {
  /* Проход мог закончиться не на range.end (каналы/сетка): дописываем
   * накопленный пиксель до пересчёта уровня. */
  flushPixel();
  computeAutoLevel(); /* один раз после входа в режим */
  sweepMin = UINT16_MAX; /* следующий проход считает свой пол */
  currentStep = 0;
}

void SP_Init(FRange *r, uint32_t stepSize, uint32_t _bw) {
  if (!stepSize)
    stepSize = 1;
  uint32_t start = r->start, end = r->end;
  /* Защита от перевёрнутых/нулевых границ: иначе (end-start) переполняется
   * и f2x возвращает индекс за массивом истории. */
  if (end <= start)
    end = start + stepSize;
  filledPoints = 0;
  step = stepSize;
  bw = _bw;
  range.start = start;
  range.end = end;
  stepsCount = (end - start) / stepSize + 1;
  SP_ResetHistory();
  SP_Begin();
  ticksRendered = false;
  forceRedraw = true; /* новая сетка/диапазон: рисуем все столбцы, а не diff */
}

/* Завершить накопленный пиксель и записать его в историю. */
static void flushPixel(void) {
  if (lastX >= SP_MAX_POINTS)
    return;
  rssiHistory[lastX] = curRssi;
  noiseHistory[lastX] = curNoise;
  markers[lastX] = curOpen;
  if (curRssi > peakHistory[lastX])
    peakHistory[lastX] = curRssi;
  lastX = 255;
}

/* Залить пустые столбцы [from, to) уровнем from-1 (шаг больше ширины
 * пикселя — измерений на них нет). */
static void fillGap(uint8_t from, uint8_t to) {
  if (from == 0 || from >= SP_MAX_POINTS)
    return;
  uint16_t r = rssiHistory[from - 1];
  for (uint8_t nx = from; nx < to; ++nx) {
    rssiHistory[nx] = r;
    noiseHistory[nx] = noiseHistory[from - 1];
    markers[nx] = markers[from - 1];
    if (peakHistory[nx] < r)
      peakHistory[nx] = r;
  }
}

void SP_AddPoint(Measurement *msm) {
  uint8_t x = f2x(msm->f);
  if (msm->rssi && msm->rssi < sweepMin)
    sweepMin = msm->rssi;
  if (lastX != x) {
    uint8_t prev = lastX;
    flushPixel();
    if (prev < x)
      fillGap(prev + 1, x);
    lastX = x;
    curRssi = 0;
    curNoise = UINT16_MAX;
    curOpen = false;
  }
  if (msm->rssi > curRssi)
    curRssi = msm->rssi;
  if (msm->noise < curNoise)
    curNoise = msm->noise;
  if (msm->open)
    curOpen = true;
  /* последняя точка диапазона: завершаем пиксель и закрашиваем хвост до
   * края (иначе правый столбец никогда не заполнялся — чёрная полоска) */
  if (msm->f + step >= range.end) {
    uint8_t last = lastX;
    flushPixel();
    fillGap(last + 1, SP_MAX_POINTS);
    filledPoints = SP_MAX_POINTS;
  } else if (x > filledPoints && x < SP_MAX_POINTS) {
    filledPoints = x + 1;
  }
}

static void drawTicks(uint8_t y, uint32_t fs, uint32_t fe, uint32_t div,
                      uint8_t h, uint16_t c) {
  for (uint32_t f = fs - (fs % div) + div; f < fe; f += div) {
    uint8_t tx = f2x(f);
    st7789_fill_rect_dma(tx, y, 1, h, c);
  }
}

/* Риски под спектром: базовая линия, минорные и мажорные штрихи. */
static void SP_DrawTicks(uint8_t y, uint8_t h, FRange *p) {
  uint32_t fs = p->start;
  uint32_t fe = p->end;
  uint32_t bw = fe - fs;
  const uint8_t ty = (uint8_t)(y + h);
  const uint16_t major = gThemeLight ? C_BLACK : C_WHITE;

  st7789_fill_rect_dma(0, ty, SP_MAX_POINTS, 1, C_GRAY);

  for (uint32_t d = 100000000; d >= 10; d /= 10) {
    if (d < bw) {
      drawTicks(ty, fs, fe, d / 2, UI_TICKS_H - 1, C_GRAY);
      drawTicks(ty, fs, fe, d, UI_TICKS_H, major);
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
  decayPeaks();
  updatePeakFloor();

  for (uint8_t x = 0; x < SP_MAX_POINTS; x++) {
    int hv = ConvertDomain(v(x), levelMin, levelMax, 0, sh);
    uint8_t h = (uint8_t)(hv > sh ? sh : hv);
    topRow[x] = (uint8_t)(sh - h);
    colColor[x] = barColor();

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

  /* Риски под спектром столбцы не перекрывают; перерисовываем их только
   * при полном кадре или смене диапазона/сетки. */
  if (full || !ticksRendered) {
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
