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
#include <string.h>

#define VFO_H 64
#define STATUS_H 24

/* Позиции внутри блока VFO */
#define VFO_LABEL_Y   17  /* 4 + 13 */
#define VFO_FREQ_Y    40  /* 4 + 13 + 2 + 23 */
#define VFO_STEP_Y    (VFO_H - 16)
#define VFO_RSSI_Y    (VFO_H - 8)
#define VFO_RSSI_W    (LCD_WIDTH - 16)

/* Ширина текстовых элементов для затирания */
#define VFO_LABEL_W   30  /* "VFO A" ~ 24px */
#define MOD_NAME_W    35  /* "%3s" = 3×9 = 27px + 8px запас */
#define STEP_W        55  /* "%5s" = 5×9 = 45px + 10px запас */

typedef struct {
  NumVal_t numVal[DEVICE_VFO_COUNT];
  /* Кэшированные значения для детекции изменений */
  uint8_t  prevActive;
  uint8_t  prevMod[DEVICE_VFO_COUNT];
  uint8_t  prevStep;
  uint8_t  prevRssi[DEVICE_VFO_COUNT]; /* по одному на VFO */
  bool     inited;
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
    NumVal_Init(&ctx.numVal[i], 8, sy + VFO_FREQ_Y, F_MONO_LG, 21, 23,
                UNIT_MHZ, vfos[i].rxF, 0, 1340 * MHZ, onFreqEntered, POS_L);
  }
}

static void renderVfoLabel(uint8_t i, uint16_t sy, bool active) {
  Color clr = active ? C_GREEN : C_GRAY;
  TextStyle st = text_style(clr, C_BLACK, POS_L, F_SM);

  /* "VFO A" */
  FillRect(8, sy + VFO_LABEL_Y - 10, VFO_LABEL_W, 12, BG());
  PrintfT(8, sy + VFO_LABEL_Y, st, "VFO %c", 'A' + i);

  /* Модуляция справа — %3s чтобы зона всегда 3 символа */
  FillRect(LCD_WIDTH - 8 - MOD_NAME_W, sy + VFO_LABEL_Y - 10, MOD_NAME_W, 12, BG());
  PrintfT(LCD_WIDTH - 8, sy + VFO_LABEL_Y, text_style(clr, C_BLACK, POS_R, F_SM), "%3s",
          MOD_NAMES[vfos[i].modulation]);
}

static void renderVfoStep(uint8_t i, uint16_t sy) {
  /* StepFrequencyTable в десятках Гц: /100 = кГц.
     2→0, 5→0, 50→0, 100→1, 250→2, 500→5, 625→6, 833→8,
     900→9, 1000→10, 1250→12, 2500→25, 5000→50, 10000→100, 50000→500 */
  uint16_t stepKhz = StepFrequencyTable[vfos[i].step] / 100;
  FillRect(LCD_WIDTH - 8 - STEP_W, sy + VFO_STEP_Y - 10, STEP_W, 12, BG());
  PrintfT(LCD_WIDTH - 8, sy + VFO_STEP_Y, text_style(C_YELLOW, C_BLACK, POS_R, F_SM),
          "%2uk", stepKhz);
}

static void renderVfoRssi(uint8_t i, uint16_t sy, uint8_t w) {
  /* Рисуем зелёную часть ПЕРВОЙ, потом фон хвостом — чтобы глаз видел
     заполнение без мерцания (зелёный DMA идёт до фона). */
  if (w)
    FillRect(8, sy + VFO_RSSI_Y, w, 8, C_GREEN);
  FillRect(8 + w, sy + VFO_RSSI_Y, VFO_RSSI_W - w, 8, BG());
}

static void renderVfo(uint8_t i) {
  uint16_t sy = STATUS_H + i * VFO_H;
  bool active = (i == currentVfo);
  bool wasActive = (ctx.prevActive == i);
  bool firstRender = !ctx.inited;

  /* Label + modulation — перерисовать если изменился active, mod или первый проход */
  if (firstRender || active != wasActive || vfos[i].modulation != ctx.prevMod[i]) {
    renderVfoLabel(i, sy, active);
  }

  NumVal_Render(&ctx.numVal[i]);

  /* Шаг — только активный VFO */
  if (active) {
    if (firstRender || vfos[i].step != ctx.prevStep || !wasActive) {
      renderVfoStep(i, sy);
    }
  } else {
    /* Если VFO стал неактивным — затереть шаг */
    if (wasActive) {
      FillRect(LCD_WIDTH - 8 - STEP_W, sy + VFO_STEP_Y - 10, STEP_W, 12, BG());
    }
  }

  /* RSSI — только BK4819 */
  if (vfos[i].radio != RADIO_SI4732) {
    uint8_t w = 0;
    if (active) {
      w = ConvertDomain(BK4819_GetRSSI(), 0, 255, 0, VFO_RSSI_W);
    }
    if (firstRender || w != ctx.prevRssi[i] || active != wasActive) {
      renderVfoRssi(i, sy, w);
      ctx.prevRssi[i] = w;
    }
  }
}

/* ── Mode callbacks ──────────────────────────────────────────── */

static void enter(Mode_t *self) {
  (void)self;
  UI_ClearScreen(C_BLACK);
  memset(&ctx, 0, sizeof(ctx));
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

static void renderFilterList(void) {
  uint16_t baseY = VFO_H * DEVICE_VFO_COUNT + STATUS_H;
  for (uint8_t i = 0; i < 4; i++) {
    uint16_t y = baseY + i * 14;
    char marker = filterIndex == i ? '>' : ' ';
    FillRect(2, y - 10, 14, 12, BG());
    PrintfT(2, y, text_style(C_GREEN, C_BLACK, POS_L, F_SM),
            "%c %s", marker, FILTER_NAMES[i]);
  }
}

static void render(Mode_t *self) {
  (void)self;
  STATUSLINE_render();

  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++)
    renderVfo(i);

  /* Обновляем кэш после рендера */
  ctx.prevActive    = currentVfo;
  ctx.prevStep      = vfos[currentVfo].step;
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++)
    ctx.prevMod[i] = vfos[i].modulation;
  ctx.inited = true;

  if (gFInputActive)
    FINPUT_render();

  /* Список фильтров — рендерим всегда (дешёво, 4 строки) */
  renderFilterList();

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
    case KEY_SIDE2:
      gInverted = !gInverted;
      gRedrawScreen = true;
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
