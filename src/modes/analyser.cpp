#include "modes.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/log.h"
#include "../helper/measurements.h"
#include "../helper/unit_fmt.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/lv_port.h"
#include "../ui/numval.h"
#include "../ui/screen_layout.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "mode.h"
#include "printf.h"
#include <stdlib.h>
#include <string.h>

/* Раскладка общая со сканером — src/ui/screen_layout.h. Ниже
 * UI_WATERFALL_Y начинается аппаратный vscroll: текста там быть не должно. */

#define CURSOR_TIMEOUT_MS 5000

/* Стрелка прослушивания: 9×8 px, остриё вниз, стоит на верхней кромке
 * спектра. Половина ширины нужна clampCursor, чтобы маркер не уезжал
 * за край экрана. */
#define LISTEN_ARROW_HALF 4
#define LISTEN_ARROW_H 8

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
  bool prevListen; /* прошлый кадр рисовал стрелку прослушивания */
  NumVal_t freqVal;  /* центр: ввод частоты (клавиша 0) */
  NumVal_t startVal; /* границы диапазона: 5 — начало, затем конец */
  NumVal_t endVal;
  bool listen;       /* «still»: скан стоит, слушаем частоту курсора */
  bool spectrumDirty; /* перерисовать спектр вне очереди (сброс пиков) */
  uint8_t savedHalfW; /* ширина рамки до режима прослушивания */
  uint32_t cursorFreq; /* точная частота курсора (шаг сетки, не пиксель) */
} anaCtx;

/* Выдержка на точке перед чтением RSSI, мкс. 0 — максимально быстрый
 * обзор; далее шаг 100 мкс до 1 мс, затем по 1 мс до 10 мс.
 * 600 мкс — прежний режим с более плотными пиками (ПЛЛ/АРУ успевают
 * установиться). Меняется 1/7, текущее значение видно в верхней строке. */
static const uint16_t ANA_DWELL_US[] = {
    0,    100,  200,  300,  400,  500,  600,  700,  800,  900,
    1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000};
#define ANA_DWELL_N (sizeof(ANA_DWELL_US) / sizeof(ANA_DWELL_US[0]))
static uint8_t s_dwellIdx = 6; /* 600 мкс, как было */

static uint32_t xToFreq(uint8_t x);

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

/* Шаг скана — шаг VFO. Экран хранит 240 столбцов с максимумом по всем
 * точкам, попавшим в столбец, так что число точек влияет только на время
 * прохода (диапазон / шаг / ~160 точек в секунду). */
static uint32_t scanStep() { return StepFrequencyTable[vfos[currentVfo].step]; }

/* Привязка частоты к сетке шага, проходящей через base. Абсолютное
 * округление (f % step) не годится для шагов вроде 8.33 кГц: у авиадиапазона
 * каналы отсчитываются от начала диапазона, а не от нуля. */
static uint32_t snapToGrid(uint32_t f, uint32_t base, uint32_t step) {
  if (!step)
    return f;
  int64_t off = (int64_t)f - (int64_t)base;
  int64_t rem = off % (int64_t)step;
  if (rem < 0)
    rem += step;
  if (rem > (int64_t)step / 2)
    off += step - rem;
  else
    off -= rem;
  int64_t v = (int64_t)base + off;
  return v < 0 ? 0 : (uint32_t)v;
}

/* BK4819 принимает 18–660 и 840–1300 МГц; остальное пропускаем */
static bool bkCovers(uint32_t f) {
  return f >= 1800000 && f <= 130000000 && !(f > 66000000 && f < 84000000);
}

static void initRange() {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  anaCtx.range.start = DEFAULT_RANGE_START;
  anaCtx.range.end = anaCtx.range.start + step * SP_MAX_POINTS;
  anaCtx.scanF = anaCtx.range.start;
}

static void exitListen(void);

/* Сетку скана задаёт range.start: с него начинается проход, все следующие
 * точки идут через шаг VFO. Поэтому границы/зум/курсор привязываем к этой
 * сетке, а не к абсолютному нулю — так работает и 8.33 кГц. Правую границу
 * держим в пределах приёмника, чтобы широкий диапазон не ушёл за BK4819_F_MAX. */
static void snapRange() {
  uint32_t step = scanStep();
  uint32_t start = anaCtx.range.start;
  uint32_t end = snapToGrid(anaCtx.range.end, start, step);
  if (end > BK4819_F_MAX) {
    uint32_t off = BK4819_F_MAX >= start ? BK4819_F_MAX - start : 0;
    end = start + (off / step) * step;
  }
  anaCtx.range.end = end;
  if (anaCtx.range.end <= anaCtx.range.start)
    anaCtx.range.end = anaCtx.range.start + step;
}

static void onRangeChanged() {
  snapRange();
  if (anaCtx.listen)
    exitListen(); /* диапазон сменился — курсор и сетка новые */
  uint32_t step = scanStep();
  SP_Init(&anaCtx.range, step, anaCtx.range.end - anaCtx.range.start);
  NumVal_SetValue(&anaCtx.startVal, anaCtx.range.start);
  NumVal_SetValue(&anaCtx.endVal, anaCtx.range.end);
  anaCtx.scanF = anaCtx.range.start;
  anaCtx.spectrumReady = false;
  anaCtx.cursorCenter = SP_MAX_POINTS / 2;
  anaCtx.cursorFreq = xToFreq(anaCtx.cursorCenter);
  anaCtx.cursorHalfW = SP_MAX_POINTS / 6;
  anaCtx.cursorActive = true;
  anaCtx.cursorTimeout = millis() + CURSOR_TIMEOUT_MS;
  anaCtx.stepCount = 0;
  anaCtx.lastStepTime = millis();
  anaCtx.stepsPerSec = 0;
  anaCtx.prevCursorActive = false;

  /* Смена диапазона/шага: сбрасываем аппаратный скролл водопада и чистим
   * всё под статусом, иначе старый водопад продолжает «ездить» и его
   * строки выглядывают поверх нового спектра. */
  WF_Reset();
  FillRect(0, UI_STATUS_H, LCD_WIDTH, UI_WATERFALL_Y - UI_STATUS_H, C_BLACK);
  WF_Clear(UI_WATERFALL_Y);
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
  uint32_t base = anaCtx.range.start;
  uint32_t newStart = base + (uint64_t)x0 * span / SP_MAX_POINTS;
  uint32_t newEnd = base + (uint64_t)x1 * span / SP_MAX_POINTS;
  newStart = snapToGrid(newStart, base, step);
  newEnd = snapToGrid(newEnd, base, step);
  if (newEnd > newStart && newEnd - newStart >= step * 10) {
    anaCtx.range.start = newStart;
    anaCtx.range.end = newEnd;
    onRangeChanged();
  }
}

static void zoomOut() {
  uint32_t step = scanStep();
  uint32_t base = anaCtx.range.start;
  int64_t span = anaCtx.range.end - anaCtx.range.start;
  int64_t newSpan = span * 2;
  int64_t newStart = (int64_t)base - span / 2;
  /* в границах приёмника; без знаковых переходов (uint32 уходил в мусор) */
  if (newSpan > (int64_t)(BK4819_F_MAX - BK4819_F_MIN)) {
    newSpan = BK4819_F_MAX - BK4819_F_MIN;
    newStart = BK4819_F_MIN;
  }
  if (newStart < (int64_t)BK4819_F_MIN)
    newStart = BK4819_F_MIN;
  if (newStart + newSpan > (int64_t)BK4819_F_MAX)
    newStart = (int64_t)BK4819_F_MAX - newSpan;
  uint32_t ns = snapToGrid((uint32_t)newStart, base, step);
  uint32_t ne = snapToGrid((uint32_t)(newStart + newSpan), base, step);
  if (ne > ns && ne - ns >= step * 10) {
    anaCtx.range.start = ns;
    anaCtx.range.end = ne;
    onRangeChanged();
  }
}

static uint32_t xToFreq(uint8_t x);

/* Режим «still» (как маркер в Mayhem): скан стоит, спектр заморожен,
 * приёмник настроен на частоту курсора. Звук открыт принудительно, как
 * пауза в сканере, — шумодав не скрывает слабый пик под стрелкой.
 * Курсор идёт по сетке шага, приёмник — следом. */
static void tuneListen(void) {
  Radio_TuneTo(anaCtx.cursorFreq, true); /* precise: мод., фильтр, шумодав */
  /* Слушаем без шумодава, как пауза в сканере: иначе слабый пик под
   * стрелкой не слышно. Для SI4732 (ниже 18 МГц) звук уже открыл applyVfo;
   * Radio_ScanAudio его бы заглушил. */
  if (vfos[currentVfo].radio != RADIO_SI4732)
    Radio_ScanAudio(true);
}

static void clampCursor(void) {
  int16_t margin = anaCtx.listen ? LISTEN_ARROW_HALF : anaCtx.cursorHalfW;
  int16_t lo = margin, hi = SP_MAX_POINTS - 1 - margin;
  if (anaCtx.cursorCenter < lo)
    anaCtx.cursorCenter = (uint8_t)lo;
  if (anaCtx.cursorCenter > hi)
    anaCtx.cursorCenter = (uint8_t)hi;
}

static void enterListen(void) {
  anaCtx.listen = true;
  anaCtx.savedHalfW = anaCtx.cursorHalfW;
  anaCtx.cursorHalfW = 2;
  anaCtx.cursorFreq = xToFreq(anaCtx.cursorCenter);
  clampCursor();
  anaCtx.cursorActive = true;
  anaCtx.cursorTimeout = 0xFFFFFFFFu; /* пока слушаем — стрелка не гаснет */
  tuneListen(); /* заодно открывает звук принудительно */
}

static void exitListen(void) {
  anaCtx.listen = false;
  anaCtx.cursorHalfW = anaCtx.savedHalfW;
  clampCursor();
  anaCtx.cursorTimeout = millis() + CURSOR_TIMEOUT_MS;
  Radio_ScanAudio(false); /* дальше скан — тишина */
  anaCtx.scanF = anaCtx.range.start; /* скан с начала диапазона */
  anaCtx.spectrumReady = false;
  SP_Begin();
}

/* Сдвиг рамки на cols колонок экрана (1 колонка = span/240). */
static void moveCursorCols(int16_t cols) {
  int16_t nc = (int16_t)anaCtx.cursorCenter + cols;
  anaCtx.cursorCenter = (uint8_t)(nc < 0 ? 0 : nc > 255 ? 255 : nc);
  clampCursor();
  anaCtx.cursorFreq = xToFreq(anaCtx.cursorCenter);
  anaCtx.cursorActive = true;
  anaCtx.cursorTimeout = millis() + CURSOR_TIMEOUT_MS;
}

/* В прослушивании курсор идёт по сетке скана, а не по пикселям: частота
 * меняется ровно на steps шагов VFO. Стрелка на экране — ближайший пиксель
 * (на широком диапазоне шаг меньше пикселя, двигается только частота). */
static void moveCursorSteps(int32_t steps) {
  uint32_t st = scanStep();
  uint32_t lo = anaCtx.range.start;
  uint32_t hi = anaCtx.range.end > st ? anaCtx.range.end - st : lo;
  int64_t f = (int64_t)anaCtx.cursorFreq + (int64_t)steps * (int64_t)st;
  if (f < (int64_t)lo)
    f = lo;
  if (f > (int64_t)hi)
    f = hi;
  anaCtx.cursorFreq = snapToGrid((uint32_t)f, lo, st);
  uint32_t span = anaCtx.range.end - lo;
  uint32_t x = span ? (uint32_t)((uint64_t)(anaCtx.cursorFreq - lo) *
                                 SP_MAX_POINTS / span)
                    : 0;
  anaCtx.cursorCenter = (uint8_t)(x > SP_MAX_POINTS - 1 ? SP_MAX_POINTS - 1 : x);
  clampCursor();
  anaCtx.cursorActive = true;
  anaCtx.cursorTimeout = anaCtx.listen ? 0xFFFFFFFFu : millis() + CURSOR_TIMEOUT_MS;
  if (anaCtx.listen)
    tuneListen();
}

/* Зум (in/out) в режиме прослушивания: диапазон меняется (onRangeChanged
 * выходит из прослушивания), потом возвращаемся в него на ТУ ЖЕ частоту. */
static void zoomKeepListen(bool in) {
  bool was = anaCtx.listen;
  uint32_t f0 = was ? anaCtx.cursorFreq : xToFreq(anaCtx.cursorCenter);
  if (in)
    zoomIn();
  else
    zoomOut();
  if (was && !anaCtx.listen) {
    uint32_t step = scanStep();
    uint32_t span = anaCtx.range.end - anaCtx.range.start;
    if (f0 < anaCtx.range.start)
      f0 = anaCtx.range.start;
    else if (f0 + step > anaCtx.range.end)
      f0 = anaCtx.range.end > step ? anaCtx.range.end - step
                                   : anaCtx.range.start;
    f0 = snapToGrid(f0, anaCtx.range.start, step);
    int32_t x = f0 <= anaCtx.range.start
                    ? 0
                    : (int32_t)((uint64_t)(f0 - anaCtx.range.start) *
                                SP_MAX_POINTS / span);
    anaCtx.cursorCenter = (uint8_t)(x > SP_MAX_POINTS - 1 ? SP_MAX_POINTS - 1 : x);
    enterListen();
    anaCtx.cursorFreq = f0; /* enterListen мог взять частоту по пикселю */
    tuneListen();
  }
}

static uint32_t xToFreq(uint8_t x) {
  uint32_t step = scanStep();
  uint32_t base = anaCtx.range.start;
  uint32_t span = anaCtx.range.end - base;
  uint32_t f =
      snapToGrid(base + (uint64_t)x * span / SP_MAX_POINTS, base, step);
  /* range.end — граница прохода, последняя измеренная точка на шаг ниже:
   * правый край экрана тоже должен попадать в измеренную сетку. */
  if (f >= anaCtx.range.end)
    f -= step;
  return f < base ? base : f;
}

/* Границы курсора на экране: в обычном режиме это края рамки, в режиме
 * прослушивания — одна колонка под стрелкой. */
static uint8_t cursorX0() {
  return anaCtx.listen ? anaCtx.cursorCenter : getCursorStart();
}

static uint8_t cursorX1() {
  return anaCtx.listen ? anaCtx.cursorCenter : getCursorEnd();
}

static void eraseCursorZone() {
  if (!anaCtx.prevCursorActive)
    return;

  if (anaCtx.prevListen) {
    FillRect((int16_t)anaCtx.prevCursorStart - LISTEN_ARROW_HALF,
             UI_SPECTRUM_Y, LISTEN_ARROW_HALF * 2 + 1, LISTEN_ARROW_H, C_BLACK);
  } else {
    uint8_t x0 = anaCtx.prevCursorStart;
    uint8_t x1 = anaCtx.prevCursorEnd;

    DrawHLine(x0, UI_SPECTRUM_Y, x1 - x0 + 1, C_BLACK);
    DrawHLine(x0, UI_SPECTRUM_Y + UI_SPECTRUM_H - 1, x1 - x0 + 1, C_BLACK);
    DrawVLine(x0, UI_SPECTRUM_Y, UI_SPECTRUM_H, C_BLACK);
    DrawVLine(x1, UI_SPECTRUM_Y, UI_SPECTRUM_H, C_BLACK);
  }

  anaCtx.prevCursorActive = false;
}

/* Красная стрелка, остриё вниз: древко + треугольная головка. */
static void drawListenArrow(uint8_t x) {
  FillRect(x, UI_SPECTRUM_Y, 1, 3, C_RED);
  for (int8_t i = 0; i <= LISTEN_ARROW_HALF; i++) {
    int8_t w = 1 + 2 * (LISTEN_ARROW_HALF - i);
    DrawHLine((int16_t)x - (w >> 1), UI_SPECTRUM_Y + 3 + i, w, C_RED);
  }
}

static void drawCursorZone() {
  if (anaCtx.listen) {
    /* Красная стрелка на частоте курсора вместо рамки: сразу видно, где
     * слушаем. SP_Render после стирания восстанавливает строки спектра. */
    drawListenArrow(anaCtx.cursorCenter);

    anaCtx.prevCursorStart = anaCtx.cursorCenter;
    anaCtx.prevCursorEnd = anaCtx.cursorCenter;
  } else {
    uint8_t x0 = getCursorStart();
    uint8_t x1 = getCursorEnd();

    DrawHLine(x0, UI_SPECTRUM_Y, x1 - x0 + 1, C_CYAN);
    DrawHLine(x0, UI_SPECTRUM_Y + UI_SPECTRUM_H - 1, x1 - x0 + 1, C_CYAN);
    DrawVLine(x0, UI_SPECTRUM_Y, UI_SPECTRUM_H, C_CYAN);
    DrawVLine(x1, UI_SPECTRUM_Y, UI_SPECTRUM_H, C_CYAN);

    anaCtx.prevCursorStart = x0;
    anaCtx.prevCursorEnd = x1;
  }

  anaCtx.prevListen = anaCtx.listen;
  anaCtx.prevCursorActive = true;
}

/* Ввод центральной частоты: диапазон центрируется на значении (поле стоит
 * в центральной ячейке строки 1), у краёв приёмника — прижимается к границе. */
static void onFreqInput(uint32_t val) {
  if (val < BK4819_F_MIN || val > BK4819_F_MAX)
    return;
  Radio_TuneToAuto(val);
  /* план диапазона мог сменить шаг — диапазон строим уже по новому */
  uint32_t step = scanStep();
  uint32_t span = step * SP_MAX_POINTS;
  uint32_t half = span / 2;
  uint32_t start = val > half ? val - half : BK4819_F_MIN;
  uint32_t end = start + span;
  if (end > BK4819_F_MAX) {
    end = BK4819_F_MAX;
    start = end - span;
  }
  anaCtx.range.start = start;
  anaCtx.range.end = end;
  onRangeChanged();
}

/* Произвольные границы: ввод идёт последовательно — сначала начало, после
 * подтверждения сразу открывается редактор конца. Шаг остаётся текущим (чем
 * шире диапазон при малом шаге — тем дольше проход). */
static void onStartEntered(uint32_t f) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  anaCtx.range.start = f;
  if (anaCtx.range.end <= f + step) { /* конец не правее начала — раздвинуть */
    uint32_t e = f + step * SP_MAX_POINTS;
    anaCtx.range.end = e > BK4819_F_MAX ? BK4819_F_MAX : e;
  }
  onRangeChanged();
  NumVal_StartEdit(&anaCtx.endVal);
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

static void beginRangeEdit() {
  NumVal_SetValue(&anaCtx.startVal, anaCtx.range.start);
  NumVal_StartEdit(&anaCtx.startVal);
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
  uint16_t renderIntervalMs() override { return 16; } /* до 60 кадров/с */

  void enter() override {
    STATUSLINE_SetTitle("SPECTRUM");
    lv_port_set_direct_areas(true);
    /* ВАЖНО: memset обнуляет весь anaCtx, включая savedVfo — сохраняем
     * до него и пишем после (раньше выход всегда уходил в VFO 0) */
    uint8_t entryVfo = currentVfo;
    if (vfos[currentVfo].radio != RADIO_BK4819A)
      currentVfo = 0;
    /* Анализатор всегда работает трактом BK A: даже если в VFO остался
     * выбор SI4732, скан и звук должны идти с BK. */
    vfos[currentVfo].radio = RADIO_BK4819A;
    /* Ручной фильтр из VFO мог остаться от другого диапазона: сканируем в
     * AUTO, иначе на 450 МГц можно получить почти пустой спектр. */
    Radio_FilterAuto();
    SP_ResetLevel(); /* шкала BK, не наследуем от другого режима/тракта */
    memset(&anaCtx, 0, sizeof(anaCtx));
    anaCtx.savedVfo = entryVfo;
    anaCtx.scanVfo = currentVfo;
    anaCtx.savedFreq = vfos[currentVfo].rxF;
    /* WFM на 64–108 МГц — это SI4732, а BK измеряет по FM: на время скана
     * VFO в FM, иначе приёмник уходит на SI и спектр — шум чужого тракта. */
    anaCtx.savedMod = vfos[currentVfo].modulation;
    if (vfos[currentVfo].modulation == MOD_WFM)
      vfos[currentVfo].modulation = MOD_FM;
    /* Поля и строка ввода — в общей сетке (screen_layout.h): частота центра
     * стоит в строке 1, поэтому ввод не перекрывает соседние подписи. */
    NumVal_Init(&anaCtx.freqVal, LCD_XCENTER, UI_INFO1_BASE, F_M, 11, 12,
                UNIT_MHZ, vfos[currentVfo].rxF, BK4819_F_MIN, BK4819_F_MAX,
                onFreqInput, POS_C, nullptr, 0, 0);
    /* границы: слева — от левого края, справа — от правого (pos задаёт и
     * выравнивание при вводе) */
    NumVal_Init(&anaCtx.startVal, UI_LEFT_X, UI_RANGE_BASE, F_M, 11, 12,
                UNIT_MHZ, 0, BK4819_F_MIN, BK4819_F_MAX, onStartEntered, POS_L,
                nullptr, 0, 0);
    NumVal_Init(&anaCtx.endVal, UI_RIGHT_X, UI_RANGE_BASE, F_M, 11, 12,
                UNIT_MHZ, 0, BK4819_F_MIN, BK4819_F_MAX, onEndEntered, POS_R,
                nullptr, 0, 0);
    initRange();
    onRangeChanged();

    /* Скан идёт молча: при входе из VFO звук мог остаться открытым по
     * шумодаву, и тогда весь проход шипел бы. Прослушивание (SIDE1) откроет
     * звук принудительно. */
    Radio_ScanAudio(false);
  }

  void update() override {
    NumVal_Update(&anaCtx.freqVal);
    NumVal_Update(&anaCtx.startVal);
    NumVal_Update(&anaCtx.endVal);
    if (editingVal())
      return;
    if (anaCtx.listen) /* still: скан стоит, звук открыт в enterListen */
      return;
    if (anaCtx.spectrumReady)
      return;

    /* Настраиваем BK явно, а не через Radio_TuneTo: applyVfo по модуляции
     * может выбрать SI4732 (WFM), и тогда BK остаётся на старой частоте —
     * спектр и водопад пустые, хотя звук в прослушивании есть. */
    for (uint8_t i = 0; i < SCAN_STEPS_PER_UPDATE; i++) {
      const bool covered = bkCovers(anaCtx.scanF);
      if (covered) {
        vfos[currentVfo].rxF = anaCtx.scanF;
        BK4819_SelectFilterByFrequency(anaCtx.scanF);
        BK4819_SelectChip(0);
        BK4819_TuneTo(anaCtx.scanF, false);
      }
      /* Выдержка на каждой точке, в том числе вне диапазона BK: иначе
       * проход вне приёма шёл бы заметно быстрее показанного времени.
       * Без паузы RSSI читается до установления ПЛЛ/АРУ и «размазывает»
       * пики вдоль прохода (кривая горка); выдержка настраивается 1/7. */
      const uint16_t dwell = ANA_DWELL_US[s_dwellIdx];
      if (dwell)
        delay_us(dwell);
      Measurement m = {
          .f = anaCtx.scanF,
          .rssi = covered ? BK4819_GetRSSI() : (uint16_t)0, /* 0 = нет данных */
          .noise = 0,
          .open = false,
      };
      SP_AddPoint(&m);
      anaCtx.stepCount++;

      uint32_t step = scanStep();
      anaCtx.scanF += step;

      if (anaCtx.scanF >= anaCtx.range.end) {
        anaCtx.spectrumReady = true;
        /* Стоим на первом канале между проходами, чтобы первый замер
         * следующего прохода читался уже установившимся (не с края). */
        if (bkCovers(anaCtx.range.start)) {
          vfos[currentVfo].rxF = anaCtx.range.start;
          BK4819_SelectFilterByFrequency(anaCtx.range.start);
          BK4819_SelectChip(0);
          BK4819_TuneTo(anaCtx.range.start, false);
        }
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

    bool editFreq = NumVal_IsEditing(&anaCtx.freqVal);
    NumVal_Render(&anaCtx.startVal);
    NumVal_Render(&anaCtx.endVal);
    /* В обычном режиме поле ввода показывает частоту центра курсора —
     * тогда клавиша 0 открывает редактор с тем же значением, что на экране. */
    if (!editFreq)
      anaCtx.freqVal.value = xToFreq(anaCtx.cursorCenter);
    NumVal_Render(&anaCtx.freqVal);

    /* Шаг и выдержка между полями границ. При вводе поля не прячем:
     * принятая граница должна оставаться видимой рядом с редактором. */
    PrintfT(LCD_XCENTER, UI_RANGE_BASE,
            text_style(C_YELLOW, C_BLACK, POS_C, F_S), "%s %uus",
            STEP_NAMES[vfos[currentVfo].step],
            (unsigned)ANA_DWELL_US[s_dwellIdx]);

    /* Рамка курсора рисуется ПОВЕРХ спектра, а SP_Render перерисовывает
     * только изменившиеся строки. Поэтому стираем старую рамку и просим
     * полную перерисовку ДО рендера спектра — иначе нижняя часть рамки
     * оставалась чёрной, когда она пропадала по таймауту.
     * На время ввода центральной частоты рамку прячем. */
    bool want =
        anaCtx.cursorActive && millis() <= anaCtx.cursorTimeout && !editFreq;
    bool moved = anaCtx.prevCursorActive &&
                 (!want || cursorX0() != anaCtx.prevCursorStart ||
                  cursorX1() != anaCtx.prevCursorEnd);
    if (moved) {
      eraseCursorZone();
      SP_Invalidate(UI_SPECTRUM_H);
    }

    bool spectrumRedrawn = false;
    /* В прослушивании спектр заморожен — периодически перерисовывать его
     * нельзя: это лишние миллисекунды и мигание стрелки. Но при движении
     * курсора/стрелки спектр под ней всё равно нужно восстановить. */
    if (moved || anaCtx.spectrumDirty || anaCtx.spectrumReady ||
        (!anaCtx.listen && millis() - lastSpectrumRender >= 500)) {
      SP_Render(&anaCtx.range, UI_SPECTRUM_Y, UI_SPECTRUM_H);
      lastSpectrumRender = millis();
      anaCtx.spectrumDirty = false;
      spectrumRedrawn = true;
    }

    if (anaCtx.spectrumReady) {
      WF_Render(UI_WATERFALL_Y, true);
      anaCtx.spectrumReady = false;
      /* историю не сбрасываем: drawnTop должен совпадать с тем, что на
       * экране, иначе старые пики остаются «призраками». SP_AddPoint
       * перезапишет. */
      SP_Begin();
      anaCtx.scanF = anaCtx.range.start;
    }

    if (want && (spectrumRedrawn || !anaCtx.prevCursorActive))
      drawCursorZone();
    renderInfo(editFreq, want);
  }

  void exit() override {
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
    lv_port_set_direct_areas(false);
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    /* идёт ввод (центр / начало / конец) — все клавиши туда */
    if (NumVal_t *ev = editingVal()) {
      if (NumVal_Key(ev, k, state))
        return true;
      return false;
    }

    if (state == KEY_LONG_PRESSED) {
      /* длинная 5 — сброс пикового удержания; короткая 5 открывает ввод
       * границ (release после long не приходит) */
      if (k == KEY_5) {
        SP_ResetPeaks();
        anaCtx.spectrumDirty = true; /* в still спектр сам не перерисуется */
        return true;
      }
      return false;
    }

    if (state == KEY_LONG_PRESSED_CONT) {
      if (k == KEY_UP || k == KEY_DOWN) {
        int8_t d = (k == KEY_UP) ? +1 : -1;
        if (anaCtx.listen)
          moveCursorSteps(d * 4); /* быстро по сетке */
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
      anaCtx.captureF =
          (anaCtx.cursorActive && millis() <= anaCtx.cursorTimeout)
              ? (anaCtx.listen ? anaCtx.cursorFreq
                               : xToFreq(anaCtx.cursorCenter))
              : 0;
      Mode_Switch(MODE_VFO);
      return true;
    case KEY_UP:
      if (anaCtx.listen)
        moveCursorSteps(+1);
      else
        moveCursorCols(+5);
      return true;
    case KEY_DOWN:
      if (anaCtx.listen)
        moveCursorSteps(-1);
      else
        moveCursorCols(-5);
      return true;
    case KEY_4: /* быстрый сдвиг курсора влево */
      if (anaCtx.listen)
        moveCursorSteps(-8);
      else
        moveCursorCols(-8);
      return true;
    case KEY_6: /* ... и вправо */
      if (anaCtx.listen)
        moveCursorSteps(+8);
      else
        moveCursorCols(+8);
      return true;
    case KEY_5: /* ввод границ: сначала начало, затем конец */
      beginRangeEdit();
      return true;
    case KEY_1: /* выдержка на точке + */
      if (s_dwellIdx + 1 < (int)ANA_DWELL_N)
        s_dwellIdx++;
      return true;
    case KEY_7: /* выдержка на точке − */
      if (s_dwellIdx > 0)
        s_dwellIdx--;
      return true;
    case KEY_3:
    case KEY_9: /* шаг сканирования +/− (сетка перестраивается) */
      vfos[currentVfo].step =
          (Step)((k == KEY_3) ? (vfos[currentVfo].step + 1) % STEP_COUNT
                              : (vfos[currentVfo].step + STEP_COUNT - 1) %
                                    STEP_COUNT);
      onRangeChanged();
      return true;
    case KEY_2:
      zoomKeepListen(true);
      return true;
    case KEY_8:
      zoomKeepListen(false);
      return true;
    case KEY_0: /* ввод центральной частоты */
      NumVal_SetValue(&anaCtx.freqVal, xToFreq(anaCtx.cursorCenter));
      NumVal_StartEdit(&anaCtx.freqVal);
      return true;
    case KEY_SIDE1: /* still: стоп скана и прослушивание частоты курсора */
      if (anaCtx.listen)
        exitListen();
      else
        enterListen();
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
  /* Информационные строки (общая сетка screen_layout.h).
   * Строка 1 (F_M): левая граница/LISTEN | центр | правая граница.
   * Строка 2 (F_S): ширина рамки курсора | скорость скана.
   * При вводе центральной частоты боковые значения строки 1 прячем, чтобы
   * длинный ввод не наезжал на них; редактор занимает тот же слот, что и
   * обычное значение центра. */
  void renderInfo(bool editFreq, bool cursorOn) {
    uint8_t x0 = getCursorStart();
    uint8_t x1 = getCursorEnd();
    uint32_t leftF = xToFreq(x0);
    uint32_t rightF = xToFreq(x1);
    uint32_t centerF =
        anaCtx.listen ? anaCtx.cursorFreq : xToFreq(anaCtx.cursorCenter);

    Color sideCol = cursorOn ? C_CYAN : C_GRAY;
    TextStyle leftStyle =
        text_style(anaCtx.listen ? C_RED : sideCol, C_BLACK, POS_L, F_M);
    TextStyle midStyle = text_style(C_WHITE, C_BLACK, POS_C, F_M);
    TextStyle rightStyle = text_style(sideCol, C_BLACK, POS_R, F_M);

    if (editFreq) {
      PrintfT(UI_LEFT_X, UI_INFO1_BASE, leftStyle, "");
      PrintfT(UI_RIGHT_X, UI_INFO1_BASE, rightStyle, "");
    } else {
      if (anaCtx.listen)
        PrintfT(UI_LEFT_X, UI_INFO1_BASE, leftStyle, "LISTEN");
      else {
        char fbuf[16];
        fmt_mhz3(fbuf, sizeof(fbuf), leftF);
        PrintfT(UI_LEFT_X, UI_INFO1_BASE, leftStyle, "%s", fbuf);
      }
      char fbuf[16];
      fmt_mhz3(fbuf, sizeof(fbuf), centerF);
      PrintfT(LCD_XCENTER, UI_INFO1_BASE, midStyle, "%s", fbuf);
      fmt_mhz3(fbuf, sizeof(fbuf), rightF);
      PrintfT(UI_RIGHT_X, UI_INFO1_BASE, rightStyle, "%s", fbuf);
    }

    char bw[16];
    fmt_span(bw, sizeof(bw), rightF - leftF);
    PrintfT(UI_LEFT_X, UI_INFO2_BASE,
            text_style(C_GRAY, C_BLACK, POS_L, F_S), "BW %s", bw);
    char span[16];
    fmt_span(span, sizeof(span), anaCtx.range.end - anaCtx.range.start);
    PrintfT(LCD_XCENTER, UI_INFO2_BASE,
            text_style(C_GRAY, C_BLACK, POS_C, F_S), "%s", span);
    PrintfT(UI_RIGHT_X, UI_INFO2_BASE,
            text_style(C_GRAY, C_BLACK, POS_R, F_S), "%lu/s",
            (unsigned long)anaCtx.stepsPerSec);
  }

  uint32_t lastSpectrumRender = 0;
};

AnalyserMode analyserModeInstance;
Mode &MODE_ANALYSER = analyserModeInstance;
