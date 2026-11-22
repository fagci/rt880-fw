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
 * Находки копятся в списке loot (helper/lootlist): каждый подтверждённый
 * сигнал — запись с частотой, длительностью, пиком. Чёрный список
 * (blacklist) сканер пропускает всегда, белый (whitelist) — тоже.
 *
 * Клавиши: 5 — ввод границ (сначала начало, затем конец),
 * MENU — захватить частоту в VFO и выйти, MENU длинный — список находок,
 * EXIT — выход, SIDE1 — в чёрный список последнюю находку (и дальше),
 * SIDE1 длинный — водопад вкл/выкл, SIDE2 — в белый список,
 * UP/DOWN — шаг сканирования, 1/7 — выдержка на частоте (скорость
 * сканирования), 3/9 — шаг сканирования.
 */
#include "scanner.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/log.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/lv_port.h"
#include "../ui/numval.h"
#include "../ui/screen_layout.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "loot_screen.h"
#include "mode.h"
#include "printf.h"
#include "vfo.h"
#include <stdlib.h>
#include <string.h>

/* Раскладка общая с анализатором — src/ui/screen_layout.h. ВАЖНО: ниже
 * UI_WATERFALL_Y начинается аппаратный vscroll: текста и виджетов там быть
 * не должно, иначе они «плывут» вместе с водопадом. */

#define LISTEN_TIMEOUT_MS 5000
#define SQL_DELAY_MS 30
#define SQL_MARGIN 10 /* ~5 дБ над шумом (RSSI: 2 ед. = 1 дБ) */
#define SQL_HYST 4
#define WARM_STEPS 16 /* калибровка шума: первые шаги скана без поиска сигнала */
#define BATCH_STEPS 32 /* шагов за один проход главного цикла */

static FRange range = {.start = 400 * MHZ, .end = 470 * MHZ};

static NumVal_t numVal[2];
static uint8_t savedVfo;
static uint8_t scanVfo;    /* VFO, которым сканируем (BK) */
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
static uint8_t warm;        /* шагов калибровки шума после смены диапазона */
static uint8_t creepCnt;    /* медленный подъём порога в HOLD на «шуме» */
static uint8_t savedMod;    /* модуляция VFO на входе (скан идёт в FM) */
static uint8_t savedStep;   /* шаг VFO сканера на входе */
static bool monitorMode;  /* слушать и белый список */
static uint8_t scanChip; /* чип, которым сканируем (A) */
static bool captureOnExit; /* MENU: оставить частоту в VFO */
static uint32_t listenUntil;
static uint8_t closedSteps;
static uint32_t lastSpRender;
static uint32_t lastWfRender;

static void onRangeChanged() {
  WF_Reset();
  /* чистим всё под статусом: мусор прошлого экрана и старый водопад */
  FillRect(0, UI_STATUS_H, LCD_WIDTH, UI_WATERFALL_Y - UI_STATUS_H, C_BLACK);
  if (waterfall)
    WF_Clear(UI_WATERFALL_Y);
  else
    FillRect(0, UI_WATERFALL_Y, LCD_WIDTH, LCD_HEIGHT - UI_WATERFALL_Y,
             C_BLACK);
  SP_Init(&range, StepFrequencyTable[vfos[currentVfo].step],
          range.end - range.start);
  curF = range.start;
  floorRssi = 0;
  warm = WARM_STEPS;
  creepCnt = 0;
  sweepDone = false;
  isListening = false;
  closedSteps = 0;
  NumVal_Invalidate(&numVal[0]);
  NumVal_Invalidate(&numVal[1]);
  stepCount = 0;
  lastCpsTime = millis();
  stepsPerSec = 0;
}

/* Шаг сканирования: меняем шаг VFO сканера (на выходе вернём) и
 * перезапускаем скан — спектр строится по новой сетке. */
static void changeStep(int8_t d) {
  VFO *v = &vfos[currentVfo];
  v->step = static_cast<Step>((v->step + STEP_COUNT + d) % STEP_COUNT);
  onRangeChanged();
}

static void onStartEntered(uint32_t f) {
  range.start = f;
  onRangeChanged();
  NumVal_StartEdit(&numVal[1]); /* сразу переходим к правой границе */
}
static void onEndEntered(uint32_t f) {
  range.end = f;
  onRangeChanged();
}

static NumVal_t *editingNumVal() {
  if (NumVal_IsEditing(&numVal[0]))
    return &numVal[0];
  if (NumVal_IsEditing(&numVal[1]))
    return &numVal[1];
  return nullptr;
}

static void beginRangeEdit() {
  NumVal_SetValue(&numVal[0], range.start);
  NumVal_StartEdit(&numVal[0]);
}

static void initNumVals() {
  /* выравнивание значения (и при вводе) задаёт pos: левое поле — от левого
   * края, правое — от правого */
  NumVal_Init(&numVal[0], UI_LEFT_X, UI_RANGE_BASE, F_M, 11, 12, UNIT_MHZ,
              range.start, 0, 1340 * MHZ, onStartEntered, POS_L, nullptr, 0,
              0);
  NumVal_Init(&numVal[1], UI_RIGHT_X, UI_RANGE_BASE, F_M, 11, 12, UNIT_MHZ,
              range.end, 0, 1340 * MHZ, onEndEntered, POS_R, nullptr, 0, 0);
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

  /* калибровка шума после смены диапазона: быстрое усреднение, без поиска.
   * Иначе заниженная первая выборка давала порог ниже шума — «всё открыто»,
   * сканер вставал в HOLD и шипел, пока не поймает настоящий сигнал. */
  if (warm) {
    warm--;
    floorRssi = floorRssi ? (uint16_t)(((uint32_t)floorRssi * 3 + rssi + 2) / 4)
                          : rssi;
    Measurement w = {.f = curF, .rssi = rssi, .noise = 0, .open = false};
    SP_AddPoint(&w);
    stepCount++;
    advance();
    return;
  }
  if (!floorRssi)
    floorRssi = rssi;
  uint16_t thr = floorRssi + SQL_MARGIN;

  /* гистерезис: уже открытый канал закрываем чуть ниже порога */
  bool open = isListening ? (rssi + SQL_HYST >= thr) : (rssi >= thr);
  if (curF % 1300000 == 0)
    open = false; /* гармоники ПЧ — мусор */

  /* чёрный (и белый вне мониторинга) список: не открываемся и не учим
   * порог по сильному сигналу */
  bool skipped = false;
  if (open && !isListening &&
      LOOT_IsSkipped(curF, StepFrequencyTable[vfos[currentVfo].step] / 2,
                     monitorMode)) {
    open = false;
    skipped = true;
  }

  if (open && !isListening) {
    /* подтверждение: повторный замер через паузу */
    delay_ms(SQL_DELAY_MS);
    open = BK4819_GetRSSI() >= thr;
    if (!open && floorRssi < 0xFFFF - SQL_MARGIN)
      floorRssi++; /* ложное срабатывание — порог выше */
  }

  if (!open && !skipped) {
    floorRssi = (uint16_t)(((uint32_t)floorRssi * 15 + rssi + 8) / 16);
  } else if (open && isListening && rssi < (uint16_t)(thr + SQL_MARGIN)) {
    /* «слабое» открытие в HOLD: порог по шуму не должен залипать. Медленно
     * подтягиваем его (не быстрее ~1 ед. на 16 замеров), чтобы шум перестал
     * удерживать HOLD; сильный сигнал (≥ +MARGIN над порогом) не трогает. */
    if (++creepCnt >= 16) {
      creepCnt = 0;
      if (floorRssi < 0xFFFF - SQL_MARGIN)
        floorRssi++;
    }
  }

  Measurement m = {.f = curF, .rssi = rssi, .noise = 0, .open = open};
  SP_AddPoint(&m);
  stepCount++;

  if (open) {
    if (!isListening) {
      isListening = true;
      listenUntil = millis() + LISTEN_TIMEOUT_MS;
    }
    /* учёт находки: добавить/обновить, копить длительность */
    LOOT_Hit(curF, rssi, vfos[currentVfo].modulation, vfos[currentVfo].bw,
             vfos[currentVfo].radio);
    setAudio(true);
    if (millis() > listenUntil) {
      /* не закрылось за LISTEN_TIMEOUT_MS: либо несущая, либо порог ниже
       * шума — поднимаем порог выше текущего уровня, чтобы не залипнуть */
      floorRssi = rssi;
      isListening = false;
      setAudio(false);
      LOOT_Standby();
      advance();
    }
  } else {
    if (isListening) {
      isListening = false;
      setAudio(false);
      LOOT_Standby();
    }
    advance();
  }
}

/* Закончить удержание и идти дальше (после занесения в список) */
static void releaseHold() {
  if (isListening) {
    isListening = false;
    setAudio(false);
  }
  LOOT_Standby();
  advance();
}

class ScannerMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("SCAN");
    lv_port_set_direct_areas(true);
    savedVfo = currentVfo;
    savedFreq = vfos[currentVfo].rxF;
    Radio_Init();
    if (vfos[currentVfo].radio != RADIO_BK4819A &&
        vfos[currentVfo].radio != RADIO_BK4819B)
      currentVfo = 0;
    scanVfo = currentVfo;
    savedStep = vfos[scanVfo].step;
    savedMod = vfos[scanVfo].modulation;
    if (savedMod == MOD_WFM) /* WFM 64–108 = SI4732, а скан идёт по BK */
      vfos[scanVfo].modulation = MOD_FM;
    savedFreq = vfos[scanVfo].rxF; /* после Radio_Init */

    initNumVals();
    onRangeChanged();
    scanChip = vfos[currentVfo].radio == RADIO_BK4819B ? 1 : 0;
    BK4819_SelectChip(scanChip);
    dacOn = false;
    Radio_ScanAudio(false);
    captureOnExit = false;
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
      SP_Render(&range, UI_SPECTRUM_Y, UI_SPECTRUM_H);
      lastSpRender = now;
    }
    if (sweepDone) { /* строка водопада — раз за полный проход */
      sweepDone = false;
      if (waterfall)
        WF_Render(UI_WATERFALL_Y, true);
      SP_Begin(); /* пересчитать автоуровни по завершённому проходу */
    }

    /* шаг между полями границ; при вводе не прячем — принятая граница
     * остаётся видимой рядом с редактором */
    uint32_t stp = StepFrequencyTable[vfos[currentVfo].step];
    PrintfT(LCD_XCENTER, UI_RANGE_BASE,
            text_style(C_YELLOW, C_BLACK, POS_C, F_S), "%u.%02luk",
            (unsigned)(stp / 100), (unsigned)(stp % 100));

    /* Строка 1: состояние | текущая частота | скорость. */
    const char *state = !scanning ? "PAUSE" : (isListening ? "HOLD" : "SCAN");
    Color stateCol = !scanning ? C_GRAY : (isListening ? C_YELLOW : C_GREEN);
    PrintfT(UI_LEFT_X, UI_INFO1_BASE,
            text_style(stateCol, C_BLACK, POS_L, F_M), "%s", state);
    PrintfT(LCD_XCENTER, UI_INFO1_BASE,
            text_style(C_WHITE, C_BLACK, POS_C, F_M), "%u.%05u",
            (unsigned)(curF / 100000), (unsigned)(curF % 100000));
    PrintfT(UI_RIGHT_X, UI_INFO1_BASE,
            text_style(C_YELLOW, C_BLACK, POS_R, F_M), "%u/s",
            (unsigned)stepsPerSec);

    /* Строка 2: последняя находка | выдержка | список/мониторинг (или A/B). */
    const Loot *ll = gLastActiveLoot;
    if (ll) {
      PrintfT(UI_LEFT_X, UI_INFO2_BASE,
              text_style(ll->open ? C_YELLOW : C_WHITE, C_BLACK, POS_L, F_S),
              "%c%u.%05u", ll->blacklist ? '-' : ll->whitelist ? '+' : ' ',
              (unsigned)(ll->f / MHZ), (unsigned)(ll->f % MHZ));
    } else {
      PrintfT(UI_LEFT_X, UI_INFO2_BASE,
              text_style(C_GRAY, C_BLACK, POS_L, F_S), "no loot yet");
    }
    PrintfT(LCD_XCENTER, UI_INFO2_BASE,
            text_style(C_GRAY, C_BLACK, POS_C, F_S), "%uus",
            (unsigned)DWELL_US[dwellIdx]);
    PrintfT(UI_RIGHT_X, UI_INFO2_BASE,
            text_style(monitorMode ? C_GREEN : C_GRAY, C_BLACK, POS_R, F_S),
            "L%u%s", (unsigned)LOOT_Size(), monitorMode ? " MON" : "");
  }

  void exit() override {
    setAudio(false);
    LOOT_Standby();
    /* BK остаётся настроенным на последнюю частоту скана (и её фильтр
     * диапазона) — возвращаем VFO сканера его частоту и настраиваем чип,
     * иначе шумодав в VFO читается по чужой частоте. Исключение: MENU
     * забрал частоту в тот же VFO (там уже precise-настройка). */
    if (!(captureOnExit && scanVfo == savedVfo)) {
      currentVfo = scanVfo;
      vfos[scanVfo].rxF = savedFreq;
      vfos[scanVfo].modulation = static_cast<ModulationType>(savedMod);
      vfos[scanVfo].step = static_cast<Step>(savedStep); /* шаг скана — не в VFO */
      Radio_TuneTo(savedFreq, true); /* заодно приёмник, модуляция, шумодав */
    }
    currentVfo = savedVfo;
    Radio_ScanAudio(false); /* precise-настройка приоткрыла звук — глушим;
                               дальше решает шумодав в VFO (Radio_Update) */
    lv_port_set_direct_areas(false);
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    /* пока идёт ввод — все клавиши редактору (сначала начало, затем конец) */
    if (NumVal_t *ev = editingNumVal()) {
      if (NumVal_Key(ev, k, state))
        return true;
      return false;
    }

    if (state == KEY_LONG_PRESSED) {
      switch (k) {
      case KEY_MENU: /* список находок */
        LOOTLIST_OpenFrom(MODE_SCANNER);
        return true;
      case KEY_SIDE1: /* водопад вкл/выкл */
        waterfall = !waterfall;
        if (!waterfall) {
          WF_Reset();
          FillRect(0, UI_WATERFALL_Y, LCD_WIDTH, LCD_HEIGHT - UI_WATERFALL_Y,
                   C_BLACK);
        }
        return true;
      case KEY_5: /* сброс пикового удержания */
        SP_ResetPeaks();
        return true;
      default:
        return false;
      }
    }

    if (state != KEY_RELEASED)
      return false;

    switch (k) {
    case KEY_MENU: /* захватить в VFO и выйти */
      currentVfo = savedVfo;
      Radio_TuneToAuto(curF);
      captureOnExit = true; /* exit() иначе вернул бы прежнюю частоту */
      Mode_Switch(MODE_VFO);
      return true;
    case KEY_EXIT:
      Mode_Switch(MODE_VFO);
      return true;
    case KEY_SIDE1: /* в чёрный список и дальше */
      if (gLastActiveLoot) {
        LOOT_BlacklistLast();
        releaseHold();
      }
      return true;
    case KEY_SIDE2: /* в белый список и дальше */
      if (gLastActiveLoot) {
        LOOT_WhitelistLast();
        releaseHold();
      }
      return true;
    case KEY_5: /* ввод границ: сначала начало, затем конец */
      beginRangeEdit();
      return true;
    case KEY_1: /* выдержка на частоте + */
      if (dwellIdx + 1 < (int)DWELL_N)
        dwellIdx++;
      return true;
    case KEY_7: /* выдержка на частоте − */
      if (dwellIdx > 0)
        dwellIdx--;
      return true;
    case KEY_3: /* шаг сканирования + */
      changeStep(+1);
      return true;
    case KEY_9: /* шаг сканирования − */
      changeStep(-1);
      return true;
    case KEY_UP:
      if (!scanning) { /* ручная перестройка в паузе */
        advance();
        tuneHere();
      } else {
        changeStep(+1);
      }
      return true;
    case KEY_DOWN:
      if (!scanning) {
        uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
        curF = (curF <= range.start + step) ? range.end
                                            : curF - step;
        tuneHere();
      } else {
        changeStep(-1);
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
