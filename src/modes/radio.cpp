#include "radio.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/bk4819.h"
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
#include "vfo.h"

#define EXTRA_W (11 * 2 + 8)

struct BandRange {
  uint32_t start;
  uint32_t end;
};

static const BandRange KV_BANDS[] = {
    {1750000, 1790000},
    {1890000, 1902000},
    {2145000, 2185000},
    {2560000, 2610000},
    {230000, 249500},
    {320000, 340000},
    {390000, 400000},
    {475000, 506000},
    {590000, 620000},
    {720000, 745000},
    {940000, 990000},
    {1160000, 1210000},
    {1357000, 1387000},
    {1510000, 1580000},
};

static uint8_t rssis[512];

#define KV_BANDS_COUNT (sizeof(KV_BANDS) / sizeof(KV_BANDS[0]))
#define KV_SCAN_STEP 500

struct {
  NumVal_t numVal;
  bool bandsScanning;
} radioCtx;

struct ScannerState {
  uint8_t band_idx;
  uint32_t freq;
  uint8_t done;
};

static void scanner_init(ScannerState *s) {
  s->band_idx = 0;
  s->freq = KV_BANDS[0].start;
  s->done = 0;
}

static int scanner_step(ScannerState *s) {
  s->freq += KV_SCAN_STEP;
  if (s->freq > KV_BANDS[s->band_idx].end) {
    s->band_idx++;
    if (s->band_idx >= KV_BANDS_COUNT) {
      s->band_idx = 0;
      s->freq = KV_BANDS[0].start;
      s->done = 1;
      return 1;
    }
    s->freq = KV_BANDS[s->band_idx].start;
  }
  s->done = 0;
  return 0;
}

static uint16_t scanIndex;

static void scan_all_kv_bands();
static uint32_t kv_freq_by_step(uint16_t step_idx);
static void scanBands();
static void onFreqEntered(uint32_t f);

class RadioMode : public Mode {
public:
  void enter() override;
  void update() override;
  void render() override;
  bool key(KEY_Code_t k, Key_State_t state) override;
};

static RadioMode radioModeInstance;
Mode &MODE_RADIO = radioModeInstance;

static void scan_all_kv_bands() {
  ScannerState sc;
  scanner_init(&sc);
  scanIndex = 0;
  do {
    Radio_TuneTo(sc.freq, true);
    delay_ms(30);
    // RSQ_GET() removed - SI4732 not present on RT-880
    rssis[scanIndex++] = BK4819_GetRSSI();
    radioModeInstance.update();
    radioModeInstance.render();
    scanner_step(&sc);
  } while (!sc.done);
}

static uint32_t kv_freq_by_step(uint16_t step_idx) {
  for (uint8_t i = 0; i < KV_BANDS_COUNT; i++) {
    uint16_t band_steps = (KV_BANDS[i].end - KV_BANDS[i].start) / KV_SCAN_STEP;
    if (step_idx < band_steps) {
      return KV_BANDS[i].start + step_idx * KV_SCAN_STEP;
    }
    step_idx -= band_steps;
  }
  return 0;
}

static void scanBands() { scan_all_kv_bands(); }

static void onFreqEntered(uint32_t f) { Radio_TuneToAuto(f); }

void RadioMode::enter() {
  UI_ClearScreen(C_BLACK);
  currentVfo = 2;
  Radio_TuneTo(10480000, true);
  NumVal_Init(&radioCtx.numVal, LCD_WIDTH - 8 - EXTRA_W - 4, LCD_HEIGHT / 2,
              F_MONO_LG, 21, 23, UNIT_MHZ, vfos[currentVfo].rxF, 0, 1340 * MHZ,
              onFreqEntered, POS_R, F_SM, 11, 12);
}

void RadioMode::update() {
  Radio_Update();
  if (vfos[currentVfo].rxF != NumVal_GetValue(&radioCtx.numVal))
    NumVal_SetValue(&radioCtx.numVal, vfos[currentVfo].rxF);
  NumVal_Update(&radioCtx.numVal);
}

void RadioMode::render() {
  STATUSLINE_render();
  NumVal_Render(&radioCtx.numVal);
}

bool RadioMode::key(KEY_Code_t k, Key_State_t state) {
  if (NumVal_Key(&radioCtx.numVal, k, state))
    return true;

  if (state == KEY_LONG_PRESSED_CONT || state == KEY_PRESSED) {
    switch (k) {
    case KEY_UP:
    case KEY_DOWN:
      scanIndex = IncDecU(scanIndex, 0, 511, k == KEY_UP);
      Radio_TuneTo(kv_freq_by_step(scanIndex), true);
      return true;
    default:
      break;
    }
  }

  if (state == KEY_LONG_PRESSED) {
    switch (k) {
    case KEY_STAR:
      scanBands();
      return true;
    default:
      break;
    }
  }

  if (state != KEY_EVT_RELEASE)
    return false;

  switch (k) {
case KEY_MENU:
     Mode_Switch(MODE_VFO);
     return true;
  default:
    break;
  }
  return false;
}