#include "spectrum.h"
#include "driver/st7789.h"
#include "helper/measurements.h"
#include <string.h>

static uint16_t rssiHistory[SP_MAX_POINTS] = {0};
static uint16_t noiseHistory[SP_MAX_POINTS] = {0};
static bool markers[SP_MAX_POINTS] = {0};
static bool needRedraw[SP_MAX_POINTS] = {0};
static uint8_t lastX = 255;
static uint8_t filledPoints;

static uint32_t stepsCount;
static uint32_t currentStep;
static uint32_t step;
static uint32_t bw;

static FRange range;

static DBmRange dBmRange = {-110, -50};

static bool ticksRendered = false;

static uint8_t wf[WF_YN][WF_XN] = {0};
static uint16_t osy[SP_MAX_POINTS] = {0};

/* static const uint16_t GRADIENT_PALETTE[] = {
    0x2000, 0x3000, 0x5000, 0x9000, 0xfc44, 0xffbf, 0x7bf, 0x1b5f,
    0x1b5f, 0x1f,   0x1f,   0x18,   0x13,   0xe,    0x9,
}; */
/* static const uint16_t GRADIENT_PALETTE[] = {
    0x0000, // black
    0x0002, // almost black blue
    0x0006, // very dark blue
    0x0010, // dark blue
    0x0019, // deep blue
    0x0030, // blue
    0x0050, // saturated blue
    0x0078, // bright blue
    0x03BF, // blue-cyan
    0x057F, // cyan
    0x06FF, // bright cyan
    0x97FF, // pale cyan
    0xBFFF, // very light cyan
    0xDFFF, // near white with blue tint
    0xEFFF, // almost white
}; */
static const uint16_t GRADIENT_PALETTE[] = {
    0x0000, // black
    0x000A, // almost black blue
    0x0016, // very dark blue
    0x001F, // blue
    0x05BF, // cyan-blue
    0x07F9, // cyan
    0x06E0, // green
    0xA7E0, // yellow-green
    0xFFE0, // yellow
    0xFC60, // orange
    0xF8E0, // red-orange
    0xF800, // red
    0xFB2C, // light red
    0xFE59, // near white
    0xFFFF, // white
};

static uint8_t getPalIndex(uint16_t rssi) {
  return ConvertDomain(Rssi2DBm(rssi), dBmRange.min, dBmRange.max, 0,
                       ARRAY_SIZE(GRADIENT_PALETTE) - 1);
}

static uint16_t v(uint8_t x) { return rssiHistory[x]; }

static uint8_t f2x(uint32_t f) {
  return ConvertDomain(f, range.start, range.end, 0, SP_MAX_POINTS - 1);
}

void SP_ResetHistory(void) {
  for (uint8_t y = 0; y < ARRAY_SIZE(wf); ++y)
    memset(wf[y], 0, ARRAY_SIZE(wf[0]));
  for (uint8_t i = 0; i < SP_MAX_POINTS; ++i) {
    osy[i] = rssiHistory[i] = 0;
    noiseHistory[i] = UINT16_MAX;
    markers[i] = false;
    needRedraw[i] = false;
  }
  filledPoints = 0;
  currentStep = 0;
}

void SP_ResetRender(void) {
  ticksRendered = false;
  for (uint8_t i = 0; i < SP_MAX_POINTS; ++i)
    needRedraw[i] = true;
}

void SP_Begin(void) { currentStep = 0; }

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

void SP_AddPoint(Loot *msm) {
  uint8_t x = f2x(msm->f);
  if (lastX != x) {
    lastX = x;
    rssiHistory[x] = markers[x] = 0;
    noiseHistory[x] = UINT16_MAX;
  }
  if (msm->rssi > rssiHistory[x])
    rssiHistory[x] = msm->rssi;
  if (msm->noise < noiseHistory[x])
    noiseHistory[x] = msm->noise;
  if (markers[x] == false && msm->open)
    markers[x] = msm->open;
  uint8_t XL = f2x(msm->f + step);
  for (uint8_t nx = x + 1; nx < XL; ++nx) {
    rssiHistory[nx] = rssiHistory[x];
    noiseHistory[nx] = noiseHistory[x];
    markers[nx] = markers[x];
  }
  if (x > filledPoints && x < SP_MAX_POINTS)
    filledPoints = x + 1;
}

typedef struct {
  uint8_t x0;
  uint8_t x1; // not inclusive
  uint16_t v;
} Seg;

static Seg seg_from_f(uint32_t f, uint16_t v) {
  uint8_t x0 = f2x(f);
  uint8_t x1;

  if (f + step <= range.end) {
    x1 = f2x(f + step);
    if (x1 <= x0)
      x1 = x0 + 1;
  } else {
    x1 = SP_MAX_POINTS;
  }

  if (x1 > SP_MAX_POINTS)
    x1 = SP_MAX_POINTS;

  return (Seg){x0, x1, v};
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

static void renderWf(uint32_t f, uint16_t val) {
  Seg s = seg_from_f(f, val);
  uint8_t pal = getPalIndex(s.v);

  for (uint8_t x = s.x0; x < s.x1; ++x) {
    uint8_t bx = x >> 1;
    if (bx >= WF_XN)
      break;

    if ((x & 1) == 0) {
      wf[0][bx] &= 0xF0;
      wf[0][bx] |= pal;
    } else {
      wf[0][bx] &= 0x0F;
      wf[0][bx] |= pal << 4;
    }
  }
}

static uint16_t nsy[SP_MAX_POINTS] = {0};

static uint8_t spDirty[SP_MAX_POINTS];

// Буфер для одной строки спектра: 240 пикселей × 2 байта = 480 байт
static uint16_t sp_line_buf[SP_MAX_POINTS];

// 2 × 240 × 2 = 960 байт — буферы уже в big-endian для SPI
#define SP_BYTES (SP_MAX_POINTS * 2)
static uint8_t sp_buf[2][SP_BYTES];

static void fill_sp_row(uint8_t *buf, int16_t row) {
  for (uint8_t x = 0; x < SP_MAX_POINTS; x++) {
    uint16_t c = ((int16_t)nsy[x] > row) ? C_CYAN : C_BLACK;
    buf[x * 2] = (uint8_t)(c >> 8);
    buf[x * 2 + 1] = (uint8_t)(c);
  }
}

void SP_Render(FRange *p, uint8_t sy, uint8_t sh) {
  const uint16_t vMin = (dBmRange.min + 160) * 2;
  const uint16_t vMax = (dBmRange.max + 160) * 2;
  dBmRange.min = Rssi2DBm(vMin);
  dBmRange.max = Rssi2DBm(vMax);

  memset(nsy, 0, sizeof(nsy));
  for (uint32_t f = range.start; f <= range.end; f += step) {
    uint16_t hv = ConvertDomain(v(f2x(f)) * 2, vMin * 2, vMax * 2, 0, sh);
    Seg s = seg_from_f(f, hv);
    if (s.v > sh)
      s.v = sh;
    for (uint8_t x = s.x0; x < s.x1; x++)
      nsy[x] = s.v;
  }

  uint8_t min_y = sh, max_y = 0;
  bool any = false;
  for (uint8_t x = 0; x < SP_MAX_POINTS; x++) {
    if (nsy[x] != osy[x]) {
      uint8_t lo = nsy[x] < osy[x] ? nsy[x] : osy[x];
      uint8_t hi = nsy[x] > osy[x] ? nsy[x] : osy[x];
      if (lo < min_y)
        min_y = lo;
      if (hi > max_y)
        max_y = hi;
      any = true;
    }
  }

  if (!any) {
    SP_DrawTicks(sy, sh, p);
    return;
  }

  uint16_t top_y = sy + sh - 1 - max_y;
  uint8_t rect_h = max_y - min_y + 1;

  st7789_cs_low();
  st7789_set_addr_window_raw(0, top_y, SP_MAX_POINTS, rect_h);

  // Первую строку готовим заранее и сразу стартуем DMA
  uint8_t cur = 0;
  fill_sp_row(sp_buf[cur], max_y);
  st7789_dma_start(sp_buf[cur], SP_BYTES);

  // Со второй строки: CPU заполняет buf[cur^1] пока DMA гонит buf[cur]
  for (int16_t row = max_y - 1; row >= (int16_t)min_y; row--) {
    cur ^= 1;
    fill_sp_row(sp_buf[cur], row);
    st7789_dma_wait();
    st7789_dma_start(sp_buf[cur], SP_BYTES);
  }

  st7789_dma_wait();
  st7789_cs_high();

  memcpy(osy, nsy, sizeof(nsy));
  SP_DrawTicks(sy, sh, p);
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

  memset(wf[0], 0, WF_XN);
  for (uint32_t f = range.start; f <= range.end; f += step) {
    uint8_t x = f2x(f);
    renderWf(f, rssiHistory[x]);
  }

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
  uint16_t vsp = wy + (wfScroll + 1) % wh;
  st7789_set_vscroll_start(vsp);
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
