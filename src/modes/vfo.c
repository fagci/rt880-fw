#include "vfo.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/log.h"
#include "../radio.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/numval.h"
#include "../ui/statusline.h"
#include <stdlib.h>

#define VFO_H 64
#define STATUS_H 24

typedef struct {
  NumVal_t numVal[DEVICE_VFO_COUNT];
} MainCtx_t;

static MainCtx_t ctx;

// Колбэк от NumVal — пользователь ввёл новую частоту
static void onFreqEntered(uint32_t f) {
  Radio_TuneTo(f, true);
  Log("tune vfo=%u f=%lu", currentVfo, f);
  Log_Render(0, LCD_HEIGHT - 8 * 6, 6);
}

static void initNumVals(void) {
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    uint16_t sy = STATUS_H + i * VFO_H;
    NumVal_Init(&ctx.numVal[i], 8, sy + 4 + 13 + 2 + 23, F_MONO_LG, 21, 23,
                UNIT_MHZ, vfos[i].rxF, 0, 1340 * MHZ, onFreqEntered);
  }
}

static void renderVfo(uint8_t i) {
  uint16_t sy = STATUS_H + i * VFO_H;
  DrawRect(0, sy, LCD_WIDTH, VFO_H, currentVfo == i ? C_GRAY : C_BLACK);
  PrintfEx(8, sy + 4 + 13, POS_L, C_GRAY, C_BLACK, F_SM, "VFO%u", i + 1);
  NumVal_Render(&ctx.numVal[i]);
}

// ── Mode callbacks ──────────────────────────────

static void enter(Mode_t *self) {
  (void)self;
  initNumVals();
  Radio_Init();
}

static void update(Mode_t *self) {
  (void)self;
  // синхронизируем NumVal если частота изменилась извне
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    if (vfos[i].rxF != NumVal_GetValue(&ctx.numVal[i])) {
      NumVal_SetValue(&ctx.numVal[i], vfos[i].rxF);
    }
    NumVal_Update(&ctx.numVal[i]);
  }
}

static void render(Mode_t *self) {
  (void)self;
  STATUSLINE_render();

  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++)
    renderVfo(i);

  if (gFInputActive)
    FINPUT_render();

  for (uint8_t i = 0; i < 4; i++) {
    PrintfEx(2, VFO_H * 3 + STATUS_H + i * 14, POS_L, C_GREEN, C_BLACK, F_SM,
             "%c %s", filterIndex == i ? '>' : ' ', FILTER_NAMES[i]);
  }
}

static bool key(Mode_t *self, key_id_t k, key_evt_type_t state) {
  (void)self;

  if (NumVal_Key(&ctx.numVal[currentVfo], k, state))
    return true;

  if (state != KEY_EVT_RELEASE)
    return false;

  switch (k) {
  case KEY_EXIT:
    Radio_NextVfo();
    // реинициализируем NumVal нового VFO с актуальной частотой
    initNumVals();
    return true;
  case KEY_UP:
    Radio_TuneStep(+1);
    return true;
  case KEY_DOWN:
    Radio_TuneStep(-1);
    return true;
  case KEY_SIDE1:
    Radio_NextFilter();
    return true;
  case KEY_SIDE2:
    Radio_ToggleFilter();
    return true;
  default:
    break;
  }
  return false;
}

Mode_t MODE_VFO = {
    .enter = enter,
    .exit = NULL,
    .update = update,
    .render = render,
    .key = key,
    .ctx = &ctx,
};
