#include "analyser.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/log.h"
#include "../helper/measurements.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "mode.h"
#include "vfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATUS_H 24
#define SPECTRUM_H 54
#define SPECTRUM_Y (STATUS_H + 14)
#define CURSOR_Y (SPECTRUM_Y + SPECTRUM_H + 1)
#define CURSOR_H 11
#define WF_Y (CURSOR_Y + CURSOR_H)

#define CURSOR_TIMEOUT_MS 5000

#define DEFAULT_RANGE_START 43300000U

typedef struct {
  FRange range;
  uint32_t scanF;
  uint8_t savedVfo;
  bool spectrumReady;
  uint32_t cursorTimeout;
  uint8_t cursorCenter;
  uint8_t cursorHalfW;
  bool cursorActive;
  uint32_t lastStepTime;
  uint32_t stepsPerSec;
  uint32_t stepCount;
  uint8_t prevCursorStart;
  uint8_t prevCursorEnd;
  bool prevCursorActive;
} AnaCtx;

static AnaCtx ctx;

static uint8_t getCursorStart(void) {
  if (ctx.cursorCenter >= ctx.cursorHalfW)
    return ctx.cursorCenter - ctx.cursorHalfW;
  return 0;
}

static uint8_t getCursorEnd(void) {
  uint8_t e = ctx.cursorCenter + ctx.cursorHalfW;
  if (e >= SP_MAX_POINTS)
    e = SP_MAX_POINTS - 1;
  return e;
}

static void initRange(void) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  ctx.range.start = DEFAULT_RANGE_START;
  ctx.range.end = ctx.range.start + step * SP_MAX_POINTS;
  ctx.scanF = ctx.range.start;
}

static void onRangeChanged(void) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  SP_Init(&ctx.range, step, ctx.range.end - ctx.range.start);
  ctx.scanF = ctx.range.start;
  ctx.spectrumReady = false;
  ctx.cursorCenter = SP_MAX_POINTS / 2;
  ctx.cursorHalfW = SP_MAX_POINTS / 6;
  ctx.cursorActive = true;
  ctx.cursorTimeout = millis() + CURSOR_TIMEOUT_MS;
  ctx.stepCount = 0;
  ctx.lastStepTime = millis();
  ctx.stepsPerSec = 0;
  ctx.prevCursorActive = false;

  FillRect(0, STATUS_H, LCD_WIDTH, WF_Y - STATUS_H, C_BLACK);
}

static void shiftRange(int32_t deltaStep) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  uint32_t shift = (uint32_t)(deltaStep > 0 ? deltaStep : -deltaStep) * step;
  uint32_t span = ctx.range.end - ctx.range.start;

  if (deltaStep > 0) {
    if (ctx.range.start + shift > BK4819_F_MAX - span)
      return;
    ctx.range.start += shift;
    ctx.range.end += shift;
  } else {
    if (ctx.range.start < shift + span) {
      ctx.range.start = 0;
      ctx.range.end = span;
    } else {
      ctx.range.start -= shift;
      ctx.range.end -= shift;
    }
  }

  onRangeChanged();
}

static void zoomIn(void) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  uint8_t x0 = getCursorStart();
  uint8_t x1 = getCursorEnd();
  if (x1 - x0 < 10)
    return;
  uint32_t span = ctx.range.end - ctx.range.start;
  uint32_t newStart = ctx.range.start + (uint32_t)x0 * span / SP_MAX_POINTS;
  uint32_t newEnd = ctx.range.start + (uint32_t)x1 * span / SP_MAX_POINTS;
  if (newEnd - newStart >= step * 10) {
    ctx.range.start = newStart;
    ctx.range.end = newEnd;
    onRangeChanged();
  }
}

static void zoomOut(void) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  uint32_t span = ctx.range.end - ctx.range.start;
  uint32_t newSpan = span * 2;
  uint32_t newStart = ctx.range.start - span / 2;
  if (newStart > BK4819_F_MAX - newSpan)
    newStart = BK4819_F_MAX - newSpan;
  ctx.range.start = newStart;
  ctx.range.end = newStart + newSpan;
  if (ctx.range.end > BK4819_F_MAX) {
    ctx.range.end = BK4819_F_MAX;
    ctx.range.start = ctx.range.end - newSpan;
  }
  if (ctx.range.end - ctx.range.start >= step * 10)
    onRangeChanged();
}

static void moveCursorZone(int8_t dir) {
  int16_t newCenter = (int16_t)ctx.cursorCenter + dir * 5;
  if (newCenter < (int16_t)ctx.cursorHalfW)
    newCenter = ctx.cursorHalfW;
  if (newCenter > (int16_t)(SP_MAX_POINTS - 1 - ctx.cursorHalfW))
    newCenter = SP_MAX_POINTS - 1 - ctx.cursorHalfW;
  ctx.cursorCenter = (uint8_t)newCenter;
  ctx.cursorActive = true;
  ctx.cursorTimeout = millis() + CURSOR_TIMEOUT_MS;
}

static void resizeCursor(int8_t dir) {
  int16_t newHalf = (int16_t)ctx.cursorHalfW + dir * 3;
  if (newHalf < 3)
    newHalf = 3;
  if (newHalf > SP_MAX_POINTS / 2)
    newHalf = SP_MAX_POINTS / 2;
  ctx.cursorHalfW = (uint8_t)newHalf;
  ctx.cursorActive = true;
  ctx.cursorTimeout = millis() + CURSOR_TIMEOUT_MS;
}

static uint32_t xToFreq(uint8_t x) {
  uint32_t span = ctx.range.end - ctx.range.start;
  return ctx.range.start + (uint32_t)x * span / SP_MAX_POINTS;
}

static void eraseCursorZone(void) {
  if (!ctx.prevCursorActive)
    return;

  uint8_t x0 = ctx.prevCursorStart;
  uint8_t x1 = ctx.prevCursorEnd;

  DrawHLine(x0, SPECTRUM_Y, x1 - x0 + 1, C_BLACK);
  DrawHLine(x0, SPECTRUM_Y + SPECTRUM_H - 1, x1 - x0 + 1, C_BLACK);
  DrawVLine(x0, SPECTRUM_Y, SPECTRUM_H, C_BLACK);
  DrawVLine(x1, SPECTRUM_Y, SPECTRUM_H, C_BLACK);

  ctx.prevCursorActive = false;
}

static void drawCursorZone(void) {
  uint8_t x0 = getCursorStart();
  uint8_t x1 = getCursorEnd();

  DrawHLine(x0, SPECTRUM_Y, x1 - x0 + 1, C_CYAN);
  DrawHLine(x0, SPECTRUM_Y + SPECTRUM_H - 1, x1 - x0 + 1, C_CYAN);
  DrawVLine(x0, SPECTRUM_Y, SPECTRUM_H, C_CYAN);
  DrawVLine(x1, SPECTRUM_Y, SPECTRUM_H, C_CYAN);

  ctx.prevCursorStart = x0;
  ctx.prevCursorEnd = x1;
  ctx.prevCursorActive = true;
}

static void onFreqInput(uint32_t val1, uint32_t val2) {
  (void)val2;
  if (val1 >= BK4819_F_MIN && val1 <= BK4819_F_MAX) {
    uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
    uint32_t span = step * SP_MAX_POINTS;
    ctx.range.start = val1;
    ctx.range.end = val1 + span;
    onRangeChanged();
  }
}

static void enter(Mode_t *self) {
  (void)self;
  ctx.savedVfo = currentVfo;
  if (vfos[currentVfo].radio != RADIO_BK4819A)
    currentVfo = 0;

  initRange();
  Radio_Init();
  onRangeChanged();

  FillRect(0, WF_Y, LCD_WIDTH, LCD_HEIGHT - WF_Y, C_BLACK);
}

#define SCAN_STEPS_PER_UPDATE 128

static void update(Mode_t *self) {
  (void)self;
  if (gFInputActive) {
    FINPUT_update();
    return;
  }
  if (ctx.spectrumReady)
    return;

  for (uint8_t i = 0; i < SCAN_STEPS_PER_UPDATE; i++) {
    Radio_TuneTo(ctx.scanF, false);
    BK4819_SelectChip(0);
    Loot m = {
        .f = ctx.scanF,
        .rssi = BK4819_GetRSSI(),
        .noise = 0,
        .open = false,
    };
    SP_AddPoint(&m);
    ctx.stepCount++;

    uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
    ctx.scanF += step;

    if (ctx.scanF >= ctx.range.end) {
      ctx.spectrumReady = true;
      Radio_TuneTo(ctx.range.start - step, false);
      break;
    }
  }

  uint32_t now = millis();
  if (now - ctx.lastStepTime >= 1000) {
    ctx.stepsPerSec = ctx.stepCount * 1000 / (now - ctx.lastStepTime);
    ctx.stepCount = 0;
    ctx.lastStepTime = now;
  }
}

static uint32_t lastSpectrumRender;

static void renderCursorInfo(void) {
  FillRect(0, CURSOR_Y, LCD_WIDTH, WF_Y - CURSOR_Y, C_BLACK);

  uint16_t textY = WF_Y - 1;

  if (ctx.cursorActive && millis() <= ctx.cursorTimeout) {
    uint8_t x0 = getCursorStart();
    uint8_t x1 = getCursorEnd();
    uint32_t leftF = xToFreq(x0);
    uint32_t rightF = xToFreq(x1);
    uint32_t centerF = xToFreq(ctx.cursorCenter);
    char buf[24];

    snprintf(buf, sizeof(buf), "%lu.%03lu", leftF / MHZ, (leftF % MHZ) / 1000);
    PrintfEx(4, textY, POS_L, C_CYAN, C_BLACK, F_SS, "%s", buf);

    snprintf(buf, sizeof(buf), "%lu.%03lu", centerF / MHZ, (centerF % MHZ) / 1000);
    PrintfEx(LCD_XCENTER, textY, POS_C, C_WHITE, C_BLACK, F_SM, "%s", buf);

    snprintf(buf, sizeof(buf), "%lu.%03lu", rightF / MHZ, (rightF % MHZ) / 1000);
    PrintfEx(LCD_WIDTH - 4, textY, POS_R, C_CYAN, C_BLACK, F_SS, "%s", buf);
  } else {
    uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
    uint32_t span = ctx.range.end - ctx.range.start;
    char buf[24];

    snprintf(buf, sizeof(buf), "Step %lu.%02luk", step / 100, step % 100);
    PrintfEx(4, textY, POS_L, C_GRAY, C_BLACK, F_SS, "%s", buf);

    snprintf(buf, sizeof(buf), "%4lu/s", ctx.stepsPerSec);
    PrintfEx(LCD_WIDTH - 4, textY, POS_R, C_GRAY, C_BLACK, F_SS, "%s", buf);

    snprintf(buf, sizeof(buf), "%lu MHz", span / MHZ);
    PrintfEx(LCD_XCENTER, textY, POS_C, C_GRAY, C_BLACK, F_SS, "%s", buf);

    ctx.cursorActive = false;
  }
}

static void render(Mode_t *self) {
  (void)self;
  STATUSLINE_render();

  if (gFInputActive) {
    FINPUT_render();
    return;
  }

  bool spectrumRedrawn = false;
  if (ctx.spectrumReady || millis() - lastSpectrumRender >= 500) {
    SP_Render(&ctx.range, STATUS_H + 14, SPECTRUM_H);
    lastSpectrumRender = millis();
    spectrumRedrawn = true;
  }

  if (ctx.spectrumReady) {
    WF_Render(WF_Y, true);
    ctx.spectrumReady = false;
    SP_ResetHistory();
    SP_ResetRender();
    SP_Begin();
    ctx.scanF = ctx.range.start;
  }

  if (spectrumRedrawn) {
    ctx.prevCursorActive = false;
  }
  eraseCursorZone();
  if (ctx.cursorActive && millis() <= ctx.cursorTimeout) {
    drawCursorZone();
  }
  renderCursorInfo();
}

static void exitMode(Mode_t *self) {
  (void)self;
  currentVfo = ctx.savedVfo;
}

static bool key(Mode_t *self, key_id_t k, key_evt_type_t state) {
  (void)self;

  if (gFInputActive) {
    if (FINPUT_key(k, state))
      return true;
    return false;
  }

  if (state == KEY_LONG_PRESSED_CONT) {
    if (k == KEY_UP) {
      shiftRange(+1);
      return true;
    }
    if (k == KEY_DOWN) {
      shiftRange(-1);
      return true;
    }
    return false;
  }

  if (state != KEY_EVT_RELEASE)
    return false;

  switch (k) {
  case KEY_MENU:
    Mode_Switch(&MODE_VFO);
    return true;
  case KEY_UP:
    moveCursorZone(+1);
    return true;
  case KEY_DOWN:
    moveCursorZone(-1);
    return true;
  case KEY_1:
    resizeCursor(-1);
    return true;
  case KEY_7:
    resizeCursor(+1);
    return true;
  case KEY_2:
    zoomIn();
    return true;
  case KEY_8:
    zoomOut();
    return true;
  case KEY_3:
  case KEY_9:
    vfos[currentVfo].step = (k == KEY_3)
        ? (vfos[currentVfo].step + 1) % STEP_COUNT
        : (vfos[currentVfo].step + STEP_COUNT - 1) % STEP_COUNT;
    onRangeChanged();
    return true;
  case KEY_0:
    FINPUT_setup(BK4819_F_MIN, BK4819_F_MAX, UNIT_MHZ, false);
    FINPUT_Show(onFreqInput);
    return true;
  case KEY_EXIT:
    ctx.cursorActive = false;
    return true;
  default:
    break;
  }
  return false;
}

Mode_t MODE_ANALYSER = {
    .enter = enter,
    .exit = exitMode,
    .update = update,
    .render = render,
    .key = key,
    .ctx = &ctx,
};
