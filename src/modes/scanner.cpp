/* Сканер — алгоритм заимствован из s0v4 (src/helper/scan.c):
 *
 * - адаптивный порог шумодава (ctx.floorRssi) тянется к noise floor;
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
#include "modes.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/log.h"
#include "../helper/lootlist.h"
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

/* Раскладка общая с анализатором — src/ui/screen_layout.h. ВАЖНО: ниже
 * UI_WATERFALL_Y начинается аппаратный vscroll: текста и виджетов там быть
 * не должно, иначе они «плывут» вместе с водопадом. */

#define LISTEN_TIMEOUT_MS 5000
#define SQL_DELAY_MS 30
#define SQL_MARGIN 10 /* ~5 дБ над шумом (RSSI: 2 ед. = 1 дБ) */
#define SQL_HYST 4
#define WARM_STEPS 16 /* калибровка шума: первые шаги скана без поиска сигнала */
#define BATCH_STEPS 32 /* шагов за один проход главного цикла */

static struct Ctx {
  FRange range = {.start = 400 * MHZ, .end = 470 * MHZ};
  NumVal_t numVal[2];
  uint8_t savedVfo;
  uint8_t scanVfo;    /* VFO, которым сканируем (BK) */
  uint32_t savedFreq; /* частота сканирующего VFO на входе */
  uint8_t dwellIdx = 2;
  uint32_t stepCount;
  uint32_t lastCpsTime;
  uint32_t stepsPerSec;
  uint32_t curF;        /* текущая позиция */
  uint16_t floorRssi;   /* noise floor (сырой RSSI) */
  bool sweepDone;       /* проход завершён — рисуем строку водопада */
  bool isListening;     /* HOLD: стоим и слушаем */
  bool scanning = true; /* false = пауза (ручная перестройка) */
  bool waterfall = true;
  bool dacOn;
  uint8_t warm;        /* шагов калибровки шума после смены диапазона */
  uint8_t creepCnt;    /* медленный подъём порога в HOLD на «шуме» */
  uint8_t savedMod;    /* модуляция VFO на входе (скан идёт в FM) */
  uint8_t savedStep;   /* шаг VFO сканера на входе */
  bool monitorMode;  /* слушать и белый список */
  uint8_t scanChip; /* чип, которым сканируем (A) */
  bool captureOnExit; /* MENU: оставить частоту в VFO */
  uint32_t listenUntil;
  uint8_t closedSteps;
  uint32_t lastSpRender;
  uint32_t lastWfRender;
} ctx;


/* скорость сканирования: выдержка на частоте до замера RSSI */
/* На 8 МГц накладные расходы SPI/GPIO давали эффективную паузу в сотни мкс;
 * на 150 МГц её нет — RSSI после перестройки не успевает устояться.
 * По умолчанию 1000 мкс, UP/DOWN меняет (отображается в центре над спектром). */
static const uint16_t DWELL_US[] = {250, 500, 1000, 2000, 4000};
#define DWELL_N (sizeof(DWELL_US) / sizeof(DWELL_US[0]))


/* состояние сканирования */

static void onRangeChanged() {
  WF_Reset();
  /* чистим всё под статусом: мусор прошлого экрана и старый водопад */
  FillRect(0, UI_STATUS_H, LCD_WIDTH, UI_WATERFALL_Y - UI_STATUS_H, C_BLACK);
  if (ctx.waterfall)
    WF_Clear(UI_WATERFALL_Y);
  else
    FillRect(0, UI_WATERFALL_Y, LCD_WIDTH, LCD_HEIGHT - UI_WATERFALL_Y,
             C_BLACK);
  SP_Init(&ctx.range, StepFrequencyTable[vfos[currentVfo].step],
          ctx.range.end - ctx.range.start);
  ctx.curF = ctx.range.start;
  ctx.floorRssi = 0;
  ctx.warm = WARM_STEPS;
  ctx.creepCnt = 0;
  ctx.sweepDone = false;
  ctx.isListening = false;
  ctx.closedSteps = 0;
  NumVal_Invalidate(&ctx.numVal[0]);
  NumVal_Invalidate(&ctx.numVal[1]);
  ctx.stepCount = 0;
  ctx.lastCpsTime = millis();
  ctx.stepsPerSec = 0;
}

/* Шаг сканирования: меняем шаг VFO сканера (на выходе вернём) и
 * перезапускаем скан — спектр строится по новой сетке. */
static void changeStep(int8_t d) {
  VFO *v = &vfos[currentVfo];
  v->step = static_cast<Step>((v->step + STEP_COUNT + d) % STEP_COUNT);
  onRangeChanged();
}

static void onStartEntered(uint32_t f) {
  ctx.range.start = f;
  onRangeChanged();
  NumVal_StartEdit(&ctx.numVal[1]); /* сразу переходим к правой границе */
}
static void onEndEntered(uint32_t f) {
  ctx.range.end = f;
  onRangeChanged();
}

static NumVal_t *editingNumVal() {
  if (NumVal_IsEditing(&ctx.numVal[0]))
    return &ctx.numVal[0];
  if (NumVal_IsEditing(&ctx.numVal[1]))
    return &ctx.numVal[1];
  return nullptr;
}

static void beginRangeEdit() {
  NumVal_SetValue(&ctx.numVal[0], ctx.range.start);
  NumVal_StartEdit(&ctx.numVal[0]);
}

static void initNumVals() {
  /* выравнивание значения (и при вводе) задаёт pos: левое поле — от левого
   * края, правое — от правого */
  NumVal_Init(&ctx.numVal[0], UI_LEFT_X, UI_RANGE_BASE, F_M, 11, 12, UNIT_MHZ,
              ctx.range.start, 0, 1340 * MHZ, onStartEntered, POS_L, nullptr, 0,
              0);
  NumVal_Init(&ctx.numVal[1], UI_RIGHT_X, UI_RANGE_BASE, F_M, 11, 12, UNIT_MHZ,
              ctx.range.end, 0, 1340 * MHZ, onEndEntered, POS_R, nullptr, 0, 0);
}

static void setAudio(bool on) {
  if (on != ctx.dacOn) {
    ctx.dacOn = on;
    Radio_ScanAudio(on); /* через applyMute — общий учёт DAC/AF_MUTE */
  }
}

static void advance() {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  ctx.curF += step;
  if (ctx.curF > ctx.range.end) {
    ctx.curF = ctx.range.start;
    ctx.sweepDone = true;
  }
  vfos[currentVfo].rxF = ctx.curF;
}

static void tuneHere() {
  vfos[currentVfo].rxF = ctx.curF;
  /* фильтр диапазона (VHF/UHF/...) — без этого остаётся от VFO и сигнал
   * вне его диапазона ослаблен; внутри — кэш по band, почти бесплатно */
  BK4819_SelectFilterByFrequency(ctx.curF);
  BK4819_TuneTo(ctx.curF, false);
}

/* Один шаг сканирования: замер, решение, движение.
 * Порог = noise floor (EMA по закрытым замерам) + SQL_MARGIN.
 * Раньше порог брался из Std() и был в разы ниже RSSI — всё считалось
 * «открытым» и сканер висел в HOLD/паузах. */
static void scanStep() {
  tuneHere();
  delay_us(DWELL_US[ctx.dwellIdx]);
  uint16_t rssi = BK4819_GetRSSI();

  /* калибровка шума после смены диапазона: быстрое усреднение, без поиска.
   * Иначе заниженная первая выборка давала порог ниже шума — «всё открыто»,
   * сканер вставал в HOLD и шипел, пока не поймает настоящий сигнал. */
  if (ctx.warm) {
    ctx.warm--;
    ctx.floorRssi = ctx.floorRssi ? (uint16_t)(((uint32_t)ctx.floorRssi * 3 + rssi + 2) / 4)
                          : rssi;
    Measurement w = {.f = ctx.curF, .rssi = rssi, .noise = 0, .open = false};
    SP_AddPoint(&w);
    ctx.stepCount++;
    advance();
    return;
  }
  if (!ctx.floorRssi)
    ctx.floorRssi = rssi;
  uint16_t thr = ctx.floorRssi + SQL_MARGIN;

  /* гистерезис: уже открытый канал закрываем чуть ниже порога */
  bool open = ctx.isListening ? (rssi + SQL_HYST >= thr) : (rssi >= thr);
  if (ctx.curF % 1300000 == 0)
    open = false; /* гармоники ПЧ — мусор */

  /* чёрный (и белый вне мониторинга) список: не открываемся и не учим
   * порог по сильному сигналу */
  bool skipped = false;
  if (open && !ctx.isListening &&
      LOOT_IsSkipped(ctx.curF, StepFrequencyTable[vfos[currentVfo].step] / 2,
                     ctx.monitorMode)) {
    open = false;
    skipped = true;
  }

  if (open && !ctx.isListening) {
    /* подтверждение: повторный замер через паузу */
    delay_ms(SQL_DELAY_MS);
    open = BK4819_GetRSSI() >= thr;
    if (!open && ctx.floorRssi < 0xFFFF - SQL_MARGIN)
      ctx.floorRssi++; /* ложное срабатывание — порог выше */
  }

  if (!open && !skipped) {
    ctx.floorRssi = (uint16_t)(((uint32_t)ctx.floorRssi * 15 + rssi + 8) / 16);
  } else if (open && ctx.isListening && rssi < (uint16_t)(thr + SQL_MARGIN)) {
    /* «слабое» открытие в HOLD: порог по шуму не должен залипать. Медленно
     * подтягиваем его (не быстрее ~1 ед. на 16 замеров), чтобы шум перестал
     * удерживать HOLD; сильный сигнал (≥ +MARGIN над порогом) не трогает. */
    if (++ctx.creepCnt >= 16) {
      ctx.creepCnt = 0;
      if (ctx.floorRssi < 0xFFFF - SQL_MARGIN)
        ctx.floorRssi++;
    }
  }

  Measurement m = {.f = ctx.curF, .rssi = rssi, .noise = 0, .open = open};
  SP_AddPoint(&m);
  ctx.stepCount++;

  if (open) {
    if (!ctx.isListening) {
      ctx.isListening = true;
      ctx.listenUntil = millis() + LISTEN_TIMEOUT_MS;
    }
    /* учёт находки: добавить/обновить, копить длительность */
    LOOT_Hit(ctx.curF, rssi, vfos[currentVfo].modulation, vfos[currentVfo].bw,
             vfos[currentVfo].radio);
    setAudio(true);
    if (millis() > ctx.listenUntil) {
      /* не закрылось за LISTEN_TIMEOUT_MS: либо несущая, либо порог ниже
       * шума — поднимаем порог выше текущего уровня, чтобы не залипнуть */
      ctx.floorRssi = rssi;
      ctx.isListening = false;
      setAudio(false);
      LOOT_Standby();
      advance();
    }
  } else {
    if (ctx.isListening) {
      ctx.isListening = false;
      setAudio(false);
      LOOT_Standby();
    }
    advance();
  }
}

/* Закончить удержание и идти дальше (после занесения в список) */
static void releaseHold() {
  if (ctx.isListening) {
    ctx.isListening = false;
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
    ctx.savedVfo = currentVfo;
    ctx.savedFreq = vfos[currentVfo].rxF;
    Radio_Init();
    if (vfos[currentVfo].radio != RADIO_BK4819A &&
        vfos[currentVfo].radio != RADIO_BK4819B)
      currentVfo = 0;
    ctx.scanVfo = currentVfo;
    ctx.savedStep = vfos[ctx.scanVfo].step;
    ctx.savedMod = vfos[ctx.scanVfo].modulation;
    if (ctx.savedMod == MOD_WFM) /* WFM 64–108 = SI4732, а скан идёт по BK */
      vfos[ctx.scanVfo].modulation = MOD_FM;
    ctx.savedFreq = vfos[ctx.scanVfo].rxF; /* после Radio_Init */
    Radio_FilterAuto(); /* скан сам выбирает фильтр по частоте */
    SP_ResetLevel();

    initNumVals();
    onRangeChanged();
    ctx.scanChip = vfos[currentVfo].radio == RADIO_BK4819B ? 1 : 0;
    BK4819_SelectChip(ctx.scanChip);
    ctx.dacOn = false;
    Radio_ScanAudio(false);
    ctx.captureOnExit = false;
  }

  void update() override {
    NumVal_Update(&ctx.numVal[0]);
    NumVal_Update(&ctx.numVal[1]);
    bool editing = NumVal_IsEditing(&ctx.numVal[0]) || NumVal_IsEditing(&ctx.numVal[1]);
    if (ctx.scanning && !editing) {
      /* пачка шагов — иначе один шаг на итерацию главного цикла (LVGL,
       * рендер) даёт единицы-десятки шагов/с */
      for (uint8_t i = 0; i < BATCH_STEPS; i++) {
        scanStep();
        if (ctx.isListening || ctx.sweepDone)
          break;
      }
    }

    uint32_t now = millis();
    if (now - ctx.lastCpsTime >= 1000) {
      ctx.stepsPerSec = ctx.stepCount * 1000 / (now - ctx.lastCpsTime);
      ctx.stepCount = 0;
      ctx.lastCpsTime = now;
    }
  }

  void render() override {
    STATUSLINE_render();

    NumVal_Render(&ctx.numVal[0]);
    NumVal_Render(&ctx.numVal[1]);

    uint32_t now = millis();
    if (now - ctx.lastSpRender >= 300) {
      SP_Render(&ctx.range, UI_SPECTRUM_Y, UI_SPECTRUM_H);
      ctx.lastSpRender = now;
    }
    if (ctx.sweepDone) { /* строка водопада — раз за полный проход */
      ctx.sweepDone = false;
      if (ctx.waterfall)
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
    const char *state = !ctx.scanning ? "PAUSE" : (ctx.isListening ? "HOLD" : "SCAN");
    Color stateCol = !ctx.scanning ? C_GRAY : (ctx.isListening ? C_YELLOW : C_GREEN);
    PrintfT(UI_LEFT_X, UI_INFO1_BASE,
            text_style(stateCol, C_BLACK, POS_L, F_M), "%s", state);
    PrintfT(LCD_XCENTER, UI_INFO1_BASE,
            text_style(C_WHITE, C_BLACK, POS_C, F_M), "%u.%05u",
            (unsigned)(ctx.curF / 100000), (unsigned)(ctx.curF % 100000));
    PrintfT(UI_RIGHT_X, UI_INFO1_BASE,
            text_style(C_YELLOW, C_BLACK, POS_R, F_M), "%u/s",
            (unsigned)ctx.stepsPerSec);

    /* Строка 2: последняя находка | выдержка | список/мониторинг (или A/B). */
    const Loot *ll = gLastActiveLoot;
    if (ll) {
      char fb[16];
      fmt_mhz5(fb, sizeof(fb), ll->f);
      PrintfT(UI_LEFT_X, UI_INFO2_BASE,
              text_style(ll->open ? C_YELLOW : C_WHITE, C_BLACK, POS_L, F_S),
              "%c%s", ll->blacklist ? '-' : ll->whitelist ? '+' : ' ', fb);
    } else {
      PrintfT(UI_LEFT_X, UI_INFO2_BASE,
              text_style(C_GRAY, C_BLACK, POS_L, F_S), "no loot yet");
    }
    PrintfT(LCD_XCENTER, UI_INFO2_BASE,
            text_style(C_GRAY, C_BLACK, POS_C, F_S), "%uus",
            (unsigned)DWELL_US[ctx.dwellIdx]);
    PrintfT(UI_RIGHT_X, UI_INFO2_BASE,
            text_style(ctx.monitorMode ? C_GREEN : C_GRAY, C_BLACK, POS_R, F_S),
            "L%u%s", (unsigned)LOOT_Size(), ctx.monitorMode ? " MON" : "");
  }

  void exit() override {
    setAudio(false);
    LOOT_Standby();
    /* BK остаётся настроенным на последнюю частоту скана (и её фильтр
     * диапазона) — возвращаем VFO сканера его частоту и настраиваем чип,
     * иначе шумодав в VFO читается по чужой частоте. Исключение: MENU
     * забрал частоту в тот же VFO (там уже precise-настройка). */
    if (!(ctx.captureOnExit && ctx.scanVfo == ctx.savedVfo)) {
      currentVfo = ctx.scanVfo;
      vfos[ctx.scanVfo].rxF = ctx.savedFreq;
      vfos[ctx.scanVfo].modulation = static_cast<ModulationType>(ctx.savedMod);
      vfos[ctx.scanVfo].step = static_cast<Step>(ctx.savedStep); /* шаг скана — не в VFO */
      Radio_TuneTo(ctx.savedFreq, true); /* заодно приёмник, модуляция, шумодав */
    }
    currentVfo = ctx.savedVfo;
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
        ctx.waterfall = !ctx.waterfall;
        if (!ctx.waterfall) {
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
      currentVfo = ctx.savedVfo;
      Radio_TuneToAuto(ctx.curF);
      ctx.captureOnExit = true; /* exit() иначе вернул бы прежнюю частоту */
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
      if (ctx.dwellIdx + 1 < (int)DWELL_N)
        ctx.dwellIdx++;
      return true;
    case KEY_7: /* выдержка на частоте − */
      if (ctx.dwellIdx > 0)
        ctx.dwellIdx--;
      return true;
    case KEY_3: /* шаг сканирования + */
      changeStep(+1);
      return true;
    case KEY_9: /* шаг сканирования − */
      changeStep(-1);
      return true;
    case KEY_UP:
      if (!ctx.scanning) { /* ручная перестройка в паузе */
        advance();
        tuneHere();
      } else {
        changeStep(+1);
      }
      return true;
    case KEY_DOWN:
      if (!ctx.scanning) {
        uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
        ctx.curF = (ctx.curF <= ctx.range.start + step) ? ctx.range.end
                                            : ctx.curF - step;
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
