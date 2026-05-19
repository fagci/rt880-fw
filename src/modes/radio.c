#include "radio.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/si473x.h"
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

typedef struct {
  uint32_t start; // нижняя граница, ×10 Гц
  uint32_t end;   // верхняя граница, ×10 Гц
} BandRange;

// Все вещательные КВ диапазоны (ITU)
// 1 МГц = 100000 × 10 Гц
static const BandRange KV_BANDS[] = {
    {1750000, 1790000}, // 17 м  (17.5 – 17.9 МГц)
    {1890000, 1902000}, // 16 м  (18.9 – 19.02 МГц) — малоиспользуемый
    {2145000, 2185000}, // 13 м  (21.45 – 21.85 МГц)
    {2560000, 2610000}, // 11 м  (25.6 – 26.1 МГц)
    {230000, 249500},   // 120 м (2.3 – 2.495 МГц)
    {320000, 340000},   // 90 м  (3.2 – 3.4 МГц)
    {390000, 400000},   // 75 м  (3.9 – 4.0 МГц)
    {475000, 506000},   // 60 м  (4.75 – 5.06 МГц)
    {590000, 620000},   // 49 м  (5.9 – 6.2 МГц)
    {720000, 745000},   // 41 м  (7.2 – 7.45 МГц)
    {940000, 990000},   // 31 м  (9.4 – 9.9 МГц)
    {1160000, 1210000}, // 25 м  (11.6 – 12.1 МГц)
    {1357000, 1387000}, // 22 м  (13.57 – 13.87 МГц)
    {1510000, 1580000}, // 19 м  (15.1 – 15.8 МГц)
};

static uint8_t rssis[512];

#define KV_BANDS_COUNT (sizeof(KV_BANDS) / sizeof(KV_BANDS[0]))

// Шаг сканирования: 5 кГц = 500 × 10 Гц (стандартный шаг АМ вещания на КВ)
#define KV_SCAN_STEP 500

typedef struct {
  NumVal_t numVal;
  bool bandsScanning;
} MainCtx_t;
static MainCtx_t ctx;

// Состояние сканера
typedef struct {
  uint8_t band_idx; // текущий индекс диапазона
  uint32_t freq;    // текущая частота, ×10 Гц
  uint8_t done;     // флаг завершения полного цикла
} Scanner;

void scanner_init(Scanner *s) {
  s->band_idx = 0;
  s->freq = KV_BANDS[0].start;
  s->done = 0;
}

// Возвращает 0 — нормально, 1 — завершён полный цикл
int scanner_step(Scanner *s) {
  s->freq += KV_SCAN_STEP;

  // Перешли за конец текущего диапазона — ищем следующий
  if (s->freq > KV_BANDS[s->band_idx].end) {
    s->band_idx++;

    // Все диапазоны пройдены
    if (s->band_idx >= KV_BANDS_COUNT) {
      s->band_idx = 0;
      s->freq = KV_BANDS[0].start;
      s->done = 1;
      return 1; // <-- конец цикла
    }

    s->freq = KV_BANDS[s->band_idx].start;
  }

  s->done = 0;
  return 0;
}

static uint16_t scanIndex;

static void update(Mode_t *self);
static void render(Mode_t *self);

// Пример использования
void scan_all_kv_bands(void) {
  Scanner sc;
  scanner_init(&sc);

  scanIndex = 0;

  do {
    Radio_TuneTo(sc.freq, true);
    // TODO: get RSSI

    delay_ms(30);
    RSQ_GET();
    rssis[scanIndex++] = rsqStatus.resp.SNR;

    update(&MODE_RADIO);
    render(&MODE_RADIO);

    scanner_step(&sc);
  } while (!sc.done);
}

uint32_t kv_freq_by_step(uint16_t step_idx) {
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

static void enter(Mode_t *self) {
  (void)self;
  UI_ClearScreen(C_BLACK);
  currentVfo = 2;
  Radio_TuneTo(10480000, true);
  NumVal_Init(&ctx.numVal, LCD_WIDTH - 8 - EXTRA_W - 4, LCD_HEIGHT / 2,
              F_MONO_LG, 21, 23, UNIT_MHZ, vfos[currentVfo].rxF, 0, 1340 * MHZ,
              onFreqEntered, POS_R, F_SM, 11, 12);
}

static void update(Mode_t *self) {
  (void)self;
  Radio_Update();

  if (vfos[currentVfo].rxF != NumVal_GetValue(&ctx.numVal))
    NumVal_SetValue(&ctx.numVal, vfos[currentVfo].rxF);
  NumVal_Update(&ctx.numVal);
}

static void render(Mode_t *self) {
  (void)self;
  STATUSLINE_render();
  NumVal_Render(&ctx.numVal);
}

static void exitMode(Mode_t *self) { (void)self; }

static bool key(Mode_t *self, key_id_t k, key_evt_type_t state) {
  (void)self;
  if (NumVal_Key(&ctx.numVal, k, state))
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
    Mode_Switch(&MODE_VFO);
    return true;
  default:
    break;
  }
  return false;
}

Mode_t MODE_RADIO = {
    .enter = enter,
    .exit = exitMode,
    .update = update,
    .render = render,
    .key = key,
    .ctx = &ctx,
};
