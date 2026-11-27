/* Сканер — алгоритм заимствован из s0v4 (src/helper/scan.c):
 *
 * - адаптивный порог шумодава (ctx.floorRssi) тянется к noise floor;
 * - быстрый проход идёт на короткой выдержке; найденный кандидат
 *   подтверждается локальным окном назад на длинной выдержке (RSSI-фильтр
 *   BK «тянет» пик на 1-6 шагов вперёд), ложные — поднимают порог;
 * - HOLD: стоим на открытой частоте и слушаем (AF DAC вкл), пока шумодав
 *   не закроется; после закрытия ещё держим частоту Stay time, затем дальше
 *   (Listen t/o ограничивает общее время HOLD, по умолчанию None);
 * - закрытая частота — дальше; каждые 64 закрытых шага порог дрейфует вниз;
 * - частоты-кратные 1.3 МГц (гармоники ПЧ) пропускаются;
 * - каждый замер попадает в спектр (SP_AddPoint), водопад — по строке;
 * - под спектром — свежие находки (10 строк: частота, «сколько назад»,
 *   пик), внизу — история уровня RSSI во времени (Level in VFO, как
 *   SP_RenderGraph в k1-fw); водопад по умолчанию выключен (long SIDE1),
 *   при нём список и график уступают зону;
 * - во время прохода экран обновляется не чаще раза в 500 мс: SPI-заливки
 *   дисплея дают помеху в 437–441 МГц. Спектр (SP_Render дорисовывает
 *   только изменившиеся столбцы) и текст (частота/состояние) — раз в 500 мс,
 *   полный кадр водопада — на границе прохода; после любого кадра следующий
 *   замер — с паузой на восстановление тракта.
 *   В HOLD дорисовывается только новый столбец графика уровня (прямой DMA):
 *   видно изменение во времени, а полный кадр не заливается.
 *
 * Только BK4819: используется VFO на BK (при необходимости временно
 * переключаемся на него, восстанавливаем на выходе).
 *
 * Находки копятся в списке loot (helper/lootlist): каждый подтверждённый
 * сигнал — запись с частотой, длительностью, пиком. Чёрный список
 * (blacklist) сканер пропускает всегда, белый (whitelist) — тоже.
 *
 * Клавиши текущего скана (элемент 0 списка): 5 — ввод границ (сначала
 * начало, затем конец), MENU — захватить частоту в VFO и выйти, STAR —
 * список находок, MENU длинный — меню (шаг/скорость), EXIT — выход,
 * SIDE1 — в чёрный список последнюю находку (и дальше), SIDE1 длинный —
 * водопад вкл/выкл, SIDE2 — в белый список, 1/7 — выдержка на частоте,
 * 3/9 — шаг сканирования, UP/DOWN — вход в список находок.
 *
 * В списке (выбранная находка): UP/DOWN — по кругу (за концами — обратно
 * к сканированию), 1/2/3/4 — сортировка (время/длительность/чёрные/
 * частота), SIDE1/SIDE2 — чёрный/белый список, 0 — удалить (длинный 0 —
 * очистить), длинный 5/8 — whitelist/blacklist в каналы, длинный 9 —
 * каналы в loot, длинный SIDE2 — выбранную в канал, MENU — в VFO и выйти,
 * EXIT — назад к сканированию.
 */
#include "modes.h"
#include "vfo_session.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/si473x.h"
#include "../driver/st7789.h"
#include "../driver/uart.h"
#include "../helper/channels.h"
#include "../helper/log.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../helper/names.h"
#include "../helper/settings.h"
#include "../helper/store.h"
#include "../helper/unit_fmt.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/level_graph.h"
#include "../ui/lv_port.h"
#include "../ui/menu_overlay.h"
#include "../ui/numval.h"
#include "../ui/radio_menu.h"
#include "../ui/screen_layout.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "mode.h"
#include "printf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Раскладка общая с анализатором — src/ui/screen_layout.h. ВАЖНО: ниже
 * UI_WATERFALL_Y начинается аппаратный vscroll: текста и виджетов там быть
 * не должно, иначе они «плывут» вместе с водопадом. */

#define SQL_MARGIN 10 /* ~5 дБ над шумом (RSSI: 2 ед. = 1 дБ) */
#define SQL_HYST 4
/* Подтверждение: пик в окне должен быть заметно выше второго максимума.
 * Абсолютный порог не годится — шкала RSSI на короткой и длинной выдержке
 * разная (фильтр/АРУ), и шум на длинной иногда перекрывает порог,
 * посчитанный по быстрым замерам. */
#define SQL_PEAK_MARGIN 16
#define WARM_STEPS 16 /* калибровка шума: первые шаги скана без поиска сигнала */
#define BATCH_STEPS 8 /* шагов за проход главного цикла (~5 мс при 500 мкс) */
/* RSSI-фильтр BK после перестройки тянется ~1 мс: при короткой выдержке
 * сигнал «виден» на 1-6 шагов выше своей частоты. Кандидат проверяем
 * локальным окном назад с длинной выдержкой. */
#define RSSI_LAG_US 1500
#define CONFIRM_DWELL_US 2500
#define CONFIRM_MAX_BINS 12
#define LEVEL_MS 250 /* шаг истории уровня в HOLD (240 px = 60 с) */

static struct Ctx {
  FRange range = {.start = 400 * MHZ, .end = 470 * MHZ};
  NumVal_t numVal[2];
  uint8_t savedVfo;
  uint8_t scanVfo;    /* VFO, которым сканируем (BK) */
  uint32_t savedFreq; /* частота сканирующего VFO на входе */
  uint32_t stepCount;
  uint32_t lastCpsTime;
  uint32_t stepsPerSec;
  uint32_t curF;        /* текущая позиция */
  uint16_t floorRssi;   /* noise floor (сырой RSSI) */
  uint16_t holdThr;     /* порог удержания по длинной выдержке */
  bool sweepDone;       /* проход завершён — рисуем кадр */
  bool isListening;     /* HOLD: стоим и слушаем */
  bool menuWasOpen;     /* меню закрылось — перерисовать спектр */
  bool paintHeld;       /* кадр HOLD нарисован — экран не трогаем */
  bool wasMeasuring;    /* прошлый кадр замеряли — нужен кадр на переходе */
  bool redraw;          /* клавиша изменила экран — нарисовать сразу */
  bool spiGuard;        /* после кадра — выдержать паузу перед замером */
  uint16_t guardUs;     /* пауза для первого замера после кадра, мкс */
  uint32_t lastFrameMs; /* последний нарисованный кадр — не реже раза в 500 мс */
  bool scanning = true; /* false = пауза (ручная перестройка) */
  bool waterfall = false; /* по умолчанию список находок, не водопад */
  bool dacOn;
  uint16_t lastRssi;      /* последний RSSI — для графика уровня */
  uint32_t lastLevelMs;   /* последняя выборка графика */
  uint8_t warm;        /* шагов калибровки шума после смены диапазона */
  uint8_t creepCnt;    /* медленный подъём порога в HOLD на «шуме» */
  uint8_t savedMod;    /* модуляция VFO на входе (скан идёт в FM) */
  uint8_t savedStep;   /* шаг VFO сканера на входе */
  bool monitorMode;  /* слушать и белый список */
  uint8_t scanChip; /* чип, которым сканируем (A) */
  bool captureOnExit; /* MENU: оставить частоту в VFO */
  uint32_t listenUntil;
  uint32_t closedAt; /* момент закрытия шумодава (0 — сигнал есть) */
  uint8_t closedSteps;
  uint32_t lastSpRender;
  uint32_t lastWfRender;
  /* Список находок под спектром: 0 — текущий скан, 1..N — запись loot.
   * Стрелки ходят по кругу; выход за края возвращает к сканированию. */
  uint16_t listSel;
  uint16_t listTop;
  bool listWasHold; /* на входе в список стояли в HOLD */
  VfoSnapshot listSaved;
  uint8_t sortIdx;
  bool sortRev;
  char msg[16];
  uint32_t msgUntil;
  /* TEMP: тест помех от дисплея. */
  bool dbg;
  uint8_t dbgFlags;
  uint32_t dbgUntil;
  uint32_t dbgSteps;
  uint16_t dbgHits;
} ctx;


/* Выдержка на частоте до замера RSSI — Settings → Scan delay (мкс, шаг
 * 100). Умолчание 1800 мкс — как в k1-fw. 1/7 (и пункт Speed меню
 * сканера) меняют её; значение отображается в центре над спектром. */
static uint16_t scanDwellUs(void) {
  int32_t v = Settings_Get(SET_SCANDELAY);
  if (v < 100)
    v = 100;
  if (v > 10000)
    v = 10000;
  return (uint16_t)v;
}

static void adjustScanDelay(int8_t dir) {
  int32_t v = Settings_Get(SET_SCANDELAY) + dir * 100;
  if (v < 100)
    v = 10000;
  if (v > 10000)
    v = 100;
  Settings_Set(SET_SCANDELAY, v);
}

/* Экран рисуется прямым SPI и даёт помеху в 437–441 МГц. Замер на стенде
 * (harm next, 438.775 МГц после кадра): при выдержке 500 мкс RSSI +39 ед.,
 * 2500 мкс +13, 5000 мкс — уже как без кадра. Первому замеру после кадра
 * даём 5 мс на восстановление тракта/АРУ. */
#define SPI_SETTLE_US 5000


/* состояние сканирования */

static void primeStart();
static void hideLootList(void);

/* Очередная выборка истории уровня (Level in VFO). Не чаще LEVEL_MS:
 * 240 столбцов при 250 мс — окно 60 с. */
static void levelTick(uint32_t now) {
  if (now - ctx.lastLevelMs < LEVEL_MS)
    return;
  ctx.lastLevelMs = now;
  LevelGraph_Add(UI_LEVEL_Y, ctx.lastRssi, UI_LEVEL_H);
}

static void setWaterfall(bool on) {
  if (on == ctx.waterfall)
    return;
  ctx.waterfall = on;
  lv_port_set_text_bottom(on ? UI_WATERFALL_Y - 3 : UI_LEVEL_Y - 1);
  WF_Reset();
  if (on) {
    hideLootList(); /* спрятать один раз: WF закрасит зону */
    WF_Clear(UI_WATERFALL_Y);
  } else {
    FillRect(0, UI_WATERFALL_Y, LCD_WIDTH, LCD_HEIGHT - UI_WATERFALL_Y,
             C_BLACK);
    LevelGraph_Reset(UI_LEVEL_Y, UI_LEVEL_H);
    ctx.lastLevelMs = millis() - LEVEL_MS; /* первый столбец — сразу */
  }
  ctx.redraw = true;
}

static void onRangeChanged() {
  WF_Reset();
  /* чистим всё под статусом: мусор прошлого экрана и старый водопад */
  FillRect(0, UI_STATUS_H, LCD_WIDTH, UI_WATERFALL_Y - UI_STATUS_H, C_BLACK);
  if (ctx.waterfall) {
    WF_Clear(UI_WATERFALL_Y);
  } else {
    FillRect(0, UI_WATERFALL_Y, LCD_WIDTH, LCD_HEIGHT - UI_WATERFALL_Y,
             C_BLACK);
    LevelGraph_Reset(UI_LEVEL_Y, UI_LEVEL_H);
  }
  /* Заливка затёрла и лейблы (границы/частоту/список) — печатаем заново. */
  UI_RedrawLabels();
  ctx.lastLevelMs = millis() - LEVEL_MS; /* первый столбец — сразу */
  SP_Init(&ctx.range, StepFrequencyTable[vfos[currentVfo].step],
          ctx.range.end - ctx.range.start);
  ctx.curF = ctx.range.start;
  ctx.floorRssi = 0;
  ctx.warm = WARM_STEPS;
  ctx.creepCnt = 0;
  ctx.sweepDone = false;
  ctx.isListening = false;
  ctx.closedAt = 0;
  ctx.closedSteps = 0;
  NumVal_Invalidate(&ctx.numVal[0]);
  NumVal_Invalidate(&ctx.numVal[1]);
  ctx.stepCount = 0;
  ctx.lastCpsTime = millis();
  ctx.stepsPerSec = 0;
  primeStart();
}

/* Шаг сканирования: меняем шаг VFO сканера (на выходе вернём) и
 * перезапускаем скан — спектр строится по новой сетке. */
static void changeStep(int8_t d) {
  VFO *v = &vfos[currentVfo];
  v->step = static_cast<Step>((v->step + STEP_COUNT + d) % STEP_COUNT);
  onRangeChanged();
}

/* ── Меню-оверлей: параметры приёмника, скорость и водопад ── */

static const char *menuSpeedValue(const MenuItem *item) {
  (void)item;
  static char buf[10];
  snprintf(buf, sizeof(buf), "%uus", (unsigned)scanDwellUs());
  return buf;
}

static void menuSpeedChange(const MenuItem *item, int8_t dir) {
  (void)item;
  adjustScanDelay(dir > 0 ? 1 : -1);
}

static const char *menuWaterfallValue(const MenuItem *item) {
  (void)item;
  return ctx.waterfall ? "ON" : "OFF";
}

static void menuWaterfallChange(const MenuItem *item, int8_t dir) {
  (void)item;
  setWaterfall(dir > 0);
}

static const char *menuPaletteValue(const MenuItem *item) {
  (void)item;
  return SP_PaletteName(gWaterfallPalette);
}

static void menuPaletteChange(const MenuItem *item, int8_t dir) {
  (void)item;
  SP_NextPalette(dir);
  if (ctx.waterfall) {
    WF_Reset();
    WF_Clear(UI_WATERFALL_Y);
  }
  Store_MarkDirty();
}

static const char *menuLevelMinValue(const MenuItem *item) {
  (void)item;
  static char b[8];
  snprintf(b, sizeof(b), "%d", SP_LevelMin());
  return b;
}

static const char *menuLevelMaxValue(const MenuItem *item) {
  (void)item;
  static char b[8];
  snprintf(b, sizeof(b), "%d", SP_LevelMax());
  return b;
}

static void menuLevelMinChange(const MenuItem *item, int8_t dir) {
  (void)item;
  SP_SetLevelMin((int16_t)(SP_LevelMin() + dir * 5));
  SP_Invalidate(UI_SPECTRUM_H);
  ctx.lastSpRender = 0;
  if (ctx.waterfall) {
    WF_Reset();
    WF_Clear(UI_WATERFALL_Y);
  }
}

static void menuLevelMaxChange(const MenuItem *item, int8_t dir) {
  (void)item;
  SP_SetLevelMax((int16_t)(SP_LevelMax() + dir * 5));
  SP_Invalidate(UI_SPECTRUM_H);
  ctx.lastSpRender = 0;
  if (ctx.waterfall) {
    WF_Reset();
    WF_Clear(UI_WATERFALL_Y);
  }
}

static const char *menuLevelAutoValue(const MenuItem *item) {
  (void)item;
  return "Fit";
}

static void menuLevelAutoChange(const MenuItem *item, int8_t dir) {
  (void)item;
  (void)dir;
  SP_LevelAutoFit();
  SP_Invalidate(UI_SPECTRUM_H);
  ctx.lastSpRender = 0;
  if (ctx.waterfall) {
    WF_Reset();
    WF_Clear(UI_WATERFALL_Y);
  }
}

/* Изменения полосы/модуляции/усиления скан сам не переприменяет на каждой
 * точке — пишем в чип сразу; шаг требует пересборки сетки. */
static void scanMenuApply(uint8_t id) {
  VFO *v = &vfos[currentVfo];
  BK4819_SelectChip(ctx.scanChip);
  switch (id) {
  case RADIO_MENU_STEP:
    onRangeChanged();
    break;
  case RADIO_MENU_MOD:
    BK4819_SetModulation(static_cast<ModulationType>(v->modulation));
    break;
  case RADIO_MENU_BW:
    BK4819_SetFilterBandwidth(static_cast<BK4819_FilterBandwidth_t>(v->bw));
    break;
  case RADIO_MENU_GAIN:
    BK4819_SetAGC(v->modulation != MOD_AM, v->gainIndex);
    break;
  default:
    break;
  }
}

/* Скорость — сразу после шага: до неё не нужно листать полменю. */
static const MenuItem SCAN_MENU[] = {
    RADIO_MENU_ITEM_STEP,
    {"Speed", menuSpeedValue, menuSpeedChange, 0},
    RADIO_MENU_ITEM_MOD,
    RADIO_MENU_ITEM_BW,
    RADIO_MENU_ITEM_SQL,
    RADIO_MENU_ITEM_GAIN,
    RADIO_MENU_ITEM_AFC,
    {"Min", menuLevelMinValue, menuLevelMinChange, 0},
    {"Max", menuLevelMaxValue, menuLevelMaxChange, 0},
    {"Auto", menuLevelAutoValue, menuLevelAutoChange, 0},
    {"Waterfall", menuWaterfallValue, menuWaterfallChange, 0},
    {"Palette", menuPaletteValue, menuPaletteChange, 0},
};
#define SCAN_MENU_COUNT (uint8_t)(sizeof(SCAN_MENU) / sizeof(SCAN_MENU[0]))

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
    primeStart();
  }
  vfos[currentVfo].rxF = ctx.curF;
}

static void tuneHere() {
  vfos[currentVfo].rxF = ctx.curF;
  /* фильтр диапазона (VHF/UHF/...) — без этого остаётся от VFO и сигнал
   * вне его диапазона ослаблен; внутри — кэш по band, почти бесплатно */
  BK4819_SelectFilterByFrequency(ctx.curF);
  /* pulse (не precise): полный сброс REG_30 гасит тракт и на короткой
   * выдержке RSSI ещё не поднимается. Импульс VCO_CALIB перестраивает
   * PLL, не выключая приём — на 250 мкс сигнал уже виден. */
  BK4819_TuneTo(ctx.curF, false);
}

/* После большого скачка частоты (старт диапазона, переход 470→400) RSSI/АРУ
 * выходят на режим за десятки мс. При короткой выдержке первые ~1 МГц
 * выглядят как всплеск и дают ложные подтверждения — прогреваем заранее. */
static void primeStart() {
  ctx.curF = ctx.range.start;
  vfos[currentVfo].rxF = ctx.curF;
  BK4819_SelectFilterByFrequency(ctx.curF);
  BK4819_TuneTo(ctx.curF, false);
  delay_ms(20);
}

/* Локальная проверка кандидата: сигнал из быстрого прохода мог «уехать»
 * на несколько шагов вперёд из-за инерции RSSI. Смотрим окно назад от
 * текущей частоты с длинной выдержкой и берём настоящий пик. */
static bool confirmHit(uint32_t *bestF, uint16_t *bestRssi) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  uint32_t dwell = scanDwellUs();
  uint8_t back = (uint8_t)((RSSI_LAG_US + dwell - 1) / dwell + 2);
  if (back > CONFIRM_MAX_BINS)
    back = CONFIRM_MAX_BINS;

  uint16_t best = 0, second = 0;
  uint16_t n = 0;
  uint32_t bestFreq = ctx.curF;
  for (int8_t i = (int8_t)back; i >= 0; i--) {
    uint32_t f = ctx.curF - (uint32_t)i * step;
    if (f < ctx.range.start)
      continue;
    vfos[currentVfo].rxF = f;
    BK4819_SelectFilterByFrequency(f);
    BK4819_TuneTo(f, false);
    delay_us(CONFIRM_DWELL_US);
    uint16_t r = BK4819_GetRSSI();
    n++;
    if (r > best) {
      second = best;
      best = r;
      bestFreq = f;
    } else if (r > second) {
      second = r;
    }
  }
  *bestF = bestFreq;
  *bestRssi = best;
  if (n < 3)
    return false;
  /* Пик должен быть заметно выше *второго* максимума окна. Среднее
   * размывается широким всплеском на старте диапазона; второй максимум
   * надёжнее отсекает одиночный шумовой выброс. */
  return best >= (uint16_t)(second + SQL_PEAK_MARGIN);
}

/* Один шаг сканирования: замер, решение, движение.
 * Порог = noise floor (EMA по закрытым замерам) + SQL_MARGIN.
 * Раньше порог брался из Std() и был в разы ниже RSSI — всё считалось
 * «открытым» и сканер висел в HOLD/паузах. */
static void scanStep() {
  bool listening = ctx.isListening;
  /* В HOLD приёмник уже стоит на частоте сигнала: не дёргаем VCO и не
   * щёлкаем звуком на каждой итерации главного цикла. Порог удержания
   * снят на длинной выдержке — шкала RSSI там другая, чем в быстром
   * проходе. */
  if (!listening) {
    tuneHere();
    /* После кадра экрана выдержка не короче SPI_SETTLE_US: SPI-помеха уже
     * кончилась, но RSSI-фильтр BK ещё не восстановился. */
    uint16_t dwell = scanDwellUs();
    if (ctx.guardUs > dwell)
      dwell = ctx.guardUs;
    ctx.guardUs = 0;
    delay_us(dwell);
  } else {
    vfos[currentVfo].rxF = ctx.curF;
    uint16_t dwell = CONFIRM_DWELL_US;
    if (ctx.guardUs > dwell)
      dwell = ctx.guardUs;
    ctx.guardUs = 0;
    delay_us(dwell);
  }
  uint16_t rssi = BK4819_GetRSSI();
  ctx.lastRssi = rssi;
  if (ctx.dbg)
    ctx.dbgSteps++;

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

  bool open = listening ? (rssi + SQL_HYST >= ctx.holdThr) : (rssi >= thr);
  if (ctx.curF % 1300000 == 0)
    open = false; /* гармоники ПЧ — мусор */
  if (Settings_Get(SET_SKIPGARBAGEFREQUENCIES) && ctx.curF % 6500 == 0)
    open = false; /* сетка 65 кГц: мусор/помехи */

  /* чёрный (и белый вне мониторинга) список: не открываемся и не учим
   * порог по сильному сигналу */
  bool skipped = false;
  if (open && !ctx.isListening &&
      LOOT_IsSkipped(ctx.curF, StepFrequencyTable[vfos[currentVfo].step] / 2,
                     ctx.monitorMode)) {
    open = false;
    skipped = true;
  }

  uint32_t measF = ctx.curF;
  uint16_t hitRssi = rssi;
  if (open && !ctx.isListening) {
    uint32_t bestF;
    uint16_t bestRssi;
    open = confirmHit(&bestF, &bestRssi);
    if (open) {
      ctx.curF = bestF;
      hitRssi = bestRssi;
      ctx.holdThr = bestRssi > 2 * SQL_HYST ? (uint16_t)(bestRssi - 2 * SQL_HYST)
                                           : bestRssi;
      tuneHere(); /* слушаем уже точную частоту */
    } else if (ctx.floorRssi < 0xFFFF - SQL_MARGIN) {
      ctx.floorRssi++; /* ложное срабатывание — порог выше */
    }
    /* чёрный список проверяем и по уточнённой частоте */
    if (open &&
        LOOT_IsSkipped(ctx.curF, StepFrequencyTable[vfos[currentVfo].step] / 2,
                       ctx.monitorMode)) {
      open = false;
      skipped = true;
    }
    /* TEMP: тест помех от дисплея — фиксируем каждое подтверждённое
     * открытие и идём дальше, не залипая в HOLD. */
    if (open && ctx.dbg) {
      uart_printf("HIT %lu %u\r\n", (unsigned long)(ctx.curF / 100),
                  (unsigned)hitRssi);
      ctx.dbgHits++;
      open = false;
      skipped = true;
    }
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

  Measurement m = {.f = measF, .rssi = rssi, .noise = 0, .open = open};
  SP_AddPoint(&m);
  ctx.stepCount++;

  uint32_t now = millis();
  if (open) {
    if (!ctx.isListening) {
      ctx.isListening = true;
      uint32_t tmo = Settings_ScanTimeoutMs(SET_SQOPENEDTIMEOUT);
      ctx.listenUntil = tmo == 0xFFFFFFFFu ? tmo : now + tmo;
    }
    ctx.closedAt = 0; /* сигнал вернулся — Stay time не действует */
    /* учёт находки: добавить/обновить, копить длительность */
    LOOT_Hit(ctx.curF, hitRssi, vfos[currentVfo].modulation, vfos[currentVfo].bw,
             vfos[currentVfo].radio);
    setAudio(true);
    if (ctx.listenUntil != 0xFFFFFFFFu &&
        (int32_t)(now - ctx.listenUntil) >= 0) {
      /* не закрылось за Listen t/o: либо несущая, либо порог ниже шума —
       * поднимаем порог выше текущего уровня, чтобы не залипнуть */
      ctx.floorRssi = rssi;
      ctx.isListening = false;
      setAudio(false);
      LOOT_Standby();
      advance();
    }
  } else if (ctx.isListening) {
    /* Stay time: шумодав закрылся, но короткие паузы в речи не должны
     * гнать скан дальше. Стоим на частоте с выключенным звуком; если
     * сигнал вернулся раньше — ветка open снимет паузу. */
    uint32_t stay = Settings_ScanTimeoutMs(SET_SQCLOSEDTIMEOUT);
    if (!ctx.closedAt) {
      ctx.closedAt = now ? now : 1; /* 0 — «ещё не закрыт» */
      setAudio(false);
      LOOT_Standby();
    }
    if (stay != 0xFFFFFFFFu &&
        (int32_t)(now - ctx.closedAt) >= (int32_t)stay) {
      ctx.closedAt = 0;
      ctx.isListening = false;
      advance();
    }
  } else {
    advance();
  }
}

/* Закончить удержание и идти дальше (после занесения в список) */
static void releaseHold() {
  if (ctx.isListening) {
    ctx.isListening = false;
    ctx.closedAt = 0;
    setAudio(false);
  }
  LOOT_Standby();
  advance();
}

/* ─── Список находок под спектром ─── */

static uint16_t lootRowIdx[UI_LOOT_ROWS];
static uint16_t lootRowAge[UI_LOOT_ROWS];

/* Свежие находки сверху: сортируем индексы по «сколько назад», сам loot не
 * трогаем (его порядок — общий для списка находок и хранилища). */
static uint8_t collectRecentLoot(void) {
  uint16_t n = LOOT_Size();
  uint16_t nowS = (uint16_t)(millis() / 1000);
  uint8_t cnt = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const Loot *it = LOOT_Item(i);
    uint16_t age = (uint16_t)(nowS - it->lastTimeOpen);
    uint8_t p = 0;
    while (p < cnt && lootRowAge[p] <= age)
      p++;
    if (p >= UI_LOOT_ROWS)
      continue;
    if (cnt < UI_LOOT_ROWS)
      cnt++;
    for (uint8_t j = (uint8_t)(cnt - 1); j > p; --j) {
      lootRowAge[j] = lootRowAge[j - 1];
      lootRowIdx[j] = lootRowIdx[j - 1];
    }
    lootRowAge[p] = age;
    lootRowIdx[p] = i;
  }
  return cnt;
}

static TextStyle lootRowStyle(const Loot *it) {
  Color c = it->blacklist ? C_GRAY
            : it->whitelist ? C_GREEN
            : it->open      ? C_YELLOW
                            : C_WHITE;
  return text_style(c, C_BLACK, POS_L, F_M);
}

static void drawLootList(void) {
  uint16_t nowS = (uint16_t)(millis() / 1000);
  if (!ctx.listSel) {
    uint8_t cnt = collectRecentLoot();
    for (uint8_t r = 0; r < UI_LOOT_ROWS; ++r) {
      uint16_t y =
          (uint16_t)(UI_LOOT_Y + r * UI_LOOT_ROW_H + FontAscent(F_M));
      if (r >= cnt) {
        /* Пустая строка прячет слот: список короче экрана. */
        PrintfT(UI_LEFT_X, y, text_style(C_WHITE, C_BLACK, POS_L, F_M), "%s",
                "");
        continue;
      }
      const Loot *it = LOOT_Item(lootRowIdx[r]);
      char fb[16], age[5];
      fmt_mhz5(fb, sizeof(fb), it->f);
      fmt_duration(age, sizeof(age), (uint16_t)(nowS - it->lastTimeOpen));
      char mark =
          it->blacklist ? '-' : it->whitelist ? '+' : it->open ? '!' : ' ';
      PrintfT(UI_LEFT_X, y, lootRowStyle(it), "%c%s %s %+4d", mark, fb, age,
              (int)it->peak - 160);
    }
    return;
  }

  /* Навигация: окно listTop..listTop+ROWS, выбранная строка — инверсией и
   * меткой '>'. Список отсортирован при входе (LOOT_Sort), поэтому индекс
   * находки = listSel-1. */
  uint16_t n = LOOT_Size();
  for (uint8_t r = 0; r < UI_LOOT_ROWS; ++r) {
    uint16_t y = (uint16_t)(UI_LOOT_Y + r * UI_LOOT_ROW_H + FontAscent(F_M));
    uint16_t idx = (uint16_t)(ctx.listTop + r);
    if (idx >= n) {
      PrintfT(UI_LEFT_X, y, text_style(C_WHITE, C_BLACK, POS_L, F_M), "%s",
              "");
      continue;
    }
    const Loot *it = LOOT_Item(idx);
    bool sel = (uint16_t)(idx + 1) == ctx.listSel;
    char fb[16], age[5];
    fmt_mhz5(fb, sizeof(fb), it->f);
    fmt_duration(age, sizeof(age), (uint16_t)(nowS - it->lastTimeOpen));
    char mark =
        it->blacklist ? '-' : it->whitelist ? '+' : it->open ? '!' : ' ';
    TextStyle st = sel ? text_style(C_BLACK, C_CYAN, POS_L, F_M)
                       : lootRowStyle(it);
    PrintfT(UI_LEFT_X, y, st, "%c%c%s %s %+4d", sel ? '>' : ' ', mark, fb,
            age, (int)it->peak - 160);
  }
}

/* Водопад занимает ту же зону — список находок прячем. */
static void hideLootList(void) {
  for (uint8_t r = 0; r < UI_LOOT_ROWS; ++r) {
    uint16_t y = (uint16_t)(UI_LOOT_Y + r * UI_LOOT_ROW_H + FontAscent(F_M));
    PrintfT(UI_LEFT_X, y, text_style(C_WHITE, C_BLACK, POS_L, F_M), "%s", "");
  }
}

/* ─── Навигация по списку находок ───
 * listSel = 0 — текущий скан, 1..N — запись loot (индекс = listSel-1).
 * Стрелки ходят по кругу: за последней записью и перед первой — возврат
 * к сканированию. На выбранной записи скан стоит (scanning=false), приёмник
 * настроен на неё; кнопки списка повторяют экран находок. */

static void msgSet(const char *text) {
  snprintf(ctx.msg, sizeof(ctx.msg), "%s", text);
  ctx.msgUntil = millis() + 1500;
  ctx.redraw = true;
}

static void msgSetSaved(const char *tag, int16_t n) {
  if (n < 0) {
    msgSet("ERR");
    return;
  }
  char b[16];
  snprintf(b, sizeof(b), "%s %d", tag, (int)n);
  msgSet(b);
}

static void clampListSel(void) {
  uint16_t n = LOOT_Size();
  if (!ctx.listSel || !n) {
    ctx.listSel = 0;
    ctx.listTop = 0;
    return;
  }
  if (ctx.listSel > n)
    ctx.listSel = n;
  uint16_t idx = (uint16_t)(ctx.listSel - 1);
  if (idx < ctx.listTop)
    ctx.listTop = idx;
  if (idx >= (uint16_t)(ctx.listTop + UI_LOOT_ROWS))
    ctx.listTop = (uint16_t)(idx - UI_LOOT_ROWS + 1);
  uint16_t maxTop = n > UI_LOOT_ROWS ? (uint16_t)(n - UI_LOOT_ROWS) : 0;
  if (ctx.listTop > maxTop)
    ctx.listTop = maxTop;
}

static void sortList(uint8_t t) {
  uint16_t idx = ctx.listSel ? (uint16_t)(ctx.listSel - 1) : 0;
  const Loot *cur = LOOT_Item(idx);
  uint32_t f = cur ? cur->f : 0;
  if (t == ctx.sortIdx)
    ctx.sortRev = !ctx.sortRev;
  else {
    ctx.sortIdx = t;
    ctx.sortRev = false;
  }
  LOOT_Sort(LOOT_SORTERS[t], ctx.sortRev);
  if (f) {
    int16_t i = LOOT_IndexOf(LOOT_Get(f));
    ctx.listSel = i < 0 ? 0 : (uint16_t)(i + 1);
  }
  ctx.listTop = 0;
  clampListSel();
  ctx.redraw = true;
}

/* Настроить приём на выбранную находку. VFO-снимок скана сохранён в
 * listSaved; на выходе resumeScan вернёт модуляцию/полосу/кристалл. */
static void tuneLoot(const Loot *it) {
  if (!it)
    return;
  currentVfo = ctx.scanVfo;
  VFO *v = &vfos[ctx.scanVfo];
  v->rxF = it->f;
  v->modulation = static_cast<ModulationType>(it->modulation & 7);
  v->bw = it->bw;
  v->radio = static_cast<Radio>(it->radio <= 2 ? it->radio : 0);
  v->radioManual = true;
  Radio_TuneTo(it->f, true);
  ctx.redraw = true;
}

static void resumeScan(void) {
  bool wasHold = ctx.listWasHold;
  ctx.listSel = 0;
  ctx.listTop = 0;
  ctx.listWasHold = false;
  ctx.msg[0] = 0;
  ctx.scanning = true;
  ctx.isListening = false;
  ctx.closedAt = 0;
  /* ctx.dacOn после Radio_Update в списке уже рассинхронизирован, поэтому
   * не setAudio(), а принудительное закрытие: сначала шумодав, потом
   * перестройка. precise-настройка Vfo_Restore звук приоткрывает — снова
   * закрываем. */
  ctx.dacOn = false;
  Radio_ScanAudio(false);
  Vfo_Restore(ctx.listSaved, true);
  currentVfo = ctx.scanVfo;
  BK4819_SelectChip(ctx.scanChip);
  BK4819_SetAFC(0);
  ctx.dacOn = false;
  Radio_ScanAudio(false);
  if (wasHold)
    advance(); /* не залипать на той же несущей после HOLD */
  ctx.stepCount = 0;
  ctx.lastCpsTime = millis();
  Mode_SetDisplayRxGate(true); /* замеры снова под защитой от SPI-помех */
  ctx.spiGuard = true; /* первый замер после кадра — с паузой */
  ctx.redraw = true;
}

static void listMove(int8_t d) {
  uint16_t n = LOOT_Size();
  if (!n) {
    if (ctx.listSel)
      resumeScan();
    return;
  }
  if (!ctx.listSel) {
    /* Вход в список: снимок режима скана, чтобы вернуть его на выходе. */
    ctx.listSaved = Vfo_Capture(ctx.scanVfo);
    ctx.listWasHold = ctx.isListening;
    ctx.listSel = d > 0 ? 1 : n;
    ctx.scanning = false;
    ctx.isListening = false;
    ctx.closedAt = 0;
    ctx.dacOn = false;
    Radio_ScanAudio(false);
    LOOT_Standby();
    Mode_SetDisplayRxGate(false); /* слушаем выбранную — без провалов звука */
    if (ctx.waterfall)
      setWaterfall(false); /* список должен быть виден */
    LOOT_Sort(LOOT_SORTERS[ctx.sortIdx], ctx.sortRev);
    ctx.listTop = 0;
  } else if (d > 0) {
    if (ctx.listSel < n)
      ctx.listSel++;
    else {
      resumeScan();
      return;
    }
  } else {
    if (ctx.listSel > 1)
      ctx.listSel--;
    else {
      resumeScan();
      return;
    }
  }
  clampListSel();
  tuneLoot(LOOT_Item((uint16_t)(ctx.listSel - 1)));
  ctx.redraw = true;
}

static void deleteSelectedLoot(void) {
  if (!ctx.listSel)
    return;
  LOOT_Remove((uint16_t)(ctx.listSel - 1));
  uint16_t n = LOOT_Size();
  if (!n) {
    resumeScan();
    return;
  }
  if (ctx.listSel > n)
    ctx.listSel = n;
  clampListSel();
  tuneLoot(LOOT_Item((uint16_t)(ctx.listSel - 1)));
  ctx.redraw = true;
}

/* Выбранную находку — в канал (та же CH_Add, что у формы канала). */
static void saveSelectedLootToChannel(void) {
  const Loot *it = LOOT_Item((uint16_t)(ctx.listSel - 1));
  if (!it)
    return;
  CH ch;
  memset(&ch, 0, sizeof(ch));
  ch.f = it->f;
  ch.modulation = (uint8_t)(it->modulation & 7);
  ch.bw = (uint8_t)(it->bw & 15);
  ch.radio = (uint8_t)(it->radio & 3);
  ch.scanlists = CH_GetScanlistMask();
  msgSet(CH_Add(&ch) ? "SAVED" : "ADD ERR");
}

/* Как MENU на экране находок: выбранную частоту в VFO и выйти. */
static void captureSelectedLoot(void) {
  const Loot *it = LOOT_Item((uint16_t)(ctx.listSel - 1));
  if (!it)
    return;
  currentVfo = ctx.savedVfo;
  vfos[currentVfo].modulation =
      static_cast<ModulationType>(it->modulation & 7);
  vfos[currentVfo].bw = it->bw;
  Radio_TuneTo(it->f, true);
  ctx.captureOnExit = true;
  Mode_Switch(MODE_VFO);
}

class ScannerMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("SCAN");
    lv_port_set_direct_areas(true);
    lv_port_set_text_bottom(ctx.waterfall ? UI_WATERFALL_Y - 3
                                          : UI_LEVEL_Y - 1);
    ctx.savedVfo = currentVfo;
    ctx.savedFreq = vfos[currentVfo].rxF;
    Radio_Init();
    RadioMenu_SetApply(scanMenuApply);
    /* Внутри сканера VFO гоняется по частотам; в сохранение должен попасть
     * снимок на входе, пока MENU не заберёт частоту обратно в VFO. */
    Store_PinRadio();
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
    ctx.scanChip = vfos[currentVfo].radio == RADIO_BK4819B ? 1 : 0;
    BK4819_SelectChip(ctx.scanChip);
    /* TEMP: тест помех — погасить вторую микросхему BK и SI4732. */
    if (ctx.dbg) {
      if (ctx.dbgFlags & 1) {
        BK4819_SelectChip(1);
        BK4819_WriteRegister(BK4819_REG_30, 0x0000);
        BK4819_WriteRegister(0x37, 0x1D00);
        BK4819_SelectChip(ctx.scanChip);
      }
      if (ctx.dbgFlags & 2)
        SI47XX_PowerDown();
    }
    /* Полоса канала из VFO: после WFM в REG_43 остаётся широкая полоса,
     * и скан «слышит» соседние каналы. AFC на время скана выключен, иначе
     * слабую несущую уводит с сетки. */
    BK4819_SetFilterBandwidth(
        static_cast<BK4819_FilterBandwidth_t>(vfos[ctx.scanVfo].bw));
    BK4819_SetAFC(0);
    onRangeChanged(); /* включает primeStart: прогрев на range.start */
    ctx.dacOn = false;
    Radio_ScanAudio(false);
    ctx.captureOnExit = false;
    ctx.listSel = 0;
    ctx.listTop = 0;
    ctx.listWasHold = false;
    ctx.msg[0] = 0;
    ctx.sortIdx = LOOT_SORT_TIME;
    ctx.sortRev = false;
    ctx.scanning = true;
    /* Первый кадр после входа рисуем сразу (wasMeasuring мы ещё не замеряли),
     * дальше — на границе прохода и раз в 500 мс. */
    ctx.wasMeasuring = false;
    ctx.paintHeld = false;
    ctx.redraw = false;
    ctx.spiGuard = false;
    ctx.guardUs = 0;
    ctx.closedAt = 0;
    ctx.lastFrameMs = millis();
    Mode_SetDisplayRxGate(true);
  }

  void update() override {
    if (ctx.dbg && (int32_t)(millis() - ctx.dbgUntil) >= 0) {
      uart_printf("SCANDONE hits=%u steps=%lu\r\n", (unsigned)ctx.dbgHits,
                  (unsigned long)ctx.dbgSteps);
      ctx.dbg = false;
      st7789_set_mute(false);
      Mode_SetPaint(true);
      Mode_Switch(MODE_VFO);
      return;
    }
    NumVal_Update(&ctx.numVal[0]);
    NumVal_Update(&ctx.numVal[1]);
    bool editing = NumVal_IsEditing(&ctx.numVal[0]) || NumVal_IsEditing(&ctx.numVal[1]);
    if (ctx.listSel) {
      Radio_Update(); /* шумодав/звук выбранной находки */
      return;         /* скан стоит, пока список выбран */
    }
    if (ctx.scanning && !editing) {
      /* После кадра экрана первый замер — с паузой на восстановление тракта. */
      ctx.guardUs = ctx.spiGuard ? SPI_SETTLE_US : 0;
      ctx.spiGuard = false;
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
    bool menuOpen = MenuOverlay_IsOpen();
    bool editing = NumVal_IsEditing(&ctx.numVal[0]) ||
                   NumVal_IsEditing(&ctx.numVal[1]);

    /* Во время прохода кадры редкие: каждый flush SPI1 дисплея даёт
     * помеху в 437–441 МГц, и замеры становятся грязнее. Кадр обновляем на
     * границе прохода (sweepDone), по клавише, изменившей экран (redraw), и
     * не реже раза в 500 мс — иначе частота и спектр на экране выглядят
     * «зависшими». Один кадр на переходе в замер — чтобы после HOLD/меню/
     * паузы не остался старый экран. */
    bool measuring = ctx.scanning && !ctx.isListening && !menuOpen && !editing;
    bool measuringStart = measuring && !ctx.wasMeasuring;
    ctx.wasMeasuring = measuring;
    uint32_t now = millis();
    if (measuring && !ctx.sweepDone && !measuringStart && !ctx.redraw &&
        now - ctx.lastFrameMs < 500)
      return;

    /* На открытом шумодаве экран больше не обновляем: каждый flush SPI1
     * дисплея даёт помеху в 437–441 МГц, её слышно в динамике. Кадр HOLD
     * рисуем один раз при входе в прослушивание. Исключение — график
     * уровня (Level in VFO): он добавляет по одному столбцу прямым DMA и
     * показывает изменение сигнала во времени. */
    bool holding = ctx.isListening && !menuOpen && !editing;
    bool firstHold = false;
    if (holding) {
      if (ctx.paintHeld) {
        if (!ctx.waterfall)
          levelTick(now);
        return;
      }
      ctx.paintHeld = true;
      firstHold = true;
    } else {
      ctx.paintHeld = false;
    }

    ctx.lastFrameMs = now;
    ctx.redraw = false;

    STATUSLINE_render();

    NumVal_Render(&ctx.numVal[0]);
    NumVal_Render(&ctx.numVal[1]);
    if (ctx.menuWasOpen && !menuOpen) {
      /* Меню закрылось: его пиксели в зоне спектра нужно заменить. */
      SP_Invalidate(UI_SPECTRUM_H);
      ctx.lastSpRender = 0;
    }
    ctx.menuWasOpen = menuOpen;

    /* Спектр — на границе прохода, по прямому запросу (смена
     * диапазона/шага/уровней, кадр HOLD) и не реже раза в 500 мс, вместе
     * с текстом: SP_Render дорисовывает только изменившиеся столбцы, так
     * что столбцы появляются по мере прохода. Иначе при медленном проходе
     * (Scan delay 1.8 мс, 400–470 МГц) спектр «замирает» на десятки секунд.
     * После кадра следующий замер — с паузой на восстановление тракта. */
    if (ctx.sweepDone || ctx.lastSpRender == 0 || firstHold ||
        (!ctx.listSel && !measuring && now - ctx.lastSpRender >= 300)) {
      BK4819_RxSuspend(); /* кадр не должен попадать в приём */
      SP_Render(&ctx.range, UI_SPECTRUM_Y, UI_SPECTRUM_H);
      BK4819_RxResume();
      ctx.lastSpRender = now;
      ctx.spiGuard = true; /* следующий замер — после паузы на восстановление */
      /* прямой DMA затёр только полосу спектра — меню перерисует пересечение */
      MenuOverlay_InvalidateBand(UI_SPECTRUM_Y, UI_SPECTRUM_Y + UI_SPECTRUM_H - 1);
    }
    if (ctx.sweepDone) { /* строка водопада — раз за полный проход */
      ctx.sweepDone = false;
      if (ctx.waterfall) {
        BK4819_RxSuspend();
        WF_Render(UI_WATERFALL_Y, true);
        BK4819_RxResume();
      }
      SP_Begin(); /* пересчитать автоуровни по завершённому проходу */
    }

    /* шаг между полями границ; при вводе не прячем — принятая граница
     * остаётся видимой рядом с редактором */
    uint32_t stp = StepFrequencyTable[vfos[currentVfo].step];
    static char stepBuf[10];
    fmt_step(stepBuf, sizeof(stepBuf), stp);
    PrintfT(LCD_XCENTER, UI_RANGE_BASE,
            text_style(C_YELLOW, C_BLACK, POS_C, F_S), "%s", stepBuf);

    /* Строка 1: состояние | частота (скана или выбранной) | скорость/номер. */
    bool browsing = ctx.listSel != 0;
    const Loot *selIt =
        browsing ? LOOT_Item((uint16_t)(ctx.listSel - 1)) : nullptr;
    uint32_t dispF = selIt ? selIt->f : ctx.curF;
    const char *state = browsing ? "LOOT"
                        : !ctx.scanning ? "PAUSE"
                        : (ctx.isListening ? "HOLD" : "SCAN");
    Color stateCol = browsing ? C_CYAN
                     : !ctx.scanning ? C_GRAY
                     : (ctx.isListening ? C_YELLOW : C_GREEN);
    PrintfT(UI_LEFT_X, UI_INFO1_BASE,
            text_style(stateCol, C_BLACK, POS_L, F_M), "%s", state);
    PrintfT(LCD_XCENTER, UI_INFO1_BASE,
            text_style(C_WHITE, C_BLACK, POS_C, F_M), "%u.%05u",
            (unsigned)(dispF / 100000), (unsigned)(dispF % 100000));
    if (browsing) {
      PrintfT(UI_RIGHT_X, UI_INFO1_BASE,
              text_style(C_YELLOW, C_BLACK, POS_R, F_M), "%u/%u",
              (unsigned)ctx.listSel, (unsigned)LOOT_Size());
    } else {
      PrintfT(UI_RIGHT_X, UI_INFO1_BASE,
              text_style(C_YELLOW, C_BLACK, POS_R, F_M), "%u/s",
              (unsigned)ctx.stepsPerSec);
    }

    /* Строка 2: находка/выдержка/счётчик или детали выбранной записи. */
    if (browsing && selIt) {
      if (ctx.msg[0] && (int32_t)(ctx.msgUntil - millis()) > 0) {
        PrintfT(UI_LEFT_X, UI_INFO2_BASE,
                text_style(C_YELLOW, C_BLACK, POS_L, F_S), "%s", ctx.msg);
      } else {
        ctx.msg[0] = 0;
        char dur[5];
        fmt_duration(dur, sizeof(dur), selIt->duration);
        PrintfT(UI_LEFT_X, UI_INFO2_BASE,
                text_style(C_WHITE, C_BLACK, POS_L, F_S), "%s %s %+ddBm",
                MOD_NAMES[selIt->modulation & 7],
                RADIO_NAMES[selIt->radio > 2 ? 2 : selIt->radio],
                (int)selIt->peak - 160);
        PrintfT(LCD_XCENTER, UI_INFO2_BASE,
                text_style(C_GRAY, C_BLACK, POS_C, F_S), "dur %s", dur);
      }
      PrintfT(UI_RIGHT_X, UI_INFO2_BASE,
              text_style(C_GRAY, C_BLACK, POS_R, F_S), "by %s%s",
              LOOT_SORT_NAMES[ctx.sortIdx], ctx.sortRev ? " rev" : "");
    } else {
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
              (unsigned)scanDwellUs());
      PrintfT(UI_RIGHT_X, UI_INFO2_BASE,
              text_style(ctx.monitorMode ? C_GREEN : C_GRAY, C_BLACK, POS_R,
                         F_S),
              "L%u%s", (unsigned)LOOT_Size(), ctx.monitorMode ? " MON" : "");
    }

    /* Низ экрана: список находок вместо водопада (он по long SIDE1) и
     * график уровня. Список — LVGL-лейблы (зона расширена до графика),
     * график — прямым DMA в нижней полосе. */
    if (!ctx.waterfall) {
      drawLootList();
      if (holding) /* история — только пока слушаем */
        levelTick(now);
    }

    /* Текст/меню — LVGL: реальный SPI-flush делает lv_timer_handler, его
     * гасит главный цикл (Mode_DisplayRxGate). */
    MenuOverlay_Render();
  }

  void exit() override {
    MenuOverlay_Close();
    RadioMenu_SetApply(nullptr);
    ctx.menuWasOpen = false;
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
    Store_UnpinRadio();
    lv_port_set_direct_areas(false);
    Mode_SetDisplayRxGate(false);
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (MenuOverlay_IsOpen()) {
      if (MenuOverlay_Key(k, state))
        return true;
      LOOTLIST_OpenFrom(MODE_SCANNER); /* STAR закрыл меню */
      return true;
    }

    /* пока идёт ввод — все клавиши редактору (сначала начало, затем конец) */
    if (NumVal_t *ev = editingNumVal()) {
      if (NumVal_Key(ev, k, state))
        return true;
      return false;
    }

    /* Выбран пункт списка находок: работаем с ним как на экране находок.
     * 0 — текущий скан (обычные клавиши сканера), 1..N — записи loot. */
    if (ctx.listSel) {
      if (state == KEY_LONG_PRESSED) {
        switch (k) {
        case KEY_5: /* whitelist-находки → каналы (как длинный 5 в loot) */
          msgSetSaved("SAVED", CH_SaveWhitelistFromLoot());
          return true;
        case KEY_8: /* blacklist-находки → каналы */
          msgSetSaved("SAVED", CH_SaveBlacklistFromLoot());
          return true;
        case KEY_9: /* чёрный список каналов → loot */
          msgSetSaved("LOADED", CH_LoadBlacklistToLoot());
          return true;
        case KEY_SIDE2: /* выбранную находку — в канал */
          saveSelectedLootToChannel();
          return true;
        case KEY_0: /* очистить весь список и вернуться к скану */
          LOOT_Clear();
          resumeScan();
          return true;
        default:
          /* пока список выбран, меню/водопад/сброс пиков не трогаем */
          return true;
        }
      }
      if (state == KEY_LONG_PRESSED_CONT &&
          (k == KEY_UP || k == KEY_DOWN)) {
        listMove(k == KEY_UP ? -1 : +1);
        return true;
      }
      if (state != KEY_RELEASED)
        return true;

      switch (k) {
      case KEY_UP:
        listMove(-1);
        return true;
      case KEY_DOWN:
        listMove(+1);
        return true;
      case KEY_1:
        sortList(LOOT_SORT_TIME);
        return true;
      case KEY_2:
        sortList(LOOT_SORT_DUR);
        return true;
      case KEY_3:
        sortList(LOOT_SORT_BL);
        return true;
      case KEY_4:
        sortList(LOOT_SORT_F);
        return true;
      case KEY_SIDE1:
        LOOT_ToggleBlacklist(LOOT_Item((uint16_t)(ctx.listSel - 1)));
        ctx.redraw = true;
        return true;
      case KEY_SIDE2:
        LOOT_ToggleWhitelist(LOOT_Item((uint16_t)(ctx.listSel - 1)));
        ctx.redraw = true;
        return true;
      case KEY_0:
        deleteSelectedLoot();
        return true;
      case KEY_MENU: /* выбранную частоту — в VFO и выйти (как в loot) */
        captureSelectedLoot();
        return true;
      case KEY_EXIT: /* назад к сканированию */
        resumeScan();
        return true;
      default:
        return true;
      }
    }

    if (state == KEY_LONG_PRESSED) {
      switch (k) {
      case KEY_MENU: /* меню-оверлей: параметры, скорость и водопад */
        MenuOverlay_Open(SCAN_MENU, SCAN_MENU_COUNT, 0, UI_STATUS_H,
                         LCD_WIDTH / 2, MENU_OVERLAY_BOX_H, true);
        return true;
      case KEY_SIDE1: /* водопад вкл/выкл (по умолчанию — список находок) */
        setWaterfall(!ctx.waterfall);
        return true;
      case KEY_5: /* сброс пикового удержания */
        SP_ResetPeaks();
        ctx.redraw = true;
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
    case KEY_STAR: /* список находок */
      LOOTLIST_OpenFrom(MODE_SCANNER);
      return true;
    case KEY_SIDE1: /* в чёрный список и дальше */
      if (gLastActiveLoot) {
        LOOT_BlacklistLast();
        releaseHold();
        ctx.redraw = true; /* метка в строке находки — сразу, не через 500 мс */
      }
      return true;
    case KEY_SIDE2: /* в белый список и дальше */
      if (gLastActiveLoot) {
        LOOT_WhitelistLast();
        releaseHold();
        ctx.redraw = true;
      }
      return true;
    case KEY_5: /* ввод границ: сначала начало, затем конец */
      beginRangeEdit();
      return true;
    case KEY_1: /* выдержка на частоте + */
      adjustScanDelay(1);
      ctx.redraw = true;
      return true;
    case KEY_7: /* выдержка на частоте − */
      adjustScanDelay(-1);
      ctx.redraw = true;
      return true;
    case KEY_3: /* шаг сканирования + */
      changeStep(+1);
      ctx.redraw = true;
      return true;
    case KEY_9: /* шаг сканирования − */
      changeStep(-1);
      ctx.redraw = true;
      return true;
    case KEY_UP: /* в список находок (0 — текущий скан, 1..N — находки) */
      listMove(-1);
      return true;
    case KEY_DOWN:
      listMove(+1);
      return true;
    default:
      break;
    }
    return false;
  }
};

ScannerMode scannerModeInstance;
Mode &MODE_SCANNER = scannerModeInstance;

/* TEMP: тест помех от дисплея. */
void SCANNER_DebugStart(uint32_t secs, bool mute, uint8_t flags) {
  ctx.dbg = true;
  ctx.dbgFlags = flags;
  ctx.dbgHits = 0;
  ctx.dbgSteps = 0;
  ctx.dbgUntil = millis() + secs * 1000u;
  ctx.scanning = true;
  ctx.isListening = false;
  ctx.range.start = 400 * MHZ;
  ctx.range.end = 470 * MHZ;
  vfos[currentVfo].step = STEP_25_0kHz;
  st7789_set_mute(mute);
  Mode_SetPaint(false); /* замер: дисплей не должен добавлять помеху */
  Mode_Switch(MODE_SCANNER);
  uart_printf("SCANDBG %u..%u sec=%lu mute=%u flags=%u\r\n",
              (unsigned)(ctx.range.start / MHZ),
              (unsigned)(ctx.range.end / MHZ), (unsigned long)secs,
              (unsigned)mute, (unsigned)flags);
}
