/* BC ALL — обход всех вещательных КВ-диапазонов SI4732 встроенным seek.
 *
 * Раньше полоса проходилась вручную по каналам 5 кГц в два прохода
 * (калибровка + поиск). Теперь чип сам ищет станции по порогу RSSI:
 * полоса задаётся свойствами AM_SEEK_*, а режим только опрашивает
 * TUNE_STATUS и складывает найденное в общий LOOT. После обхода
 * открывается список находок.
 *
 * Вход: VFO → длинный 3. Выход — EXIT (в VFO), MENU во время скана —
 * открыть уже найденное.
 */
#include "bc_all.h"
#include "bc_bands.h"
#include "../driver/board.h"
#include "../driver/si473x.h"
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

#define BC_ALL_SEEK_POLL_MS 25
#define BC_ALL_SEEK_RETRY_MS 200
#define BC_ALL_SEEK_STALL_MS 8000
#define BC_ALL_SEEK_RSSI 10 /* порог встроенного seek, дБмкВ */
#define BC_ALL_THR_MIN 5
#define BC_ALL_THR_MAX 60
#define BC_ALL_THR_STEP 5

static uint8_t s_band;
static uint16_t s_channels;
static uint32_t s_freq;
static uint16_t s_rssi;
static uint16_t s_seekThr = BC_ALL_SEEK_RSSI;
static uint16_t s_shownPct;
static uint16_t s_found;
static uint16_t s_total;
static bool s_done;
static bool s_noSi;
static bool s_restored;
static uint32_t s_nextAt;
static uint32_t s_lastSeekAt;
static uint32_t s_progressAt;
static uint32_t s_lastFoundF;
static uint32_t s_savedF;
static uint8_t s_savedMod;
static uint8_t s_savedStep;

static uint16_t bandChannelsAt(uint8_t band) {
  const BcBand *b = &BC_BANDS[band];
  uint32_t n = (b->end - b->start) / BC_CH_STEP;
  return n ? (uint16_t)n : 1;
}

static uint16_t currentChannel(void) {
  const BcBand *b = &BC_BANDS[s_band];
  uint32_t off = s_freq > b->start ? s_freq - b->start : 0;
  uint16_t ch = (uint16_t)(off / BC_CH_STEP);
  return ch < s_channels ? ch : (uint16_t)(s_channels - 1);
}

static void startBand(void) {
  const BcBand *b = &BC_BANDS[s_band];
  s_channels = bandChannelsAt(s_band);
  s_freq = b->start;
  s_rssi = 0;
  vfos[currentVfo].rxF = b->start;
  /* top — последний канал полосы (end исключительный) */
  SI47XX_SeekAmStart(b->start, b->end - BC_CH_STEP, BC_CH_STEP, s_seekThr,
                     true, false);
  s_lastSeekAt = millis();
  s_progressAt = millis();
  s_nextAt = millis() + BC_ALL_SEEK_POLL_MS;
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

static void nextBand(void) {
  if (s_band + 1u < BC_BAND_COUNT) {
    s_band++;
    startBand();
    return;
  }
  openLoot();
}

static void sendSeek(void) {
  SI47XX_Seek(true, false); /* вверх от текущей частоты */
  s_lastSeekAt = millis();
  s_nextAt = millis() + BC_ALL_SEEK_POLL_MS;
}

static void pollSeek(void) {
  SI47XX_SeekStatus st;
  SI47XX_TuneStatusGet(&st);
  if (st.freq) {
    s_freq = st.freq;
    s_rssi = st.rssi;
  }

  if (st.valid) {
    uint32_t now = millis();
    bool isNew = (st.freq != s_lastFoundF);
    if (isNew) {
      vfos[currentVfo].rxF = st.freq;
      LOOT_Hit(st.freq, st.rssi, MOD_AM, vfos[currentVfo].bw, RADIO_SI4732);
      s_found++;
      s_lastFoundF = st.freq;
      s_progressAt = now;
    }
    if (st.limit) {
      nextBand();
      return;
    }
    if (isNew || (uint32_t)(now - s_lastSeekAt) >= BC_ALL_SEEK_RETRY_MS) {
      sendSeek();
      return;
    }
    s_nextAt = now + BC_ALL_SEEK_POLL_MS;
    return;
  }

  if (st.limit) {
    nextBand();
    return;
  }

  /* Не валид и не предел: STC с AM-патчем приходит не всегда, поэтому
   * опираемся на VALID/BLTF, а команду периодически повторяем от текущей
   * частоты (как SI4735 seekStationProgress). */
  uint32_t now = millis();
  if ((uint32_t)(now - s_progressAt) >= BC_ALL_SEEK_STALL_MS) {
    nextBand(); /* полоса не двигается — не залипаем */
    return;
  }
  if (st.done || (uint32_t)(now - s_lastSeekAt) >= BC_ALL_SEEK_RETRY_MS) {
    sendSeek();
    return;
  }
  s_nextAt = now + BC_ALL_SEEK_POLL_MS;
}

static uint16_t completedChannels(void) {
  uint16_t done = 0;
  for (uint8_t i = 0; i < s_band; i++)
    done += bandChannelsAt(i);
  done += currentChannel();
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
    s_found = 0;
    s_done = false;
    s_noSi = false;
    s_restored = false;
    s_rssi = 0;
    s_shownPct = 0;
    s_lastFoundF = 0;
    s_total = 0;
    for (uint8_t i = 0; i < BC_BAND_COUNT; i++)
      s_total += bandChannelsAt(i);

    s_savedF = vfos[currentVfo].rxF;
    s_savedMod = vfos[currentVfo].modulation;
    s_savedStep = vfos[currentVfo].step;

    vfos[currentVfo].radio = RADIO_SI4732;
    /* Штатный AM-ром: с continuous-патчем seek игнорирует AM_SEEK_*. */
    if (!SI47XX_AcquireRawAm()) {
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
    pollSeek();
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
    snprintf(buf, sizeof(buf), "%s  SEEK %u/%u", BC_BANDS[s_band].name,
             (unsigned)(currentChannel() + 1), (unsigned)s_channels);
    PrintfT(LCD_XCENTER, UI_INFO1_BASE,
            text_style(C_WHITE, C_BLACK, POS_C, F_M), "%s", buf);

    uint16_t done = completedChannels();
    uint16_t target = s_total ? (uint16_t)((uint32_t)done * 100 / s_total) : 0;
    /* Прогресс seek приходит рывками (по станциям) — подтягиваем
     * отображаемый процент к фактическому за несколько кадров. */
    if (s_shownPct < target) {
      uint16_t step = (uint16_t)((target - s_shownPct + 3) / 4);
      s_shownPct = (uint16_t)(s_shownPct + (step ? step : 1));
      if (s_shownPct > target)
        s_shownPct = target;
    }
    snprintf(buf, sizeof(buf), "found %u  %u%%", (unsigned)s_found,
             (unsigned)s_shownPct);
    PrintfT(LCD_XCENTER, UI_INFO2_BASE,
            text_style(C_GREEN, C_BLACK, POS_C, F_M), "%s", buf);

    const uint16_t bx = 20;
    const uint16_t bw = LCD_WIDTH - 2 * bx;
    const uint16_t by = UI_INFO2_BASE + 24;
    FillRect(bx, by, bw, 8, C_GRAY);
    if (s_shownPct) {
      uint16_t w = (uint16_t)((uint32_t)bw * s_shownPct / 100);
      FillRect(bx, by, w, 8, C_GREEN);
    }

    PrintfT(LCD_XCENTER, by + 24,
            text_style(C_GRAY, C_BLACK, POS_C, F_S),
            "RSSI %u  thr %u  1/7  EXIT abort", (unsigned)s_rssi,
            (unsigned)s_seekThr);
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    /* Порог seek: 7 — выше (чище, меньше станций), 1 — ниже. */
    if (state == KEY_EVT_LONG_PRESS && (k == KEY_1 || k == KEY_7)) {
      if (k == KEY_7) {
        if (s_seekThr < BC_ALL_THR_MAX)
          s_seekThr += BC_ALL_THR_STEP;
      } else if (s_seekThr > BC_ALL_THR_MIN) {
        s_seekThr -= BC_ALL_THR_STEP;
      }
      SI47XX_SetSeekAmRssiThreshold(s_seekThr);
      return true;
    }

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
