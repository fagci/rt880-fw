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

static DBmRange dBmRange = {-150, -40};

static bool ticksRendered = false;

static uint8_t wf[WF_YN][WF_XN] = {0};
static uint16_t osy[SP_MAX_POINTS] = {0};

/* static const uint16_t GRADIENT_PALETTE[] = {
    0x2000, 0x3000, 0x5000, 0x9000, 0xfc44, 0xffbf, 0x7bf, 0x1b5f,
    0x1b5f, 0x1f,   0x1f,   0x18,   0x13,   0xe,    0x9,
}; */
static const uint16_t GRADIENT_PALETTE[] = {
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

void SP_Render(FRange *p, uint8_t sy, uint8_t sh) {
  const uint16_t rssiMin = Min(rssiHistory, filledPoints);
  const uint16_t noiseFloor = SP_GetNoiseFloor();
  const uint16_t rssiMax = Max(rssiHistory, filledPoints);

  const uint16_t vMin = rssiMin - 1;
  const uint16_t vMax =
      rssiMax + ((rssiMax - noiseFloor) < 40 ? (rssiMax - noiseFloor) : 40);

  dBmRange.min = Rssi2DBm(vMin);
  dBmRange.max = Rssi2DBm(vMax);

  for (uint32_t x = 0; x < SP_MAX_POINTS; ++x)
    nsy[x] = 0;

  for (uint32_t f = range.start; f <= range.end; f += step) {
    Seg s =
        seg_from_f(f, ConvertDomain(v(f2x(f)) * 2, vMin * 2, vMax * 2, 0, sh));

    if (s.v > sh)
      s.v = sh;

    for (uint8_t x = s.x0; x < s.x1; ++x)
      nsy[x] = s.v;
  }

  for (uint8_t x = 0; x < SP_MAX_POINTS; ++x) {
    uint16_t oldv = osy[x];
    uint16_t newv = nsy[x];

    if (oldv == newv)
      continue;

    if (newv > oldv) {
      uint8_t top = sy + (sh - newv);
      uint8_t h = newv - oldv;
      st7789_fill_rect_dma(x, top, 1, h, C_WHITE);
    } else {
      uint8_t top = sy + (sh - oldv);
      uint8_t h = oldv - newv;
      st7789_fill_rect_dma(x, top, 1, h, C_BLACK);
    }

    osy[x] = newv;
  }

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
    // wfScroll = (wfScroll + 1) % wh;
    wfScroll = (wfScroll == 0) ? (wh - 1) : (wfScroll - 1);
  } else {
    wfScroll = (wfScroll == 0) ? (wh - 1) : (wfScroll - 1);
  }

  st7789_set_vscroll_start(wy + wfScroll);

  uint16_t localY;
  if (wfDown) {
    localY = (wfScroll + wh - 1) % wh;
  } else {
    localY = wfScroll;
  }

  uint16_t drawY = wy + localY;

  st7789_cs_low();
  st7789_set_addr_window_raw(0, drawY, SP_MAX_POINTS, 1);
  st7789_write_pixels_dma(wfLine, SP_MAX_POINTS);
  st7789_cs_high();
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
