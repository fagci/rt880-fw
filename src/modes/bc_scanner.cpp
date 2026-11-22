/* BC-сканер — вещательный КВ (короткие волны) по каналам.
 *
 * Отличие от универсального сканера (scanner.cpp): сканируются не
 * произвольные границы, а вещательные КВ-диапазоны целиком, по сетке
 * каналов 5 кГц. Приём — SI4732 в AM (ниже 18 МГц BK4819 не принимает),
 * поэтому и порог, и прослушивание строятся вокруг RSSI SI4732, а не
 * шумодава BK.
 *
 * Алгоритм:
 *  - первый проход по диапазону — калибровка (CAL): сканируем все каналы,
 *    запоминаем минимальный RSSI как шумовой пол; на сигналы не встаём;
 *  - дальше порог = пол + BC_SQL_MARGIN; кандидат подтверждается повторным
 *    замером через BC_CONFIRM_MS (защита от импульсной помехи);
 *  - подтверждённый канал — HOLD: звук открыт, находка пишется в loot;
 *    RSSI во время прослушивания не читается (I2C щёлкает), HOLD 15 с;
 *  - каждый завершённый проход обновляет шумовой пол и рисует строку
 *    водопада; спектр показывает занятость каналов диапазона.
 *
 * Клавиши:
 *   UP/DOWN       — канал +/- (переводит в PAUSE и включает звук);
 *   SIDE1/SIDE2   — следующий/предыдущий КВ-диапазон;
 *   длинный #     — пауза/продолжить скан;
 *   длинный *     — выдержка на канале (скорость скана);
 *   длинный SIDE1 — водопад вкл/выкл;
 *   длинный SIDE2 — полоса AM (6/4/3/2 кГц);
 *   длинный 5     — сброс пиков спектра;
 *   MENU          — текущий канал в VFO и выход;
 *   длинный MENU  — список находок;
 *   EXIT          — выход в VFO.
 */
#include "bc_scanner.h"
#include "bc_bands.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/si473x.h"
#include "../driver/st7789.h"
#include "../helper/log.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../helper/unit_fmt.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/lv_port.h"
#include "../ui/screen_layout.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "loot_screen.h"
#include "mode.h"
#include "printf.h"
#include "vfo.h"
#include <stdio.h>
#include <string.h>


/* Выдержка на канале до чтения RSSI, мс. Малые значения — быстрый обзор,
 * большие — точнее установка АРУ/антенного контура SI4732. */
static const uint8_t BC_SETTLE_MS[] = {15, 25, 40, 60, 100};
#define BC_SETTLE_N (sizeof(BC_SETTLE_MS) / sizeof(BC_SETTLE_MS[0]))

#define BC_SQL_MARGIN 10 /* порог над шумовым полом, дБ (RSSI SI: 1 ед. = 1 дБ) */
#define BC_CONFIRM_MS 60
#define BC_HOLD_MAX_MS 15000 /* HOLD без RSSI: слушаем 15 с, потом дальше */
#define BC_PAUSE_POLL_MS 1000

static const SI47XX_FilterBW BC_BW[] = {SI47XX_BW_6_kHz, SI47XX_BW_4_kHz,
                                        SI47XX_BW_3_kHz, SI47XX_BW_2_kHz};
#define BC_BW_N (sizeof(BC_BW) / sizeof(BC_BW[0]))

typedef enum {
  BC_CAL,     /* первый проход: калибровка шумового пола */
  BC_SCAN,    /* поиск сигнала */
  BC_CONFIRM, /* кандидат: повторный замер */
  BC_HOLD,    /* слушаем найденный канал */
  BC_PAUSE,   /* ручное переключение каналов */
} BcState;

static const char *const BC_STATE_NAMES[] = {"CAL", "SCAN", "CONFIRM", "HOLD",
                                             "PAUSE"};

static uint8_t s_band = 6; /* 31m */
static BcState s_state = BC_CAL;
static uint16_t s_chan;
static uint16_t s_floor;    /* шумовой пол, дБ */
static uint16_t s_thr;      /* порог открытия */
static uint16_t s_sweepMin; /* минимум RSSI за текущий проход */
static uint8_t s_settleIdx = 2; /* 40 мс: SI4732 успевает перестроиться */
static uint8_t s_bwIdx;
static uint16_t s_rssi;
static uint32_t s_nextAt;
static uint32_t s_stepCount;
static uint32_t s_lastCps;
static uint32_t s_cps;
static uint32_t s_lastSp;
static bool s_waterfall = true;
static bool s_sweepDone;
static bool s_noSi;
static bool s_audio;
static bool s_calDone; /* шумовой пол уже измерен для текущего диапазона */

static uint8_t s_savedVfo;
static uint8_t s_savedMod;
static uint8_t s_savedStep;
static uint32_t s_savedF;
static bool s_capture;

static uint16_t bandChannels(void) {
  return (uint16_t)((BC_BANDS[s_band].end - BC_BANDS[s_band].start) / BC_CH_STEP);
}

static uint32_t channelFreq(uint16_t ch) {
  return BC_BANDS[s_band].start + (uint32_t)ch * BC_CH_STEP;
}

static uint32_t currentFreq(void) { return channelFreq(s_chan); }

static uint16_t currentChannels(void) {
  uint16_t n = bandChannels();
  return n ? n : 1;
}

static void setAudio(bool on) {
  if (on == s_audio)
    return;
  s_audio = on;
  Radio_SiAudio(on);
}

static void tuneCurrent(void) {
  uint32_t f = currentFreq();
  vfos[currentVfo].rxF = f;
  SI47XX_TuneTo(f);
  /* метка канала на спектре переезжает — нужен полный кадр, иначе старый
   * маркер остаётся на прежнем столбце */
  SP_Invalidate(UI_SPECTRUM_H);
}

static void calcThreshold(void) {
  uint16_t thr = s_floor + BC_SQL_MARGIN;
  if (thr < 10)
    thr = 10; /* SI RSSI не опускается до нуля на живом эфире; 0 — сбой */
  if (thr > 120)
    thr = 120; /* SI RSSI максимум 127; выше — заведомо сигнал */
  s_thr = thr;
}

/* Новый проход: сброс спектра/водопада и калибровки. */
static void startBand(void) {
  const BcBand *b = &BC_BANDS[s_band];
  FRange range = {.start = b->start, .end = b->end};
  SP_Init(&range, BC_CH_STEP, b->end - b->start);

  WF_Reset();
  FillRect(0, UI_STATUS_H, LCD_WIDTH, UI_WATERFALL_Y - UI_STATUS_H, C_BLACK);
  if (s_waterfall)
    WF_Clear(UI_WATERFALL_Y);
  else
    FillRect(0, UI_WATERFALL_Y, LCD_WIDTH, LCD_HEIGHT - UI_WATERFALL_Y,
             C_BLACK);

  s_chan = 0;
  s_floor = 0;
  s_calDone = false;
  calcThreshold();
  s_sweepMin = 0xFFFF;
  s_rssi = 0;
  s_sweepDone = false;
  s_state = BC_CAL;
  s_stepCount = 0;
  s_cps = 0;
  s_lastCps = millis();
  s_lastSp = millis() - 150; /* новый диапазон — сразу перерисовать спектр */
  setAudio(false);
  LOOT_Standby();
  tuneCurrent();
  s_nextAt = millis() + BC_SETTLE_MS[s_settleIdx];
}

static void nextChannel(void) {
  uint16_t n = currentChannels();
  s_chan = (s_chan + 1 < n) ? (s_chan + 1) : 0;
  if (s_chan == 0) {
    /* проход закончен: пол по минимуму RSSI, кадр водопада — в render() */
    if (s_sweepMin != 0xFFFF)
      s_floor = s_sweepMin;
    calcThreshold();
    if (!s_calDone) {
      s_calDone = true; /* калибровка закончена — начинаем искать сигналы */
      s_state = BC_SCAN;
    }
    s_sweepMin = 0xFFFF;
    s_sweepDone = true;
    SP_Begin();
  }
  tuneCurrent();
  s_nextAt = millis() + BC_SETTLE_MS[s_settleIdx];
}

static void startScanFromCurrent(void) {
  LOOT_Standby();
  setAudio(false);
  if (s_calDone) {
    s_state = BC_SCAN;
  } else {
    /* калибровку прервали на середине — начинаем проход заново, иначе
     * шумовой пол считался бы только по хвосту диапазона */
    s_state = BC_CAL;
    s_chan = 0;
    s_sweepMin = 0xFFFF;
  }
  tuneCurrent();
  s_nextAt = millis() + BC_SETTLE_MS[s_settleIdx];
}

static void pauseScan(void) {
  if (s_state == BC_HOLD) {
    /* уже слушаем: остаёмся на канале, но выходим из авто-HOLD */
    LOOT_Standby();
    s_state = BC_PAUSE;
    s_nextAt = millis() + BC_PAUSE_POLL_MS;
    return;
  }
  s_state = BC_PAUSE;
  setAudio(true);
  tuneCurrent();
  s_nextAt = millis() + BC_SETTLE_MS[s_settleIdx];
}

/* Ручное переключение канала: из скана/калибровки/HOLD — в PAUSE. */
static void stepChannel(int8_t dir) {
  if (s_state == BC_HOLD)
    LOOT_Standby(); /* ушли с найденного канала — сигнал больше не «открыт» */
  if (s_state != BC_PAUSE) {
    s_state = BC_PAUSE;
    setAudio(true);
  }
  uint16_t n = currentChannels();
  if (dir > 0)
    s_chan = (s_chan + 1 < n) ? (s_chan + 1) : 0;
  else
    s_chan = s_chan ? (s_chan - 1) : (n - 1);
  s_rssi = 0;
  tuneCurrent();
  s_nextAt = millis() + BC_SETTLE_MS[s_settleIdx];
}

static void switchBand(int8_t dir) {
  s_band = (uint8_t)((s_band + BC_BAND_COUNT + dir) % BC_BAND_COUNT);
  startBand();
}

static void addPoint(uint16_t rssi, bool open) {
  Measurement m = {};
  m.f = currentFreq();
  m.rssi = rssi;
  m.open = open;
  SP_AddPoint(&m);
}

static void measureAndAdvance(void) {
  uint16_t r = SI47XX_GetRssi();
  s_rssi = r;
  s_stepCount++;
  if (r < s_sweepMin)
    s_sweepMin = r;

  addPoint(r, false);

  if (s_state == BC_CAL) {
    nextChannel();
    return;
  }

  calcThreshold();
  if (r >= s_thr) {
    /* кандидат: подтверждаем через BC_CONFIRM_MS, не блокируя цикл */
    s_state = BC_CONFIRM;
    s_nextAt = millis() + BC_CONFIRM_MS;
    return;
  }
  nextChannel();
}

static void confirmCandidate(void) {
  uint16_t r = SI47XX_GetRssi();
  s_rssi = r;
  addPoint(r, false);

  if (r >= s_thr) {
    s_state = BC_HOLD;
    setAudio(true);
    LOOT_Hit(currentFreq(), r, MOD_AM, vfos[currentVfo].bw, RADIO_SI4732);
    s_nextAt = millis() + BC_HOLD_MAX_MS;
    return;
  }

  /* ложное срабатывание — чуть поднимаем пол, чтобы не залипать */
  if (s_floor < 0xFFF0)
    s_floor++;
  s_state = BC_SCAN;
  nextChannel();
}

static void holdPoll(void) {
  /* Во время прослушивания RSSI SI не читаем — I2C-запрос щёлкает.
   * HOLD живёт BC_HOLD_MAX_MS, дальше снова скан. */
  addPoint(s_rssi, true);
  LOOT_Hit(currentFreq(), s_rssi, MOD_AM, vfos[currentVfo].bw, RADIO_SI4732);
  setAudio(false);
  LOOT_Standby();
  s_state = BC_SCAN;
  nextChannel();
}

static void pausePoll(void) {
  /* Ручное прослушивание: тоже без RSSI. */
  s_nextAt = millis() + BC_PAUSE_POLL_MS;
}

static void updateCps(uint32_t now) {
  if (now - s_lastCps < 1000)
    return;
  s_cps = s_stepCount * 1000 / (now - s_lastCps);
  s_stepCount = 0;
  s_lastCps = now;
}

class BcScannerMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("BC SW");
    lv_port_set_direct_areas(true);
    Radio_Init();

    s_savedVfo = currentVfo;
    s_savedF = vfos[currentVfo].rxF;
    s_savedMod = vfos[currentVfo].modulation;
    s_savedStep = vfos[currentVfo].step;
    s_capture = false;
    s_noSi = false;
    s_audio = false;
    s_settleIdx = s_settleIdx < BC_SETTLE_N ? s_settleIdx : 2;
    s_bwIdx = s_bwIdx < BC_BW_N ? s_bwIdx : 0;

    vfos[currentVfo].radio = RADIO_SI4732;
    if (!SI47XX_Acquire(SI47XX_AM)) {
      s_noSi = true;
      Radio_ScanAudio(false); /* на всякий случай гасим и BK, и SI */
      FillRect(0, UI_STATUS_H, LCD_WIDTH, LCD_HEIGHT - UI_STATUS_H, C_BLACK);
      return;
    }

    vfos[currentVfo].modulation = MOD_AM;
    vfos[currentVfo].step = STEP_5_0kHz;
    /* В скане АРУ включена (в VFO для AM/SSB она выключена): так RSSI
     * сравним между каналами и сильные станции не перегружают тракт. */
    SI47XX_SetAutomaticGainControl(0, 0);
    SI47XX_SetBandwidth(BC_BW[s_bwIdx], true);
    /* POWER_UP включает громкость 63: сначала явно гасим SI (учёт siMuted
     * мог остаться от прежнего режима), затем BK — в него мог играть VFO. */
    Radio_SiAudio(false);
    Radio_ScanAudio(false);
    s_audio = false;
    startBand();
  }

  void exit() override {
    setAudio(false);
    LOOT_Standby();
    if (!(s_capture && s_savedVfo == currentVfo)) {
      vfos[currentVfo].rxF = s_savedF;
      vfos[currentVfo].modulation = static_cast<ModulationType>(s_savedMod);
      vfos[currentVfo].step = static_cast<Step>(s_savedStep);
      Radio_TuneTo(s_savedF, true);
    }
    /* precise-настройка приоткрыла звук (BK — безусловно): глушим, дальше
     * состоянием управляет шумодав VFO (Radio_Update). */
    Radio_ScanAudio(false);
    lv_port_set_direct_areas(false);
  }

  void update() override {
    if (s_noSi)
      return;

    uint32_t now = millis();
    updateCps(now);
    if ((int32_t)(now - s_nextAt) < 0)
      return;

    switch (s_state) {
    case BC_CAL:
    case BC_SCAN:
      measureAndAdvance();
      break;
    case BC_CONFIRM:
      confirmCandidate();
      break;
    case BC_HOLD:
      holdPoll();
      break;
    case BC_PAUSE:
      pausePoll();
      break;
    }
  }

  void render() override {
    STATUSLINE_render();

    if (s_noSi) {
      /* только разрешённые полосы: ниже UI_WATERFALL_Y LVGL не рисует */
      PrintfT(LCD_XCENTER, UI_INFO1_BASE,
              text_style(C_RED, C_BLACK, POS_C, F_M), "SI4732 NOT FOUND");
      PrintfT(LCD_XCENTER, UI_INFO2_BASE,
              text_style(C_GRAY, C_BLACK, POS_C, F_S), "EXIT to return");
      return;
    }

    uint32_t now = millis();
    uint16_t n = currentChannels();
    char fbuf[16];
    fmt_value(fbuf, sizeof(fbuf), currentFreq(), UMHZ, UNIT_MHZ);

    PrintfT(UI_LEFT_X, UI_RANGE_BASE,
            text_style(C_CYAN, C_BLACK, POS_L, F_M), "%s", BC_BANDS[s_band].name);
    PrintfT(LCD_XCENTER, UI_RANGE_BASE,
            text_style(C_WHITE, C_BLACK, POS_C, F_M), "%s", fbuf);
    PrintfT(UI_RIGHT_X, UI_RANGE_BASE,
            text_style(C_YELLOW, C_BLACK, POS_R, F_M), "%u/%u",
            (unsigned)(s_chan + 1), (unsigned)n);

    if (now - s_lastSp >= 150) {
      s_lastSp = now;
      const BcBand *b = &BC_BANDS[s_band];
      FRange range = {.start = b->start, .end = b->end};
      SP_Render(&range, UI_SPECTRUM_Y, UI_SPECTRUM_H);
      /* Метка текущего канала: после спектра, иначе её затирают
       * изменённые столбцы. Полный кадр (SP_Invalidate в tuneCurrent)
       * стирает метку с прошлого канала. */
      SP_RenderArrow(&range, currentFreq(), 0, UI_SPECTRUM_Y, UI_SPECTRUM_H);
    }

    if (s_sweepDone) {
      s_sweepDone = false;
      if (s_waterfall)
        WF_Render(UI_WATERFALL_Y, true);
    }

    const char *state = BC_STATE_NAMES[s_state];
    Color stateCol = s_state == BC_HOLD      ? C_YELLOW
                     : s_state == BC_PAUSE   ? C_GRAY
                     : s_state == BC_CONFIRM ? C_YELLOW
                                             : C_GREEN;
    PrintfT(UI_LEFT_X, UI_INFO1_BASE,
            text_style(stateCol, C_BLACK, POS_L, F_M), "%s", state);
    PrintfT(LCD_XCENTER, UI_INFO1_BASE,
            text_style(C_WHITE, C_BLACK, POS_C, F_M), "RSSI %u",
            (unsigned)s_rssi);
    PrintfT(UI_RIGHT_X, UI_INFO1_BASE,
            text_style(C_YELLOW, C_BLACK, POS_R, F_M), "%u/s", (unsigned)s_cps);

    PrintfT(UI_LEFT_X, UI_INFO2_BASE,
            text_style(C_GRAY, C_BLACK, POS_L, F_S), "thr %u", (unsigned)s_thr);
    PrintfT(LCD_XCENTER, UI_INFO2_BASE,
            text_style(C_GRAY, C_BLACK, POS_C, F_S), "bw %uk",
            (unsigned)(BC_BW[s_bwIdx] == SI47XX_BW_6_kHz   ? 6
                       : BC_BW[s_bwIdx] == SI47XX_BW_4_kHz ? 4
                       : BC_BW[s_bwIdx] == SI47XX_BW_3_kHz ? 3
                                                           : 2));
    PrintfT(UI_RIGHT_X, UI_INFO2_BASE,
            text_style(C_GRAY, C_BLACK, POS_R, F_S), "L%u",
            (unsigned)LOOT_Size());
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (s_noSi) {
      if (state == KEY_RELEASED && (k == KEY_EXIT || k == KEY_MENU)) {
        Mode_Switch(MODE_VFO);
        return true;
      }
      return false;
    }

    /* Канал листаем по нажатию и автоповтору; отпускание не обрабатываем,
     * иначе после длинного удержания был бы лишний шаг. */
    if ((k == KEY_UP || k == KEY_DOWN) &&
        (state == KEY_PRESSED || state == KEY_LONG_PRESSED_CONT)) {
      stepChannel(k == KEY_UP ? +1 : -1);
      return true;
    }

    if (state == KEY_LONG_PRESSED) {
      switch (k) {
      case KEY_HASH:
        if (s_state == BC_PAUSE)
          startScanFromCurrent();
        else
          pauseScan();
        return true;
      case KEY_MENU:
        LOOTLIST_OpenFrom(MODE_BC_SCANNER);
        return true;
      case KEY_1:
      case KEY_7:
        s_settleIdx =
            (s_settleIdx + (k == KEY_7 ? 1 : BC_SETTLE_N - 1)) % BC_SETTLE_N;
        return true;
      case KEY_SIDE1:
        s_waterfall = !s_waterfall;
        if (!s_waterfall) {
          WF_Reset();
          FillRect(0, UI_WATERFALL_Y, LCD_WIDTH, LCD_HEIGHT - UI_WATERFALL_Y,
                   C_BLACK);
        } else {
          WF_Clear(UI_WATERFALL_Y);
        }
        return true;
      case KEY_SIDE2:
        s_bwIdx = (s_bwIdx + 1) % BC_BW_N;
        SI47XX_SetBandwidth(BC_BW[s_bwIdx], true);
        return true;
      case KEY_5:
        SP_ResetPeaks();
        return true;
      default:
        return false;
      }
    }

    if (state != KEY_RELEASED)
      return false;

    switch (k) {
    case KEY_MENU: /* текущий канал — в VFO и выход */
      vfos[currentVfo].modulation = MOD_AM;
      vfos[currentVfo].step = STEP_5_0kHz;
      Radio_TuneToAuto(currentFreq());
      s_capture = true;
      Mode_Switch(MODE_VFO);
      return true;
    case KEY_EXIT:
      Mode_Switch(MODE_VFO);
      return true;
    case KEY_SIDE1:
      switchBand(+1);
      return true;
    case KEY_SIDE2:
      switchBand(-1);
      return true;
    default:
      break;
    }
    return false;
  }
};

BcScannerMode bcScannerModeInstance;
Mode &MODE_BC_SCANNER = bcScannerModeInstance;
