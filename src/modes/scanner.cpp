/* Сканер — алгоритм заимствован из s0v4 (src/helper/scan.c):
 *
 * - адаптивный порог шумодава (floorRssi) тянется к noise floor;
 * - открытие подтверждается повторным замером через SQL_DELAY («thinking»),
 *   ложные срабатывания поднимают порог;
 * - HOLD: стоим на открытой частоте и слушаем (AF DAC вкл), пока шумодав
 *   не закроется или не истечёт LISTEN_TIMEOUT_MS;
 * - закрытая частота — сразу дальше; каждые 64 закрытых шага порог
 *   дрейфует вниз;
 * - частоты-кратные 1.3 МГц (гармоники ПЧ) пропускаются;
 * - каждый замер попадает в спектр (SP_AddPoint), водопад — по строке.
 *
 * Только BK4819: используется VFO на BK (при необходимости временно
 * переключаемся на него, восстанавливаем на выходе).
 *
 * Клавиши: 0-9 — ввод границы (фокус — '#' короткий), MENU — захватить
 * частоту в VFO и выйти, EXIT — выход, '#' длинный — пауза/продолжить
 * (в паузе UP/DOWN — ручная перестройка), '*' — ввод границы, SIDE1 — водопад вкл/выкл,
 * UP/DOWN — выдержка на частоте (скорость сканирования).
 */
#include "scanner.h"
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
#include "printf.h"
#include "vfo.h"
#include <stdlib.h>
#include <string.h>

/* Раскладка экрана. ВАЖНО: водопад использует hardware vscroll зоны
 * [WF_Y, HEIGHT) — ниже этой линии нельзя размещать никакой текст/виджеты,
 * они будут «плыть» вместе с водопадом. */
#define STATUS_H 24
/* поля частот — лейблы F_SM [y-11, y+4] с y = SPECTRUM_Y-6: не перекрывают
 * спектр и статус-строку */
#define SPECTRUM_Y (STATUS_H + 20)
#define SP_H 52
#define STATE_Y (SPECTRUM_Y + SP_H + 10) /* строка состояния (с запасом от спектра) */
/* LVGL-лейбл F_SM занимает [y-11, y+4]: строка состояния (baseline
 * STATE_Y+12) кончается на STATE_Y+16. Водопад начинаем ниже — иначе
 * перерисовка лейбла (чёрный фон) пишет в память зоны скролла. */
#define WF_Y (STATE_Y + 18)             /* водопад отсюда и до низа */

#define LISTEN_TIMEOUT_MS 5000
#define SQL_DELAY_MS 30
#define SQL_MARGIN 10 /* ~5 дБ над шумом (RSSI: 2 ед. = 1 дБ) */
#define SQL_HYST 4
#define BATCH_STEPS 32 /* шагов за один проход главного цикла */

static FRange range = {.start = 400 * MHZ, .end = 470 * MHZ};

static NumVal_t numVal[2];
static bool endFSel;
static uint8_t savedVfo;
static uint32_t savedFreq; /* частота сканирующего VFO на входе */

/* скорость сканирования: выдержка на частоте до замера RSSI */
/* На 8 МГц накладные расходы SPI/GPIO давали эффективную паузу в сотни мкс;
 * на 150 МГц её нет — RSSI после перестройки не успевает устояться.
 * По умолчанию 1000 мкс, UP/DOWN меняет (отображается в центре над спектром). */
static const uint16_t DWELL_US[] = {250, 500, 1000, 2000, 4000};
#define DWELL_N (sizeof(DWELL_US) / sizeof(DWELL_US[0]))
static uint8_t dwellIdx = 2;

static uint32_t stepCount;
static uint32_t lastCpsTime;
static uint32_t stepsPerSec;

/* состояние сканирования */
static uint32_t curF;        /* текущая позиция */
static uint16_t floorRssi;   /* noise floor (сырой RSSI) */
static bool sweepDone;       /* проход завершён — рисуем строку водопада */
static bool isListening;     /* HOLD: стоим и слушаем */
static bool scanning = true; /* false = пауза (ручная перестройка) */
static bool waterfall = true;
static bool dacOn;
static uint32_t listenUntil;
static uint8_t closedSteps;
static uint32_t lastSpRender;
static uint32_t lastWfRender;

static void onRangeChanged() {
  WF_Reset();
  /* чистим всё под статусом: мусор прошлого экрана и старый водопад */
  FillRect(0, STATUS_H, LCD_WIDTH, LCD_HEIGHT - STATUS_H, C_BLACK);
  SP_Init(&range, StepFrequencyTable[vfos[currentVfo].step],
          range.end - range.start);
  curF = range.start;
  floorRssi = 0;
  sweepDone = false;
  isListening = false;
  closedSteps = 0;
  NumVal_Invalidate(&numVal[0]);
  NumVal_Invalidate(&numVal[1]);
  stepCount = 0;
  lastCpsTime = millis();
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

static void initNumVals() {
  /* NumVal печатает от ПРАВОГО края x: левому полю задаём правый край */
  NumVal_Init(&numVal[0], 8 + 9 * 11, SPECTRUM_Y - 6, F_SM, 11, 12, UNIT_MHZ,
              range.start, 0, 1340 * MHZ, onStartEntered, POS_R, nullptr, 0,
              0);
  NumVal_Init(&numVal[1], LCD_WIDTH - 8, SPECTRUM_Y - 6, F_SM, 11, 12,
              UNIT_MHZ, range.end, 0, 1340 * MHZ, onEndEntered, POS_R, nullptr,
              0, 0);
}

static void setAudio(bool on) {
  if (on != dacOn) {
    dacOn = on;
    Radio_ScanAudio(on); /* через applyMute — общий учёт DAC/AF_MUTE */
  }
}

static void advance() {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  curF += step;
  if (curF > range.end) {
    curF = range.start;
    sweepDone = true;
  }
  vfos[currentVfo].rxF = curF;
}

static void tuneHere() {
  vfos[currentVfo].rxF = curF;
  /* фильтр диапазона (VHF/UHF/...) — без этого остаётся от VFO и сигнал
   * вне его диапазона ослаблен; внутри — кэш по band, почти бесплатно */
  BK4819_SelectFilterByFrequency(curF);
  BK4819_TuneTo(curF, false);
}

/* Один шаг сканирования: замер, решение, движение.
 * Порог = noise floor (EMA по закрытым замерам) + SQL_MARGIN.
 * Раньше порог брался из Std() и был в разы ниже RSSI — всё считалось
 * «открытым» и сканер висел в HOLD/паузах. */
static void scanStep() {
  tuneHere();
  delay_us(DWELL_US[dwellIdx]);
  uint16_t rssi = BK4819_GetRSSI();

  if (!floorRssi)
    floorRssi = rssi;
  uint16_t thr = floorRssi + SQL_MARGIN;

  /* гистерезис: уже открытый канал закрываем чуть ниже порога */
  bool open = isListening ? (rssi + SQL_HYST >= thr) : (rssi >= thr);
  if (curF % 1300000 == 0)
    open = false; /* гармоники ПЧ — мусор */

  if (open && !isListening) {
    /* подтверждение: повторный замер через паузу */
    delay_ms(SQL_DELAY_MS);
    open = BK4819_GetRSSI() >= thr;
    if (!open && floorRssi < 0xFFFF - SQL_MARGIN)
      floorRssi++; /* ложное срабатывание — порог выше */
  }

  if (!open)
    floorRssi = (uint16_t)(((uint32_t)floorRssi * 15 + rssi + 8) / 16);

  Loot m = {.f = curF, .rssi = rssi, .noise = 0, .open = open};
  SP_AddPoint(&m);
  stepCount++;

  if (open) {
    if (!isListening) {
      isListening = true;
      listenUntil = millis() + LISTEN_TIMEOUT_MS;
    }
    setAudio(true);
    if (millis() > listenUntil) {
      isListening = false;
      setAudio(false);
      advance();
    }
  } else {
    if (isListening) {
      isListening = false;
      setAudio(false);
    }
    advance();
  }
}

class ScannerMode : public Mode {
public:
  void enter() override {
    savedVfo = currentVfo;
    savedFreq = vfos[currentVfo].rxF;
    Radio_Init();
    if (vfos[currentVfo].radio != RADIO_BK4819A &&
        vfos[currentVfo].radio != RADIO_BK4819B)
      currentVfo = 0;
    savedFreq = vfos[currentVfo].rxF; /* после Radio_Init */

    initNumVals();
    onRangeChanged();
    BK4819_SelectChip(vfos[currentVfo].radio == RADIO_BK4819B ? 1 : 0);
    dacOn = false;
    Radio_ScanAudio(false);
  }

  void update() override {
    NumVal_Update(&numVal[0]);
    NumVal_Update(&numVal[1]);
    bool editing = NumVal_IsEditing(&numVal[0]) || NumVal_IsEditing(&numVal[1]);
    if (scanning && !editing) {
      /* пачка шагов — иначе один шаг на итерацию главного цикла (LVGL,
       * рендер) даёт единицы-десятки шагов/с */
      for (uint8_t i = 0; i < BATCH_STEPS; i++) {
        scanStep();
        if (isListening || sweepDone)
          break;
      }
    }

    uint32_t now = millis();
    if (now - lastCpsTime >= 1000) {
      stepsPerSec = stepCount * 1000 / (now - lastCpsTime);
      stepCount = 0;
      lastCpsTime = now;
    }
  }

  void render() override {
    STATUSLINE_render();
    NumVal_Render(&numVal[0]);
    NumVal_Render(&numVal[1]);

    uint32_t now = millis();
    if (now - lastSpRender >= 300) {
      SP_Render(&range, SPECTRUM_Y, SP_H);
      lastSpRender = now;
    }
    if (sweepDone) { /* строка водопада — раз за полный проход */
      sweepDone = false;
      if (waterfall)
        WF_Render(WF_Y, true);
    }

    /* выдержка на частоте — между полями границ (мелким шрифтом) */
    PrintfT(LCD_XCENTER, SPECTRUM_Y - 6,
            text_style(C_GRAY, C_BLACK, POS_C, F_SS), "%uus",
            (unsigned)DWELL_US[dwellIdx]);

    /* строка состояния: SCAN/HOLD/PAUSE + частота + скорость */
    char state[6];
    strcpy(state, !scanning ? "PAUSE" : (isListening ? "HOLD" : "SCAN"));
    PrintfT(4, STATE_Y + 12, text_style(C_GREEN, C_BLACK, POS_L, F_SM),
            "%s %u.%05u", state, curF / 100000, curF % 100000);
    PrintfT(LCD_WIDTH - 4, STATE_Y + 12,
            text_style(C_YELLOW, C_BLACK, POS_R, F_SM), "%4u/s", stepsPerSec);
  }

  void exit() override {
    setAudio(false);
    vfos[currentVfo].rxF = savedFreq; /* не тащим позицию скана в VFO */
    currentVfo = savedVfo;
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (NumVal_Key(&numVal[endFSel ? 1 : 0], k, state))
      return true;

    if (state == KEY_LONG_PRESSED) {
      if (k == KEY_HASH) { /* пауза/продолжить */
        scanning = !scanning;
        isListening = false;
        setAudio(!scanning); /* в паузе слышно текущую частоту */
        return true;
      }
      return false;
    }

    if (state != KEY_RELEASED)
      return false;

    switch (k) {
    case KEY_MENU: /* захватить в VFO и выйти */
      currentVfo = savedVfo;
      Radio_TuneToAuto(curF);
      Mode_Switch(MODE_VFO);
      return true;
    case KEY_EXIT:
      Mode_Switch(MODE_VFO);
      return true;
    case KEY_HASH:
      if (!NumVal_IsEditing(&numVal[endFSel ? 1 : 0]))
        endFSel = !endFSel;
      return true;
    case KEY_SIDE1: /* водопад вкл/выкл */
      waterfall = !waterfall;
      if (!waterfall) {
        WF_Reset();
        FillRect(0, WF_Y, LCD_WIDTH, LCD_HEIGHT - WF_Y, C_BLACK);
      }
      return true;
    case KEY_STAR:
      NumVal_StartEdit(&numVal[endFSel ? 1 : 0]);
      return true;
    case KEY_UP:
      if (!scanning) { /* ручная перестройка в паузе */
        advance();
        tuneHere();
      } else {
        dwellIdx = (dwellIdx + 1) % DWELL_N;
      }
      return true;
    case KEY_DOWN:
      if (!scanning) {
        uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
        curF = (curF <= range.start + step) ? range.end
                                            : curF - step;
        tuneHere();
      } else {
        dwellIdx = (dwellIdx + DWELL_N - 1) % DWELL_N;
      }
      return true;
    default:
      break;
    }
    return false;
  }
};

ScannerMode scannerModeInstance;
Mode &MODE_SCANNER = scannerModeInstance;
