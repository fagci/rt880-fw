#include "analyser.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/log.h"
#include "../helper/measurements.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/numval.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "mode.h"
#include "vfo.h"
#include "printf.h"
#include <stdlib.h>
#include <string.h>

#define STATUS_H 24
#define SP_H 54
#define SPECTRUM_Y (STATUS_H + 14)
#define CURSOR_Y (SPECTRUM_Y + SP_H + 1)
#define CURSOR_H 11
#define WF_Y (CURSOR_Y + CURSOR_H)

#define CURSOR_TIMEOUT_MS 5000

#define DEFAULT_RANGE_START 43300000U

struct {
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
  NumVal_t freqVal;
} anaCtx;

static uint8_t getCursorStart() {
  if (anaCtx.cursorCenter >= anaCtx.cursorHalfW)
    return anaCtx.cursorCenter - anaCtx.cursorHalfW;
  return 0;
}

static uint8_t getCursorEnd() {
  uint8_t e = anaCtx.cursorCenter + anaCtx.cursorHalfW;
  if (e >= SP_MAX_POINTS)
    e = SP_MAX_POINTS - 1;
  return e;
}

static void initRange() {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  anaCtx.range.start = DEFAULT_RANGE_START;
  anaCtx.range.end = anaCtx.range.start + step * SP_MAX_POINTS;
  anaCtx.scanF = anaCtx.range.start;
}

static void onRangeChanged() {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  SP_Init(&anaCtx.range, step, anaCtx.range.end - anaCtx.range.start);
  anaCtx.scanF = anaCtx.range.start;
  anaCtx.spectrumReady = false;
  anaCtx.cursorCenter = SP_MAX_POINTS / 2;
  anaCtx.cursorHalfW = SP_MAX_POINTS / 6;
  anaCtx.cursorActive = true;
  anaCtx.cursorTimeout = millis() + CURSOR_TIMEOUT_MS;
  anaCtx.stepCount = 0;
  anaCtx.lastStepTime = millis();
  anaCtx.stepsPerSec = 0;
  anaCtx.prevCursorActive = false;

  FillRect(0, STATUS_H, LCD_WIDTH, WF_Y - STATUS_H, C_BLACK);
}

static void shiftRange(int32_t deltaStep) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  uint32_t shift = (uint32_t)(deltaStep > 0 ? deltaStep : -deltaStep) * step;
  uint32_t span = anaCtx.range.end - anaCtx.range.start;

  if (deltaStep > 0) {
    if (anaCtx.range.start + shift > BK4819_F_MAX - span)
      return;
    anaCtx.range.start += shift;
    anaCtx.range.end += shift;
  } else {
    if (anaCtx.range.start < shift + span) {
      anaCtx.range.start = 0;
      anaCtx.range.end = span;
    } else {
      anaCtx.range.start -= shift;
      anaCtx.range.end -= shift;
    }
  }

  onRangeChanged();
}

static void zoomIn() {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  uint8_t x0 = getCursorStart();
  uint8_t x1 = getCursorEnd();
  if (x1 - x0 < 10)
    return;
  uint32_t span = anaCtx.range.end - anaCtx.range.start;
  uint32_t newStart = anaCtx.range.start + (uint32_t)x0 * span / SP_MAX_POINTS;
  uint32_t newEnd = anaCtx.range.start + (uint32_t)x1 * span / SP_MAX_POINTS;
  if (newEnd - newStart >= step * 10) {
    anaCtx.range.start = newStart;
    anaCtx.range.end = newEnd;
    onRangeChanged();
  }
}

static void zoomOut() {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  uint32_t span = anaCtx.range.end - anaCtx.range.start;
  uint32_t newSpan = span * 2;
  uint32_t newStart = anaCtx.range.start - span / 2;
  if (newStart > BK4819_F_MAX - newSpan)
    newStart = BK4819_F_MAX - newSpan;
  anaCtx.range.start = newStart;
  anaCtx.range.end = newStart + newSpan;
  if (anaCtx.range.end > BK4819_F_MAX) {
    anaCtx.range.end = BK4819_F_MAX;
    anaCtx.range.start = anaCtx.range.end - newSpan;
  }
  if ((uint32_t)(anaCtx.range.end - anaCtx.range.start) >= step * 10)
    onRangeChanged();
}

static void moveCursorZone(int8_t dir) {
  int16_t newCenter = (int16_t)anaCtx.cursorCenter + dir * 5;
  if (newCenter < (int16_t)anaCtx.cursorHalfW)
    newCenter = anaCtx.cursorHalfW;
  if (newCenter > (int16_t)(SP_MAX_POINTS - 1 - anaCtx.cursorHalfW))
    newCenter = SP_MAX_POINTS - 1 - anaCtx.cursorHalfW;
  anaCtx.cursorCenter = (uint8_t)newCenter;
  anaCtx.cursorActive = true;
  anaCtx.cursorTimeout = millis() + CURSOR_TIMEOUT_MS;
}

static void resizeCursor(int8_t dir) {
  int16_t newHalf = (int16_t)anaCtx.cursorHalfW + dir * 3;
  if (newHalf < 3)
    newHalf = 3;
  if (newHalf > SP_MAX_POINTS / 2)
    newHalf = SP_MAX_POINTS / 2;
  anaCtx.cursorHalfW = (uint8_t)newHalf;
  anaCtx.cursorActive = true;
  anaCtx.cursorTimeout = millis() + CURSOR_TIMEOUT_MS;
}

static uint32_t xToFreq(uint8_t x) {
  uint32_t span = anaCtx.range.end - anaCtx.range.start;
  return anaCtx.range.start + (uint32_t)x * span / SP_MAX_POINTS;
}

static void eraseCursorZone() {
  if (!anaCtx.prevCursorActive)
    return;

  uint8_t x0 = anaCtx.prevCursorStart;
  uint8_t x1 = anaCtx.prevCursorEnd;

  DrawHLine(x0, SPECTRUM_Y, x1 - x0 + 1, C_BLACK);
  DrawHLine(x0, SPECTRUM_Y + SP_H - 1, x1 - x0 + 1, C_BLACK);
  DrawVLine(x0, SPECTRUM_Y, SP_H, C_BLACK);
  DrawVLine(x1, SPECTRUM_Y, SP_H, C_BLACK);

  anaCtx.prevCursorActive = false;
}

static void drawCursorZone() {
  uint8_t x0 = getCursorStart();
  uint8_t x1 = getCursorEnd();

  DrawHLine(x0, SPECTRUM_Y, x1 - x0 + 1, C_CYAN);
  DrawHLine(x0, SPECTRUM_Y + SP_H - 1, x1 - x0 + 1, C_CYAN);
  DrawVLine(x0, SPECTRUM_Y, SP_H, C_CYAN);
  DrawVLine(x1, SPECTRUM_Y, SP_H, C_CYAN);

  anaCtx.prevCursorStart = x0;
  anaCtx.prevCursorEnd = x1;
  anaCtx.prevCursorActive = true;
}

static void onFreqInput(uint32_t val) {
  if (val >= BK4819_F_MIN && val <= BK4819_F_MAX) {
    Radio_TuneToAuto(val);
    uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
    uint32_t span = step * SP_MAX_POINTS;
    anaCtx.range.start = val;
    anaCtx.range.end = val + span;
    onRangeChanged();
  }
}

#define SCAN_STEPS_PER_UPDATE 128

class AnalyserMode : public Mode {
public:
  void enter() override {
    anaCtx.savedVfo = currentVfo;
    if (vfos[currentVfo].radio != RADIO_BK4819A)
      currentVfo = 0;
    memset(&anaCtx, 0, sizeof(anaCtx));
    /* ВАЖНО: ниже WF_Y экран скроллится аппаратно — поле ввода держим
     * в фиксированной зоне над водопадом (строка cursor info). */
    NumVal_Init(&anaCtx.freqVal, LCD_XCENTER, WF_Y - 1, F_SM, 11, 12,
                UNIT_MHZ, vfos[currentVfo].rxF, BK4819_F_MIN, BK4819_F_MAX,
                onFreqInput, POS_C, nullptr, 0, 0);
    initRange();
    onRangeChanged();

    FillRect(0, WF_Y, LCD_WIDTH, LCD_HEIGHT - WF_Y, C_BLACK);
  }

  void update() override {
    NumVal_Update(&anaCtx.freqVal);
    if (NumVal_IsEditing(&anaCtx.freqVal))
      return;
    if (anaCtx.spectrumReady)
      return;

    for (uint8_t i = 0; i < SCAN_STEPS_PER_UPDATE; i++) {
      Radio_TuneTo(anaCtx.scanF, false);
      BK4819_SelectChip(0);
      Loot m = {
          .f = anaCtx.scanF,
          .rssi = BK4819_GetRSSI(),
          .noise = 0,
          .open = false,
      };
      SP_AddPoint(&m);
      anaCtx.stepCount++;

      uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
      anaCtx.scanF += step;

      if (anaCtx.scanF >= anaCtx.range.end) {
        anaCtx.spectrumReady = true;
        Radio_TuneTo(anaCtx.range.start - step, false);
        break;
      }
    }

    uint32_t now = millis();
    if (now - anaCtx.lastStepTime >= 1000) {
      anaCtx.stepsPerSec = anaCtx.stepCount * 1000 / (now - anaCtx.lastStepTime);
      anaCtx.stepCount = 0;
      anaCtx.lastStepTime = now;
    }
  }

  void render() override {
    STATUSLINE_render();

    if (NumVal_IsEditing(&anaCtx.freqVal)) {
      NumVal_Render(&anaCtx.freqVal);
      return;
    }

    bool spectrumRedrawn = false;
    if (anaCtx.spectrumReady || millis() - lastSpectrumRender >= 500) {
      SP_Render(&anaCtx.range, STATUS_H + 14, SP_H);
      lastSpectrumRender = millis();
      spectrumRedrawn = true;
    }

    if (anaCtx.spectrumReady) {
      WF_Render(WF_Y, true);
      anaCtx.spectrumReady = false;
      /* историю не сбрасываем: osy должен совпадать с тем, что на экране,
       * иначе старые пики остаются «призраками». SP_AddPoint перезапишет. */
      SP_Begin();
      anaCtx.scanF = anaCtx.range.start;
    }

    if (spectrumRedrawn) {
      anaCtx.prevCursorActive = false;
    }
    eraseCursorZone();
    if (anaCtx.cursorActive && millis() <= anaCtx.cursorTimeout) {
      drawCursorZone();
    }
    renderCursorInfo();
  }

  void exit() override {
    currentVfo = anaCtx.savedVfo;
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (NumVal_IsEditing(&anaCtx.freqVal)) {
      if (NumVal_Key(&anaCtx.freqVal, k, state))
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
      Mode_Switch(MODE_VFO);
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
      vfos[currentVfo].step = (Step)((k == KEY_3)
          ? (vfos[currentVfo].step + 1) % STEP_COUNT
          : (vfos[currentVfo].step + STEP_COUNT - 1) % STEP_COUNT);
      onRangeChanged();
      return true;
    case KEY_STAR: /* ввод частоты */
      FillRect(0, CURSOR_Y, LCD_WIDTH, WF_Y - CURSOR_Y, C_BLACK);
      anaCtx.cursorActive = false;
      NumVal_StartEdit(&anaCtx.freqVal);
      return true;
    case KEY_0:
      FillRect(0, CURSOR_Y, LCD_WIDTH, WF_Y - CURSOR_Y, C_BLACK);
      anaCtx.cursorActive = false;
      NumVal_Key(&anaCtx.freqVal, KEY_0, KEY_RELEASED);
      return true;
    case KEY_EXIT:
      anaCtx.cursorActive = false;
      return true;
    default:
      break;
    }
    return false;
  }

private:
  void renderCursorInfo() {
    FillRect(0, CURSOR_Y, LCD_WIDTH, WF_Y - CURSOR_Y, C_BLACK);

    uint16_t textY = WF_Y - 1;

    if (anaCtx.cursorActive && millis() <= anaCtx.cursorTimeout) {
      uint8_t x0 = getCursorStart();
      uint8_t x1 = getCursorEnd();
      uint32_t leftF = xToFreq(x0);
      uint32_t rightF = xToFreq(x1);
      uint32_t centerF = xToFreq(anaCtx.cursorCenter);
      char buf[24];

      snprintf(buf, sizeof(buf), "%lu.%03lu", leftF / MHZ, (leftF % MHZ) / 1000);
      PrintfEx(4, textY, POS_L, C_CYAN, C_BLACK, F_SS, "%s", buf);

      snprintf(buf, sizeof(buf), "%lu.%03lu", centerF / MHZ, (centerF % MHZ) / 1000);
      PrintfEx(LCD_XCENTER, textY, POS_C, C_WHITE, C_BLACK, F_SM, "%s", buf);

      snprintf(buf, sizeof(buf), "%lu.%03lu", rightF / MHZ, (rightF % MHZ) / 1000);
      PrintfEx(LCD_WIDTH - 4, textY, POS_R, C_CYAN, C_BLACK, F_SS, "%s", buf);
    } else {
      uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
      uint32_t span = anaCtx.range.end - anaCtx.range.start;
      char buf[24];

      snprintf(buf, sizeof(buf), "Step %lu.%02luk", step / 100, step % 100);
      PrintfEx(4, textY, POS_L, C_GRAY, C_BLACK, F_SS, "%s", buf);

      snprintf(buf, sizeof(buf), "%4lu/s", anaCtx.stepsPerSec);
      PrintfEx(LCD_WIDTH - 4, textY, POS_R, C_GRAY, C_BLACK, F_SS, "%s", buf);

      snprintf(buf, sizeof(buf), "%lu MHz", span / MHZ);
      PrintfEx(LCD_XCENTER, textY, POS_C, C_GRAY, C_BLACK, F_SS, "%s", buf);

      anaCtx.cursorActive = false;
    }
  }

  uint32_t lastSpectrumRender = 0;
};

AnalyserMode analyserModeInstance;
Mode &MODE_ANALYSER = analyserModeInstance;
