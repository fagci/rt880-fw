#include "vfo.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/log.h"
#include "../helper/measurements.h"
#include "../radio.h"
#include "../ui/finput.h"
#include "../ui/graphics.h"
#include "../ui/numval.h"
#include "../ui/statusline.h"
#include "scanner.h"
#include <stdlib.h>

#define VFO_H 64
#define STATUS_H 24

typedef struct {
  NumVal_t numVal[DEVICE_VFO_COUNT];
} MainCtx_t;

static MainCtx_t ctx;
static bool secondReceiver;

static const char *MOD_NAMES[] = {
    [MOD_FM] = "FM",   [MOD_AM] = "AM",   [MOD_LSB] = "LSB", [MOD_USB] = "USB",
    [MOD_BYP] = "BYP", [MOD_RAW] = "RAW", [MOD_WFM] = "WFM",
};

/* Колбэк от NumVal — пользователь ввёл частоту вручную */
static void onFreqEntered(uint32_t f) {
  Radio_TuneToAuto(f);
  Log("tune vfo=%u f=%lu mod=%u", currentVfo, f, vfos[currentVfo].modulation);
}

static void initNumVals(void) {
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    uint16_t sy = STATUS_H + i * VFO_H;
    NumVal_Init(&ctx.numVal[i], 8, sy + 4 + 13 + 2 + 23, F_MONO_LG, 21, 23,
                UNIT_MHZ, vfos[i].rxF, 0, 1340 * MHZ, onFreqEntered, POS_L);
  }
}

static void renderVfo(uint8_t i) {
  uint16_t sy = STATUS_H + i * VFO_H;
  bool active = (i == currentVfo);
  uint16_t clr = active ? C_GREEN : C_GRAY;

  /* "VFO A" слева, модуляция справа */
  PrintfEx(8, sy + 4 + 13, POS_L, clr, C_BLACK, F_SM, "VFO %c", 'A' + i);
  PrintfEx(LCD_WIDTH - 8, sy + 4 + 13, POS_R, clr, C_BLACK, F_SM, "%s",
           MOD_NAMES[vfos[i].modulation]);

  NumVal_Render(&ctx.numVal[i]);

  /* Шаг — только для активного VFO, в правом углу нижней строки блока */
  if (active) {
    PrintfEx(LCD_WIDTH - 8, sy + VFO_H - 16, POS_R, C_YELLOW, C_BLACK, F_SM,
             "%5u", StepFrequencyTable[vfos[i].step]);
  }

  /* Полоска RSSI (только BK4819) */
  if (vfos[i].radio != RADIO_SI4732) {
    uint8_t w = 0;
    if (active) {
      w = ConvertDomain(BK4819_GetRSSI(), 0, 255, 0, LCD_WIDTH - 16);
    }
    FillRect(8, sy + VFO_H - 8, w, 8, C_GREEN);
    FillRect(8 + w, sy + VFO_H - 8, LCD_WIDTH - 16, 8, C_BLACK);
  }
}

/* ── Mode callbacks ──────────────────────────────────────────── */

static void enter(Mode_t *self) {
  (void)self;
  UI_ClearScreen(C_BLACK);
  initNumVals();
  Radio_Init();
}

static void update(Mode_t *self) {
  (void)self;
  Radio_Update();

  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    if (vfos[i].rxF != NumVal_GetValue(&ctx.numVal[i]))
      NumVal_SetValue(&ctx.numVal[i], vfos[i].rxF);
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
  Log_Render(0, LCD_HEIGHT - 8 * 6, 6);
}

static bool key(Mode_t *self, key_id_t k, key_evt_type_t state) {
  (void)self;

  if (NumVal_Key(&ctx.numVal[currentVfo], k, state))
    return true;

  /* Долгие нажатия */
  if (state == KEY_EVT_LONG_PRESS) {
    switch (k) {
    case KEY_UP:
      Radio_NextStep();
      return true;
    case KEY_DOWN:
      Radio_PrevStep();
      return true;
    case KEY_6:
      vfos[currentVfo].modulation =
          (vfos[currentVfo].modulation + 1) % (MOD_WFM + 1);
      Radio_TuneStep(+1);
      Radio_TuneStep(-1);
      return true;
    default:
      break;
    }
  }

  if (state != KEY_EVT_RELEASE)
    return false;

  switch (k) {
  case KEY_MENU:
    Mode_Switch(&MODE_SCANNER);
    return true;
  case KEY_EXIT:
    Radio_NextVfo();
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
  case KEY_ALARM:
    secondReceiver = !secondReceiver;
    BK4819_SelectB(secondReceiver);
    Log("BK: %c", secondReceiver ? 'B' : 'A');
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
