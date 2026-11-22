/* BC ALL — проход по всем вещательным КВ-диапазонам SI4732.
 *
 * В отличие от BC-сканера (одна полоса с водопадом), этот режим сам
 * обходит BC_BANDS: сначала калибрует шумовой пол каждой полосы, затем
 * повторно проходит её с порогом пол+10 дБ и складывает найденные
 * частоты в общий LOOT. После обхода открывается список находок, по
 * которому можно ходить UP/DOWN и слушать выбранную станцию.
 *
 * Вход: VFO → длинный 3. Выход — EXIT (в VFO), MENU во время скана —
 * открыть уже найденное.
 */
#include "bc_all.h"
#include "bc_bands.h"
#include "../driver/board.h"
#include "../driver/si473x.h"
#include "../driver/st7789.h"
#include "../helper/lootlist.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/lv_port.h"
#include "../ui/screen_layout.h"
#include "../ui/statusline.h"
#include "loot_screen.h"
#include "mode.h"
#include "vfo.h"
#include <stdio.h>

#define BC_ALL_SETTLE_MS 35
#define BC_ALL_MARGIN 10
#define BC_ALL_THR_MIN 12
#define BC_ALL_THR_MAX 120

static uint8_t s_band;
static uint16_t s_chan;
static uint16_t s_channels;
static uint16_t s_floor;
static uint16_t s_thr;
static uint16_t s_rssi;
static uint16_t s_found;
static uint16_t s_total;
static bool s_cal;
static bool s_done;
static bool s_noSi;
static bool s_restored;
static uint32_t s_nextAt;
static uint32_t s_savedF;
static uint8_t s_savedMod;
static uint8_t s_savedStep;

static uint16_t bandChannels(void) {
  const BcBand *b = &BC_BANDS[s_band];
  uint32_t n = (b->end - b->start) / BC_CH_STEP;
  return n ? (uint16_t)n : 1;
}

static uint32_t channelFreq(void) {
  return BC_BANDS[s_band].start + (uint32_t)s_chan * BC_CH_STEP;
}

static void tuneCurrent(void) {
  vfos[currentVfo].rxF = channelFreq();
  SI47XX_TuneTo(channelFreq());
}

static void startBand(void) {
  s_channels = bandChannels();
  s_chan = 0;
  s_floor = 0xFFFF;
  s_thr = BC_ALL_THR_MIN;
  s_cal = true;
  tuneCurrent();
  s_nextAt = millis() + BC_ALL_SETTLE_MS;
}

static void restoreVfo(void) {
  if (s_restored)
    return;
  s_restored = true;
  vfos[currentVfo].rxF = s_savedF;
  vfos[currentVfo].modulation = (ModulationType)s_savedMod;
  vfos[currentVfo].step = (Step)s_savedStep;
  Radio_TuneTo(s_savedF, true);
}

static void openLoot(void) {
  if (s_done)
    return;
  s_done = true;
  LOOT_Standby();
  restoreVfo();
  LOOTLIST_OpenFrom(MODE_VFO);
}

static void advanceChannel(void) {
  if (s_cal) {
    if (s_chan + 1 < s_channels) {
      s_chan++;
      tuneCurrent();
      s_nextAt = millis() + BC_ALL_SETTLE_MS;
      return;
    }
    /* Полоса откалибрована: второй проход уже с порогом. */
    uint16_t thr = s_floor + BC_ALL_MARGIN;
    if (thr < BC_ALL_THR_MIN)
      thr = BC_ALL_THR_MIN;
    if (thr > BC_ALL_THR_MAX)
      thr = BC_ALL_THR_MAX;
    s_thr = thr;
    s_cal = false;
    s_chan = 0;
    tuneCurrent();
    s_nextAt = millis() + BC_ALL_SETTLE_MS;
    return;
  }

  if (s_chan + 1 < s_channels) {
    s_chan++;
    tuneCurrent();
    s_nextAt = millis() + BC_ALL_SETTLE_MS;
    return;
  }

  if (s_band + 1 < BC_BAND_COUNT) {
    s_band++;
    startBand();
    return;
  }
  openLoot();
}

static void measure(void) {
  uint16_t r = SI47XX_GetRssi();
  s_rssi = r;
  if (s_cal) {
    if (r < s_floor)
      s_floor = r;
  } else if (r >= s_thr) {
    LOOT_Hit(channelFreq(), r, MOD_AM, vfos[currentVfo].bw, RADIO_SI4732);
    s_found++;
  }
  advanceChannel();
}

static uint16_t completedChannels(void) {
  uint16_t done = 0;
  for (uint8_t i = 0; i < s_band; i++) {
    uint32_t n = (BC_BANDS[i].end - BC_BANDS[i].start) / BC_CH_STEP;
    done += (uint16_t)(2 * n);
  }
  done += s_cal ? s_chan : (uint16_t)(s_channels + s_chan);
  if (done > s_total)
    done = s_total;
  return done;
}

class BcAllMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("BC ALL");
    lv_port_set_direct_areas(false);

    Radio_Init();
    s_band = 0;
    s_chan = 0;
    s_found = 0;
    s_done = false;
    s_noSi = false;
    s_restored = false;
    s_rssi = 0;
    s_total = 0;
    for (uint8_t i = 0; i < BC_BAND_COUNT; i++) {
      uint32_t n = (BC_BANDS[i].end - BC_BANDS[i].start) / BC_CH_STEP;
      s_total += (uint16_t)(2 * n);
    }

    s_savedF = vfos[currentVfo].rxF;
    s_savedMod = vfos[currentVfo].modulation;
    s_savedStep = vfos[currentVfo].step;

    vfos[currentVfo].radio = RADIO_SI4732;
    if (!SI47XX_Acquire(SI47XX_AM)) {
      s_noSi = true;
      Radio_ScanAudio(false);
      return;
    }

    vfos[currentVfo].modulation = MOD_AM;
    vfos[currentVfo].step = STEP_5_0kHz;
    SI47XX_SetAutomaticGainControl(0, 0);
    SI47XX_SetBandwidth(SI47XX_BW_4_kHz, true);
    Radio_SiAudio(false);
    Radio_ScanAudio(false);
    startBand();
  }

  void exit() override {
    Radio_SiAudio(false);
    LOOT_Standby();
    if (!s_restored)
      restoreVfo();
    Radio_ScanAudio(false);
    lv_port_set_direct_areas(false);
  }

  void update() override {
    if (s_noSi || s_done)
      return;
    if ((int32_t)(millis() - s_nextAt) < 0)
      return;
    measure();
  }

  void render() override {
    STATUSLINE_render();

    if (s_noSi) {
      PrintfT(LCD_XCENTER, UI_INFO1_BASE,
              text_style(C_RED, C_BLACK, POS_C, F_M), "SI4732 NOT FOUND");
      PrintfT(LCD_XCENTER, UI_INFO2_BASE,
              text_style(C_GRAY, C_BLACK, POS_C, F_S), "EXIT to return");
      return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "%s  %s %u/%u", BC_BANDS[s_band].name,
             s_cal ? "CAL" : "SCAN", (unsigned)(s_chan + 1),
             (unsigned)s_channels);
    PrintfT(LCD_XCENTER, UI_INFO1_BASE,
            text_style(C_WHITE, C_BLACK, POS_C, F_M), "%s", buf);

    uint16_t done = completedChannels();
    uint16_t pct = s_total ? (uint16_t)((uint32_t)done * 100 / s_total) : 0;
    snprintf(buf, sizeof(buf), "found %u  %u%%", (unsigned)s_found,
             (unsigned)pct);
    PrintfT(LCD_XCENTER, UI_INFO2_BASE,
            text_style(C_GREEN, C_BLACK, POS_C, F_M), "%s", buf);

    const uint16_t bx = 20;
    const uint16_t bw = LCD_WIDTH - 2 * bx;
    const uint16_t by = UI_INFO2_BASE + 24;
    FillRect(bx, by, bw, 8, C_GRAY);
    if (pct) {
      uint16_t w = (uint16_t)((uint32_t)bw * pct / 100);
      FillRect(bx, by, w, 8, C_GREEN);
    }

    PrintfT(LCD_XCENTER, by + 24,
            text_style(C_GRAY, C_BLACK, POS_C, F_S),
            "RSSI %u  scan all BC  EXIT abort", (unsigned)s_rssi);
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (state != KEY_EVT_RELEASE)
      return false;

    if (k == KEY_MENU) {
      openLoot();
      return true;
    }
    if (k == KEY_EXIT) {
      Mode_Switch(MODE_VFO);
      return true;
    }
    return false;
  }
};

BcAllMode bcAllModeInstance;
Mode &MODE_BC_ALL = bcAllModeInstance;
