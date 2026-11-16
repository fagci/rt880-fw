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
#include "lvgl.h"
#include "mode.h"
#include "vfo.h"
#include "printf.h"
#include <stdlib.h>
#include <string.h>

#define STATUS_H 24
#define SP_H 54
/* Над спектром — поля границ диапазона (лейблы F_M [y-11,y+4], y=SPECTRUM_Y-10)
 * и подчёркивание выбранной границы (SPECTRUM_Y-4..-3). Под спектром — зазор
 * до строки информации (иначе цифры перекрывали низ спектра), строка
 * информации и запас до водопада: лейблы не должны заходить в зону
 * аппаратного скролла (с WF_Y). */
#define SPECTRUM_Y (STATUS_H + 24)
#define CURSOR_Y (SPECTRUM_Y + SP_H + 6)
#define INFO_BASE (CURSOR_Y + 16) /* baseline строки информации и ввода частоты */
/* Запас от низа подписей (INFO_BASE+4) до зоны скролла — 14 строк: надписи
 * ни при каких обстоятельствах не должны попадать в память водопада */
#define WF_Y (INFO_BASE + 18)
#define CURSOR_H (WF_Y - CURSOR_Y)
#define BORDER_Y (SPECTRUM_Y - 10)

#define CURSOR_TIMEOUT_MS 5000

#define DEFAULT_RANGE_START 43300000U

struct {
  FRange range;
  uint32_t scanF;
  uint8_t savedVfo;
  uint8_t scanVfo;     /* VFO, которым сканируем (BK-A) */
  uint8_t savedMod;    /* модуляция VFO на входе (скан идёт в FM) */
  uint32_t savedFreq;  /* его частота на входе */
  uint32_t captureF;   /* MENU: частота курсора — в VFO (0 = нет) */
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
  NumVal_t freqVal;  /* центр: ввод частоты (клавиша 0) */
  NumVal_t startVal; /* границы диапазона: произвольный ввод (клавиша *) */
  NumVal_t endVal;
  bool borderSel;    /* false — начало, true — конец ('#' переключает) */
  bool listen;       /* «still»: скан стоит, слушаем частоту курсора */
  uint8_t savedHalfW; /* ширина рамки до режима прослушивания */
} anaCtx;

/* Кэш строки информации: перерисовываем только при смене текста. Раньше
 * чёрная заливка и повторная отрисовка лейблов шли каждый кадр — лишние
 * записи в зону экрана рядом с водопадом. */
static bool s_infoValid;
static uint8_t s_infoMode;
static char s_info[3][24];

/* Строка информации — три нативных LVGL-лейбла (создаются в enter, удаляются
 * в exit): слева — шаг/левая граница рамки/LISTEN, центр — диапазон/частота
 * курсора, справа — скорость/правая граница. Узкие непересекающиеся зоны. */
static lv_obj_t *s_infoLbl[3];

static inline lv_color_t lvc565(uint16_t c) {
  c = TC(c);
  return lv_color_make((uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC),
                       (uint8_t)(c << 3));
}

static lv_obj_t *mkInfoLabel(const lv_font_t *font, lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_text_align_t al) {
  lv_obj_t *l = lv_label_create(lv_scr_act());
  if (!l)
    return nullptr;
  lv_obj_set_pos(l, x, y);
  lv_obj_set_width(l, w);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_align(l, al, LV_PART_MAIN);
  lv_obj_set_style_bg_color(l, lvc565(C_BLACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(l, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(l, 0, LV_PART_MAIN);
  return l;
}

static void setInfo(uint8_t i, const char *text, uint16_t color) {
  if (!s_infoLbl[i])
    return;
  lv_obj_set_style_text_color(s_infoLbl[i], lvc565(color), LV_PART_MAIN);
  lv_label_set_text(s_infoLbl[i], text);
}

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

/* Шаг скана: шаг VFO, но не мельче span/240 — на широком диапазоне
 * (100…470 МГц при 25 кГц это 14000 точек) проход иначе длится минуты,
 * а точек всё равно больше, чем пикселей. */
static uint32_t scanStep() {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  uint32_t span = anaCtx.range.end - anaCtx.range.start;
  uint32_t min = (span + SP_MAX_POINTS - 1) / SP_MAX_POINTS;
  return step > min ? step : min;
}

static void initRange() {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  anaCtx.range.start = DEFAULT_RANGE_START;
  anaCtx.range.end = anaCtx.range.start + step * SP_MAX_POINTS;
  anaCtx.scanF = anaCtx.range.start;
}

static void exitListen(void);

static void onRangeChanged() {
  if (anaCtx.listen)
    exitListen(); /* диапазон сменился — курсор и сетка новые */
  uint32_t step = scanStep();
  SP_Init(&anaCtx.range, step, anaCtx.range.end - anaCtx.range.start);
  NumVal_SetValue(&anaCtx.startVal, anaCtx.range.start);
  NumVal_SetValue(&anaCtx.endVal, anaCtx.range.end);
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
  s_infoValid = false;

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
  if (anaCtx.listen) { /* маркер узкий — зумим в «рабочую» ширину рамки */
    int16_t c = anaCtx.cursorCenter, h = anaCtx.savedHalfW;
    x0 = (uint8_t)(c - h < 0 ? 0 : c - h);
    x1 = (uint8_t)(c + h > SP_MAX_POINTS - 1 ? SP_MAX_POINTS - 1 : c + h);
  }
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

static uint32_t xToFreq(uint8_t x);

/* Режим «still» (как маркер в Mayhem): скан стоит, спектр заморожен, приёмник
 * настроен на частоту курсора — слышно через обычный шумодав
 * (Radio_Update в update()). Курсор двигается по экрану, приёмник идёт
 * следом. Рамка на время прослушивания сужается до маркера. */
static void tuneListen(void) {
  Radio_TuneTo(xToFreq(anaCtx.cursorCenter), true); /* precise: мод., фильтр, шумодав */
}

static void clampCursor(void) {
  int16_t lo = anaCtx.cursorHalfW, hi = SP_MAX_POINTS - 1 - anaCtx.cursorHalfW;
  if (anaCtx.cursorCenter < lo)
    anaCtx.cursorCenter = (uint8_t)lo;
  if (anaCtx.cursorCenter > hi)
    anaCtx.cursorCenter = (uint8_t)hi;
}

static void enterListen(void) {
  anaCtx.listen = true;
  anaCtx.savedHalfW = anaCtx.cursorHalfW;
  anaCtx.cursorHalfW = 2;
  clampCursor();
  anaCtx.cursorActive = true;
  anaCtx.cursorTimeout = 0xFFFFFFFFu; /* пока слушаем — рамка не гаснет */
  tuneListen();
}

static void exitListen(void) {
  anaCtx.listen = false;
  anaCtx.cursorHalfW = anaCtx.savedHalfW;
  anaCtx.cursorTimeout = millis() + CURSOR_TIMEOUT_MS;
  Radio_ScanAudio(false);      /* дальше скан — тишина */
  anaCtx.scanF = anaCtx.range.start; /* скан с начала диапазона */
  anaCtx.spectrumReady = false;
  SP_Begin();
}

/* Сдвиг курсора на cols колонок экрана (1 колонка = span/240). В режиме
 * прослушивания приёмник сразу идёт на новую частоту. */
static void moveCursorCols(int16_t cols) {
  int16_t nc = (int16_t)anaCtx.cursorCenter + cols;
  anaCtx.cursorCenter = (uint8_t)(nc < 0 ? 0 : nc > 255 ? 255 : nc);
  clampCursor();
  anaCtx.cursorActive = true;
  anaCtx.cursorTimeout =
      anaCtx.listen ? 0xFFFFFFFFu : millis() + CURSOR_TIMEOUT_MS;
  if (anaCtx.listen)
    tuneListen();
}

/* Зум (in/out) в режиме прослушивания: диапазон меняется (onRangeChanged
 * выходит из прослушивания), потом возвращаемся в него на ТУ ЖЕ частоту. */
static void zoomKeepListen(bool in) {
  bool was = anaCtx.listen;
  uint32_t f0 = xToFreq(anaCtx.cursorCenter);
  if (in)
    zoomIn();
  else
    zoomOut();
  if (was && !anaCtx.listen) {
    enterListen();
    uint32_t span = anaCtx.range.end - anaCtx.range.start;
    int32_t x = f0 <= anaCtx.range.start
                    ? 0
                    : (int32_t)((uint64_t)(f0 - anaCtx.range.start) *
                                SP_MAX_POINTS / span);
    anaCtx.cursorCenter = (uint8_t)(x > SP_MAX_POINTS - 1 ? SP_MAX_POINTS - 1 : x);
    clampCursor();
    tuneListen();
  }
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

/* Произвольные границы: начало/конец задаются независимо. Шаг остаётся
 * текущим (чем шире диапазон при малом шаге — тем дольше проход). */
static void onStartEntered(uint32_t f) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  anaCtx.range.start = f;
  if (anaCtx.range.end <= f + step) { /* конец не правее начала — раздвинуть */
    uint32_t e = f + step * SP_MAX_POINTS;
    anaCtx.range.end = e > BK4819_F_MAX ? BK4819_F_MAX : e;
  }
  onRangeChanged();
}

static void onEndEntered(uint32_t f) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  if (f <= anaCtx.range.start + step) { /* конец ≤ начала — игнор, вернуть поле */
    NumVal_SetValue(&anaCtx.endVal, anaCtx.range.end);
    return;
  }
  anaCtx.range.end = f;
  onRangeChanged();
}

static NumVal_t *editingVal(void) {
  if (NumVal_IsEditing(&anaCtx.freqVal))
    return &anaCtx.freqVal;
  if (NumVal_IsEditing(&anaCtx.startVal))
    return &anaCtx.startVal;
  if (NumVal_IsEditing(&anaCtx.endVal))
    return &anaCtx.endVal;
  return nullptr;
}

#define SCAN_STEPS_PER_UPDATE 128

class AnalyserMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("SPECTRUM");
    /* ВАЖНО: memset обнуляет весь anaCtx, включая savedVfo — сохраняем
     * до него и пишем после (раньше выход всегда уходил в VFO 0) */
    uint8_t entryVfo = currentVfo;
    if (vfos[currentVfo].radio != RADIO_BK4819A)
      currentVfo = 0;
    memset(&anaCtx, 0, sizeof(anaCtx));
    anaCtx.savedVfo = entryVfo;
    anaCtx.scanVfo = currentVfo;
    anaCtx.savedFreq = vfos[currentVfo].rxF;
    /* WFM на 64–108 МГц — это SI4732, а BK измеряет по FM: на время скана
     * VFO в FM, иначе приёмник уходит на SI и спектр — шум чужого тракта. */
    anaCtx.savedMod = vfos[currentVfo].modulation;
    if (vfos[currentVfo].modulation == MOD_WFM)
      vfos[currentVfo].modulation = MOD_FM;
    /* ВАЖНО: ниже WF_Y экран скроллится аппаратно — поле ввода держим
     * в фиксированной зоне над водопадом (строка cursor info). */
    NumVal_Init(&anaCtx.freqVal, LCD_XCENTER + 5 * 11, INFO_BASE, F_M, 11, 12,
                UNIT_MHZ, vfos[currentVfo].rxF, BK4819_F_MIN, BK4819_F_MAX,
                onFreqInput, POS_R, nullptr, 0, 0);
    /* границы: слева — от левого края, справа — от правого (pos задаёт и
     * выравнивание при вводе) */
    NumVal_Init(&anaCtx.startVal, 8, BORDER_Y, F_M, 11, 12, UNIT_MHZ, 0,
                BK4819_F_MIN, BK4819_F_MAX, onStartEntered, POS_L, nullptr, 0, 0);
    NumVal_Init(&anaCtx.endVal, LCD_WIDTH - 8, BORDER_Y, F_M, 11, 12,
                UNIT_MHZ, 0, BK4819_F_MIN, BK4819_F_MAX, onEndEntered, POS_R,
                nullptr, 0, 0);
    /* строка информации: узкие непересекающиеся зоны, центр создаём первым */
    s_infoLbl[1] = mkInfoLabel(F_M, 60, INFO_BASE - 11, 120,
                               LV_TEXT_ALIGN_CENTER);
    s_infoLbl[0] = mkInfoLabel(F_S, 4, INFO_BASE - 4, 56,
                               LV_TEXT_ALIGN_LEFT);
    s_infoLbl[2] = mkInfoLabel(F_S, LCD_WIDTH - 4 - 56,
                               INFO_BASE - 4, 56, LV_TEXT_ALIGN_RIGHT);
    initRange();
    onRangeChanged();

    FillRect(0, WF_Y, LCD_WIDTH, LCD_HEIGHT - WF_Y, C_BLACK);
  }

  void update() override {
    NumVal_Update(&anaCtx.freqVal);
    NumVal_Update(&anaCtx.startVal);
    NumVal_Update(&anaCtx.endVal);
    if (editingVal())
      return;
    if (anaCtx.listen) { /* still: скан стоит, звук — по шумодаву */
      Radio_Update();
      return;
    }
    if (anaCtx.spectrumReady)
      return;

    for (uint8_t i = 0; i < SCAN_STEPS_PER_UPDATE; i++) {
      Radio_TuneTo(anaCtx.scanF, false);
      BK4819_SelectChip(0);
      Measurement m = {
          .f = anaCtx.scanF,
          .rssi = BK4819_GetRSSI(),
          .noise = 0,
          .open = false,
      };
      SP_AddPoint(&m);
      anaCtx.stepCount++;

      uint32_t step = scanStep();
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
    NumVal_Render(&anaCtx.startVal);
    NumVal_Render(&anaCtx.endVal);

    /* подчёркивание редактируемой границы — прямая заливка, LVGL про неё не
     * знает и может закрасить, поэтому каждый кадр (как в сканере) */
    {
      const int16_t uy = SPECTRUM_Y - 4, uw = 8 * FontW(F_M);
      FillRect(8, uy, uw, 2, anaCtx.borderSel ? C_BLACK : C_YELLOW);
      FillRect(LCD_WIDTH - 8 - uw, uy, uw, 2,
               anaCtx.borderSel ? C_YELLOW : C_BLACK);
    }

    if (NumVal_IsEditing(&anaCtx.freqVal)) {
      NumVal_Render(&anaCtx.freqVal);
      s_infoValid = false; /* зону информации перерисуем после ввода */
      return;
    }

    bool spectrumRedrawn = false;
    if (anaCtx.spectrumReady || millis() - lastSpectrumRender >= 500) {
      SP_Render(&anaCtx.range, SPECTRUM_Y, SP_H);
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

    /* Рамка курсора рисуется ПОВЕРХ спектра, а SP_Render теперь перерисовывает
     * только изменившиеся строки. Поэтому:
     *  - сдвинули/изменили/спрятали рамку → стираем старую и просим
     *    перерисовать весь спектр (иначе остаются «призраки» линий);
     *  - после любой перерисовки спектра рамку рисуем заново (SP_Render
     *    мог закрасить её линии). */
    bool want = anaCtx.cursorActive && millis() <= anaCtx.cursorTimeout;
    bool moved = anaCtx.prevCursorActive &&
                 (!want || getCursorStart() != anaCtx.prevCursorStart ||
                  getCursorEnd() != anaCtx.prevCursorEnd);
    if (moved) {
      eraseCursorZone();
      SP_Invalidate(SP_H);
      SP_Render(&anaCtx.range, SPECTRUM_Y, SP_H);
      lastSpectrumRender = millis();
      spectrumRedrawn = true;
    }
    if (want && (spectrumRedrawn || !anaCtx.prevCursorActive))
      drawCursorZone();
    renderCursorInfo();
  }

  void exit() override {
    for (uint8_t i = 0; i < 3; i++) {
      if (s_infoLbl[i]) {
        lv_obj_del(s_infoLbl[i]);
        s_infoLbl[i] = nullptr;
      }
    }
    /* BK и rxF остались на последней точке скана — возвращаем VFO сканера
     * его частоту и настраиваем чип (иначе шумодав читается по чужой
     * частоте, а VFO уезжает). MENU на курсоре — забрать его частоту. */
    if (anaCtx.captureF && anaCtx.scanVfo == anaCtx.savedVfo) {
      currentVfo = anaCtx.savedVfo;
      Radio_TuneToAuto(anaCtx.captureF); /* модуляция и шаг диапазона */
    } else {
      currentVfo = anaCtx.scanVfo;
      vfos[currentVfo].rxF = anaCtx.savedFreq;
      vfos[currentVfo].modulation = static_cast<ModulationType>(anaCtx.savedMod);
      Radio_TuneTo(anaCtx.savedFreq, true);
    }
    currentVfo = anaCtx.savedVfo;
    Radio_ScanAudio(false); /* precise-настройка приоткрыла звук — глушим;
                               дальше решает шумодав в VFO */
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    /* идёт ввод (центр / начало / конец) — все клавиши туда */
    if (NumVal_t *ev = editingVal()) {
      if (NumVal_Key(ev, k, state))
        return true;
      return false;
    }

    if (state == KEY_LONG_PRESSED_CONT) {
      if (k == KEY_UP || k == KEY_DOWN) {
        int8_t d = (k == KEY_UP) ? +1 : -1;
        if (anaCtx.listen)
          moveCursorCols(d * 4); /* быстро по экрану */
        else
          shiftRange(d);
        return true;
      }
      return false;
    }

    if (state != KEY_EVT_RELEASE)
      return false;

    switch (k) {
    case KEY_MENU:
      anaCtx.captureF = (anaCtx.cursorActive && millis() <= anaCtx.cursorTimeout)
                            ? xToFreq(anaCtx.cursorCenter)
                            : 0;
      Mode_Switch(MODE_VFO);
      return true;
    case KEY_UP:
      moveCursorCols(anaCtx.listen ? +1 : +5);
      return true;
    case KEY_DOWN:
      moveCursorCols(anaCtx.listen ? -1 : -5);
      return true;
    case KEY_4: /* быстрый сдвиг курсора влево */
      moveCursorCols(-8);
      return true;
    case KEY_6: /* ... и вправо */
      moveCursorCols(+8);
      return true;
    case KEY_5: /* still: стоп скана и прослушивание частоты курсора */
      if (anaCtx.listen)
        exitListen();
      else
        enterListen();
      return true;
    case KEY_1:
      if (!anaCtx.listen)
        resizeCursor(-1);
      return true;
    case KEY_7:
      if (!anaCtx.listen)
        resizeCursor(+1);
      return true;
    case KEY_2:
      zoomKeepListen(true);
      return true;
    case KEY_8:
      zoomKeepListen(false);
      return true;
    case KEY_3:
    case KEY_9:
      vfos[currentVfo].step = (Step)((k == KEY_3)
          ? (vfos[currentVfo].step + 1) % STEP_COUNT
          : (vfos[currentVfo].step + STEP_COUNT - 1) % STEP_COUNT);
      onRangeChanged();
      return true;
    case KEY_HASH: /* выбор границы: начало/конец */
      anaCtx.borderSel = !anaCtx.borderSel;
      return true;
    case KEY_STAR: /* ввод выбранной границы (подчёркнута) */
      NumVal_StartEdit(anaCtx.borderSel ? &anaCtx.endVal : &anaCtx.startVal);
      return true;
    case KEY_0: /* ввод центральной частоты */
      s_infoValid = false;
      FillRect(0, CURSOR_Y, LCD_WIDTH, WF_Y - CURSOR_Y, C_BLACK);
      anaCtx.cursorActive = false;
      NumVal_StartEdit(&anaCtx.freqVal);
      return true;
    case KEY_EXIT: /* как везде: выход в VFO на прежнюю частоту */
      anaCtx.captureF = 0;
      Mode_Switch(MODE_VFO);
      return true;
    default:
      break;
    }
    return false;
  }

private:
  void renderCursorInfo() {
    char b0[24], b1[24], b2[24];
    uint8_t mode;

    if (anaCtx.cursorActive && millis() <= anaCtx.cursorTimeout) {
      uint8_t x0 = getCursorStart();
      uint8_t x1 = getCursorEnd();
      uint32_t leftF = xToFreq(x0);
      uint32_t rightF = xToFreq(x1);
      uint32_t centerF = xToFreq(anaCtx.cursorCenter);
      mode = anaCtx.listen ? 2 : 1;

      if (anaCtx.listen)
        snprintf(b0, sizeof(b0), "LISTEN");
      else
        snprintf(b0, sizeof(b0), "%lu.%03lu", leftF / MHZ, (leftF % MHZ) / 1000);
      snprintf(b1, sizeof(b1), "%lu.%03lu", centerF / MHZ, (centerF % MHZ) / 1000);
      snprintf(b2, sizeof(b2), "%lu.%03lu", rightF / MHZ, (rightF % MHZ) / 1000);
    } else {
      uint32_t step = scanStep();
      uint32_t span = anaCtx.range.end - anaCtx.range.start;
      mode = 0;
      snprintf(b0, sizeof(b0), "Step %lu.%02luk", step / 100, step % 100);
      snprintf(b1, sizeof(b1), "%lu MHz", span / MHZ);
      snprintf(b2, sizeof(b2), "%4lu/s", anaCtx.stepsPerSec);
      anaCtx.cursorActive = false;
    }

    /* ничего не изменилось — не трогаем экран */
    if (s_infoValid && mode == s_infoMode && !strcmp(b0, s_info[0]) &&
        !strcmp(b1, s_info[1]) && !strcmp(b2, s_info[2]))
      return;

    /* Три нативных LVGL-лейбла с непрозрачным фоном и фиксированной шириной:
     * текст/цвет меняются на месте, старый текст стирается самим лейблом.
     * Раньше это был пул Printf* с разными шрифтами в разных режимах — старый
     * лейбл другого режима оставался и просвечивал поверх нового. */
    if (mode == 0) {
      setInfo(0, b0, C_GRAY);
      setInfo(1, b1, C_GRAY);
      setInfo(2, b2, C_GRAY);
    } else {
      setInfo(0, b0, mode == 2 ? C_GREEN : C_CYAN);
      setInfo(1, b1, C_WHITE);
      setInfo(2, b2, C_CYAN);
    }
    s_infoMode = mode;
    strcpy(s_info[0], b0);
    strcpy(s_info[1], b1);
    strcpy(s_info[2], b2);
    s_infoValid = true;
  }

  uint32_t lastSpectrumRender = 0;
};

AnalyserMode analyserModeInstance;
Mode &MODE_ANALYSER = analyserModeInstance;
