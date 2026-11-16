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
 * (blacklist) сканер пропускает всегда, белый (whitelist) — пока не
 * включён режим мониторинга.
 *
 * Клавиши: 0-9 / '*' — ввод границы (фокус — '#' короткий), MENU — захватить
 * частоту в VFO и выйти, MENU длинный — список находок, EXIT — выход,
 * '#' длинный — пауза/продолжить (в паузе UP/DOWN — ручная перестройка),
 * SIDE1 — в чёрный список последнюю находку (и дальше), SIDE1 длинный —
 * водопад вкл/выкл, SIDE2 — в белый список, SIDE2 длинный — мониторинг
 * (слушать и белые), UP/DOWN — шаг сканирования, '*' длинный — выдержка на
 * частоте (скорость сканирования). Подчёркнута та граница диапазона,
 * которую правим ('#' переключает).
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
#include "../ui/numval.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "loot_screen.h"
#include "mode.h"
#include "printf.h"
#include "vfo.h"
#include <stdlib.h>
#include <string.h>

/* Раскладка экрана. ВАЖНО: водопад использует hardware vscroll зоны
 * [WF_Y, HEIGHT) — ниже этой линии нельзя размещать никакой текст/виджеты,
 * они будут «плыть» вместе с водопадом. */
#define STATUS_H 24
/* поля частот — лейблы F_SM [y-11, y+4] с y = SPECTRUM_Y-10; под ними
 * (SPECTRUM_Y-4..-3) полоса-подчёркивание выбранной границы; ниже — спектр */
#define SPECTRUM_Y (STATUS_H + 24)
#define SP_H 52
#define STATE_Y (SPECTRUM_Y + SP_H + 10) /* строка состояния (с запасом от спектра) */
/* LVGL-лейбл F_SM занимает [y-11, y+4]: строка состояния (baseline
 * STATE_Y+12) кончается на STATE_Y+16. Водопад начинаем ниже — иначе
 * перерисовка лейбла (чёрный фон) пишет в память зоны скролла. */
#define INFO_Y (STATE_Y + 28)           /* строка находок (baseline) */
#define WF_Y (STATE_Y + 33)             /* водопад отсюда и до низа */

#define LISTEN_TIMEOUT_MS 5000
#define SQL_DELAY_MS 30
#define SQL_MARGIN 10 /* ~5 дБ над шумом (RSSI: 2 ед. = 1 дБ) */
#define SQL_HYST 4
#define WARM_STEPS 16 /* калибровка шума: первые шаги скана без поиска сигнала */
#define BATCH_STEPS 32 /* шагов за один проход главного цикла */

static FRange range = {.start = 400 * MHZ, .end = 470 * MHZ};

static NumVal_t numVal[2];
static bool endFSel;
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
static uint8_t savedStep;   /* шаг VFO сканера на входе */
static bool monitorMode;  /* слушать и белый список */
/* Тест второго BK (U400/B): на каждом шаге B настраивается на ту же частоту,
 * на экране — RSSI обоих. По схеме RT880 V2 вход B к антенне не подключён
 * (RSSI низкий); с сигналом на LNAIN/после доработки платы видно, слышит ли B
 * и насколько согласован с A. ALARM — вкл/выкл (замедляет скан). */
static bool probeB;
static uint16_t probeRssiA, probeRssiB;
static uint8_t scanChip; /* чип, которым сканируем (A) */
static bool captureOnExit; /* MENU: оставить частоту в VFO */
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
}
static void onEndEntered(uint32_t f) {
  range.end = f;
  onRangeChanged();
}

static void initNumVals() {
  /* выравнивание значения (и при вводе) задаёт pos: левое поле — от левого
   * края, правое — от правого */
  NumVal_Init(&numVal[0], 8, SPECTRUM_Y - 10, F_SM, 11, 12, UNIT_MHZ,
              range.start, 0, 1340 * MHZ, onStartEntered, POS_L, nullptr, 0,
              0);
  NumVal_Init(&numVal[1], LCD_WIDTH - 8, SPECTRUM_Y - 10, F_SM, 11, 12,
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

  if (probeB) { /* B на ту же частоту, замер, возврат на A */
    probeRssiA = rssi;
    BK4819_SelectChip(1);
    BK4819_TuneTo(curF, false);
    delay_us(DWELL_US[dwellIdx]);
    probeRssiB = BK4819_GetRSSI();
    BK4819_SelectChip(scanChip);
  }

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
    savedVfo = currentVfo;
    savedFreq = vfos[currentVfo].rxF;
    Radio_Init();
    if (vfos[currentVfo].radio != RADIO_BK4819A &&
        vfos[currentVfo].radio != RADIO_BK4819B)
      currentVfo = 0;
    scanVfo = currentVfo;
    savedStep = vfos[scanVfo].step;
    savedFreq = vfos[scanVfo].rxF; /* после Radio_Init */

    initNumVals();
    onRangeChanged();
    scanChip = vfos[currentVfo].radio == RADIO_BK4819B ? 1 : 0;
    BK4819_SelectChip(scanChip);
    probeB = false;
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
      SP_Render(&range, SPECTRUM_Y, SP_H);
      lastSpRender = now;
    }
    if (sweepDone) { /* строка водопада — раз за полный проход */
      sweepDone = false;
      if (waterfall)
        WF_Render(WF_Y, true);
    }

    /* подчёркивание редактируемой границы (жёлтое), у другой — чёрное.
     * Рисуется ПРЯМОЙ заливкой, а LVGL про неё не знает: любая его
     * перерисовка пересекающейся области (смена текста, удаление объектов
     * прошлого экрана) закрашивает её. Поэтому не «по смене», а каждый
     * кадр — две заливки 99×2 px. */
    {
      const int16_t uy = SPECTRUM_Y - 4, uw = 9 * 11;
      FillRect(8, uy, uw, 2, endFSel ? C_BLACK : C_YELLOW);
      FillRect(LCD_WIDTH - 8 - uw, uy, uw, 2, endFSel ? C_YELLOW : C_BLACK);
    }

    /* шаг и выдержка на частоте — между полями границ (мелким шрифтом) */
    uint32_t stp = StepFrequencyTable[vfos[currentVfo].step];
    PrintfT(LCD_XCENTER, SPECTRUM_Y - 14,
            text_style(C_YELLOW, C_BLACK, POS_C, F_SS), "%u.%02uk",
            (unsigned)(stp / 100), (unsigned)(stp % 100));
    PrintfT(LCD_XCENTER, SPECTRUM_Y - 8,
            text_style(C_GRAY, C_BLACK, POS_C, F_SS), "%uus",
            (unsigned)DWELL_US[dwellIdx]);

    /* строка состояния: SCAN/HOLD/PAUSE + частота + скорость */
    char state[6];
    strcpy(state, !scanning ? "PAUSE" : (isListening ? "HOLD" : "SCAN"));
    PrintfT(4, STATE_Y + 12, text_style(C_GREEN, C_BLACK, POS_L, F_SM),
            "%s %u.%05u", state, curF / 100000, curF % 100000);
    PrintfT(LCD_WIDTH - 4, STATE_Y + 12,
            text_style(C_YELLOW, C_BLACK, POS_R, F_SM), "%4u/s", stepsPerSec);

    /* строка находок: последняя находка (+белый/-чёрный), сколько назад;
     * справа — размер списка и режим мониторинга */
    const Loot *ll = gLastActiveLoot;
    if (ll) {
      uint32_t ago = LOOT_SecondsAgo(ll);
      PrintfT(4, INFO_Y, text_style(C_WHITE, C_BLACK, POS_L, F_SM),
              "%c%u.%05u %2u:%02u", ll->blacklist ? '-' : ll->whitelist ? '+' : ' ',
              (unsigned)(ll->f / MHZ), (unsigned)(ll->f % MHZ),
              (unsigned)(ago / 60 % 100), (unsigned)(ago % 60));
    } else {
      PrintfT(4, INFO_Y, text_style(C_GRAY, C_BLACK, POS_L, F_SM),
              "no loot yet     ");
    }
    if (probeB) {
      /* A и B: RSSI в дБм на текущей частоте (B ≈ A при подключённом входе) */
      PrintfT(LCD_WIDTH - 4, INFO_Y,
              text_style(C_YELLOW, C_BLACK, POS_R, F_SS), "A%d B%d  ",
              (int)(probeRssiA >> 1) - 160, (int)(probeRssiB >> 1) - 160);
    } else {
      PrintfT(LCD_WIDTH - 4, INFO_Y,
              text_style(monitorMode ? C_GREEN : C_GRAY, C_BLACK, POS_R, F_SS),
              "L%u%s", (unsigned)LOOT_Size(), monitorMode ? " MON" : "    ");
    }
  }

  void exit() override {
    setAudio(false);
    LOOT_Standby();
    if (probeB) { /* тест B включён — усыпить на выходе */
      probeB = false;
      Radio_BkBWake(false);
    }
    /* BK остаётся настроенным на последнюю частоту скана (и её фильтр
     * диапазона) — возвращаем VFO сканера его частоту и настраиваем чип,
     * иначе шумодав в VFO читается по чужой частоте. Исключение: MENU
     * забрал частоту в тот же VFO (там уже precise-настройка). */
    if (!(captureOnExit && scanVfo == savedVfo)) {
      currentVfo = scanVfo;
      vfos[scanVfo].rxF = savedFreq;
      vfos[scanVfo].step = static_cast<Step>(savedStep); /* шаг скана — не в VFO */
      Radio_TuneTo(savedFreq, true); /* заодно приёмник, модуляция, шумодав */
    }
    currentVfo = savedVfo;
    Radio_ScanAudio(false); /* precise-настройка приоткрыла звук — глушим;
                               дальше решает шумодав в VFO (Radio_Update) */
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (NumVal_Key(&numVal[endFSel ? 1 : 0], k, state))
      return true;

    if (state == KEY_LONG_PRESSED) {
      switch (k) {
      case KEY_HASH: /* пауза/продолжить */
        scanning = !scanning;
        isListening = false;
        setAudio(!scanning); /* в паузе слышно текущую частоту */
        return true;
      case KEY_MENU: /* список находок */
        LOOTLIST_OpenFrom(MODE_SCANNER);
        return true;
      case KEY_SIDE1: /* водопад вкл/выкл */
        waterfall = !waterfall;
        if (!waterfall) {
          WF_Reset();
          FillRect(0, WF_Y, LCD_WIDTH, LCD_HEIGHT - WF_Y, C_BLACK);
        }
        return true;
      case KEY_SIDE2: /* мониторинг: слушать и белый список */
        monitorMode = !monitorMode;
        return true;
      case KEY_STAR: /* выдержка на частоте (скорость скана) */
        dwellIdx = (dwellIdx + 1) % DWELL_N;
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
    case KEY_HASH:
      if (!NumVal_IsEditing(&numVal[endFSel ? 1 : 0]))
        endFSel = !endFSel;
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
    case KEY_ALARM: /* тест второго BK: RSSI A и B на одной частоте */
      probeB = !probeB;
      Radio_BkBWake(probeB); /* B обычно спит — будим только на время теста */
      return true;
    case KEY_STAR:
      NumVal_StartEdit(&numVal[endFSel ? 1 : 0]);
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
