#include "scanner.h"
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
#include "../ui/numval.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "mode.h"
#include "vfo.h"
#include <stdlib.h>

#define STATUS_H 24
#define SPECTRUM_H 64
#define SPECTRUM_Y (STATUS_H + 2 + 12)

static FRange range = {
    .start = 400 * MHZ,
    .end = 470 * MHZ,
};

typedef struct {
  NumVal_t numVal[2];
} MainCtx_t;

static MainCtx_t ctx;

static bool spectrumReady;
static bool endFSel;
static uint8_t savedVfo;

static uint32_t stepCount;
static uint32_t lastStepTime;
static uint32_t stepsPerSec;

static void onRangeChanged() {
  SP_Init(&range, StepFrequencyTable[vfos[currentVfo].step],
          range.end - range.start);
  Radio_TuneTo(range.start - StepFrequencyTable[vfos[currentVfo].step], true);
  FillRect(0, STATUS_H, LCD_WIDTH, SPECTRUM_Y + SPECTRUM_H - STATUS_H, C_BLACK);
  NumVal_Invalidate(&ctx.numVal[0]);
  NumVal_Invalidate(&ctx.numVal[1]);
  stepCount = 0;
  lastStepTime = millis();
  stepsPerSec = 0;
}

static void onStartEntered(uint32_t f) {
  range.start = f;
  onRangeChanged();
}
static void onEndEntered(uint32_t f) {
  range.end = f;
  onRangeChanged();
}

static void initNumVals(void) {
  NumVal_Init(&ctx.numVal[0], 8, SPECTRUM_Y - 2, F_SM, 11, 12, UNIT_MHZ,
              range.start, 0, 1340 * MHZ, onStartEntered, POS_L, NULL, 0, 0);
  NumVal_Init(&ctx.numVal[1], LCD_WIDTH - 8, SPECTRUM_Y - 2, F_SM, 11, 12,
              UNIT_MHZ, range.end, 0, 1340 * MHZ, onEndEntered, POS_R, NULL, 0, 0);
}

// ── Mode callbacks ──────────────────────────────

static void enter(Mode_t *self) {
  (void)self;
  savedVfo = currentVfo;
  // сканер работает только с BK4819A; если активен SI4732 — откатываемся на VFO 0
  if (vfos[currentVfo].radio != RADIO_BK4819A)
    currentVfo = 0;
  initNumVals();
  Radio_Init();

  onRangeChanged();
  FillRect(0, SPECTRUM_Y + SPECTRUM_H, LCD_WIDTH,
           LCD_HEIGHT - (SPECTRUM_Y + SPECTRUM_H), C_BLACK);
}

#define SCAN_STEPS_PER_UPDATE 128

static void update(Mode_t *self) {
  if (spectrumReady)
    return;

  for (uint8_t i = 0; i < SCAN_STEPS_PER_UPDATE; i++) {
    Radio_TuneStep(+1);
    BK4819_SelectChip(0);
    Loot m = {
        .f = vfos[currentVfo].rxF,
        .rssi = BK4819_GetRSSI(),
        .noise = 0,
        .open = false,
    };
    SP_AddPoint(&m);
    stepCount++; // <-- считаем каждый шаг

    if (vfos[currentVfo].rxF >= range.end) {
      spectrumReady = true;
      Radio_TuneTo(range.start - StepFrequencyTable[vfos[currentVfo].step],
                   false);
      break;
    }
  }

  // пересчёт раз в секунду
  uint32_t now = millis();
  if (now - lastStepTime >= 1000) {
    stepsPerSec = stepCount * 1000 / (now - lastStepTime);
    stepCount = 0;
    lastStepTime = now;
  }
}

static uint32_t lastSpectrumRender;

static void render(Mode_t *self) {
  (void)self;
  STATUSLINE_render();
  NumVal_Render(&ctx.numVal[0]);
  NumVal_Render(&ctx.numVal[1]);

  if (spectrumReady || millis() - lastSpectrumRender >= 500) {
    SP_Render(&range, STATUS_H + 14, SPECTRUM_H);
    lastSpectrumRender = millis();

    /* Индикаторы выбора start/end — минимальные зоны 32px */
    DrawHLine(8, SPECTRUM_Y, 32, endFSel ? C_BLACK : C_WHITE);
    DrawHLine(LCD_WIDTH - 8 - 32, SPECTRUM_Y, 32, !endFSel ? C_BLACK : C_WHITE);

    /* stepsPerSec — затереть старое + нарисовать новое */
    {
      int16_t tw = TextWidth("9999/s", F_SM);
      FillRect(LCD_XCENTER - tw / 2, SPECTRUM_Y - 14, tw, 12, BG());
      PrintfT(LCD_XCENTER, SPECTRUM_Y - 2, text_style(C_WHITE, C_BLACK, POS_C, F_SM), "%4u/s", stepsPerSec);
    }
  }
  if (spectrumReady) {
    WF_Render(SPECTRUM_Y + SPECTRUM_H, true);
    spectrumReady = false;
    SP_Begin();
  }
}

static void exitMode(Mode_t *self) {
  (void)self;
  currentVfo = savedVfo;
}

static bool key(Mode_t *self, key_id_t k, key_evt_type_t state) {
  (void)self;

  if (NumVal_Key(&ctx.numVal[endFSel], k, state))
    return true;

  if (state != KEY_EVT_RELEASE)
    return false;

  switch (k) {
  case KEY_MENU:
    Mode_Switch(&MODE_VFO);
    return true;
  case KEY_EXIT:
    endFSel = !endFSel;
    return true;
  default:
    break;
  }
  return false;
}

Mode_t MODE_SCANNER = {
    .enter = enter,
    .exit = exitMode,
    .update = update,
    .render = render,
    .key = key,
    .ctx = &ctx,
};
