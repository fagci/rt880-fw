#include "scanner.h"
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

static void onRangeChanged() {
  SP_Init(&range, StepFrequencyTable[vfos[currentVfo].step],
          range.end - range.start);
  Radio_TuneTo(range.start - StepFrequencyTable[vfos[currentVfo].step], true);
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
  NumVal_Init(&ctx.numVal[0], 8, STATUS_H + 2 + 12, F_SM, 11, 12, UNIT_MHZ,
              range.start, 0, 1340 * MHZ, onStartEntered);
  NumVal_Init(&ctx.numVal[0], LCD_XCENTER, STATUS_H + 2 + 12, F_SM, 11, 12,
              UNIT_MHZ, range.end, 0, 1340 * MHZ, onEndEntered);
}

// ── Mode callbacks ──────────────────────────────

static void enter(Mode_t *self) {
  (void)self;
  UI_ClearScreen(C_BLACK);
  initNumVals();
  Radio_Init();

  onRangeChanged();
}

static void update(Mode_t *self) {
  (void)self;
  // синхронизируем NumVal если частота изменилась извне
  for (uint8_t i = 0; i < 2; i++) {
    NumVal_Update(&ctx.numVal[i]);
  }

  if (spectrumReady)
    return;

  Radio_TuneStep(+1);
  delay_us(2500);
  Loot m = {
      .f = vfos[currentVfo].rxF,
      .rssi = BK4819_GetRSSI(),
  };
  SP_AddPoint(&m);

  if (vfos[currentVfo].rxF >= range.end) {
    spectrumReady = true;
  }
}

static uint32_t lastSpectrumRender;

static void render(Mode_t *self) {
  (void)self;
  STATUSLINE_render();
  NumVal_Render(&ctx.numVal[0]);
  NumVal_Render(&ctx.numVal[1]);

  PrintfEx(LCD_WIDTH, STATUS_H + 12, POS_R, C_WHITE, C_BLACK, F_SM, "%s",
           (const char *[]){"Start", "End"}[endFSel]);

  if (spectrumReady || millis() - lastSpectrumRender >= 500) {
    SP_Render(&range, STATUS_H + 12, 64);
    lastSpectrumRender = millis();
  }
  if (spectrumReady) {
    WF_Render(STATUS_H + 12 + 64, true);
    spectrumReady = false;
  }
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
    .exit = NULL,
    .update = update,
    .render = render,
    .key = key,
    .ctx = &ctx,
};
