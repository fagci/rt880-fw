#pragma once
#include "../misc.h"
#include <stdint.h>

/* Сетка вещательных КВ-диапазонов: 5 кГц. Частоты — в единицах 10 Гц,
 * поэтому 5 кГц = 500. Границы — как в PLAN (radio.cpp), конец
 * исключительный: последний канал = end - 5 кГц. */
#define BC_CH_STEP 500U

typedef struct {
  const char *name;
  uint32_t start; /* ед. 10 Гц */
  uint32_t end;   /* ед. 10 Гц, исключительно */
} BcBand;

static const BcBand BC_BANDS[] = {
    {"120m", 2300 * KHZ, 2495 * KHZ},  {"90m", 3200 * KHZ, 3400 * KHZ},
    {"75m", 3900 * KHZ, 4000 * KHZ},   {"60m", 4750 * KHZ, 5060 * KHZ},
    {"49m", 5900 * KHZ, 6200 * KHZ},   {"41m", 7200 * KHZ, 7450 * KHZ},
    {"31m", 9400 * KHZ, 9900 * KHZ},   {"25m", 11600 * KHZ, 12100 * KHZ},
    {"22m", 13570 * KHZ, 13870 * KHZ}, {"19m", 15100 * KHZ, 15800 * KHZ},
    {"16m", 17480 * KHZ, 17900 * KHZ}, {"15m", 18900 * KHZ, 19020 * KHZ},
    {"13m", 21450 * KHZ, 21850 * KHZ}, {"11m", 25670 * KHZ, 26100 * KHZ},
};
#define BC_BAND_COUNT (sizeof(BC_BANDS) / sizeof(BC_BANDS[0]))
