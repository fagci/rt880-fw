#include "bands.h"
#include "misc.h"
#include "radio.h"

/* Единственный источник вещательных КВ-полос (кГц, конец исключительный). */
#define BC_SW_BANDS(X)                                                         \
  X("120m", 2300, 2495)                                                        \
  X("90m", 3200, 3400)                                                         \
  X("75m", 3900, 4000)                                                         \
  X("60m", 4750, 5060)                                                         \
  X("49m", 5900, 6200)                                                         \
  X("41m", 7200, 7450)                                                         \
  X("31m", 9400, 9900)                                                         \
  X("25m", 11600, 12100)                                                       \
  X("22m", 13570, 13870)                                                       \
  X("19m", 15100, 15800)                                                       \
  X("16m", 17480, 17900)                                                       \
  X("15m", 18900, 19020)                                                       \
  X("13m", 21450, 21850)                                                       \
  X("11m", 25670, 26100)

#define BC_ROW(nm, a, b) {nm, (a) * KHZ, (b) * KHZ},
const BcBand BC_BANDS[] = {BC_SW_BANDS(BC_ROW)};
#undef BC_ROW
const uint8_t BC_BAND_COUNT = sizeof(BC_BANDS) / sizeof(BC_BANDS[0]);

#define BC(a, b, m, st, nm) {a, b, m, st, nm, CAT_BC, true}
#define HAM(a, b, m, st, nm) {a, b, m, st, nm, CAT_HAM, true}
#define SVC(a, b, m, st, nm) {a, b, m, st, nm, CAT_SVC, true}
#define BC_PLAN_ROW(nm, a, b) BC(a, b, MOD_AM, STEP_5_0kHz, nm " BC"),

/* Первое совпадение выигрывает; общие диапазоны (loop=false) — в конце.
 * V/UHF: пока везде FM и шаг 25 кГц (авиа — AM). */
static const BandPlan PLAN[] = {
    /* вещание: ДВ/СВ и все КВ-вещательные */
    BC(153, 279, MOD_AM, STEP_9_0kHz, "LW"),
    BC(520, 1710, MOD_AM, STEP_9_0kHz, "MW"),
    BC_SW_BANDS(BC_PLAN_ROW)
    BC(64000, 108000, MOD_WFM, STEP_100_0kHz, "FM BC"), /* на SI4732 */
    /* любительские КВ (SSB): ниже 10 МГц LSB, выше USB */
    HAM(1800, 2000, MOD_LSB, STEP_1_0kHz, "160m"),
    HAM(3500, 3800, MOD_LSB, STEP_1_0kHz, "80m"),
    HAM(5300, 5410, MOD_USB, STEP_1_0kHz, "60m"),
    HAM(7000, 7200, MOD_LSB, STEP_1_0kHz, "40m"),
    HAM(10100, 10150, MOD_USB, STEP_1_0kHz, "30m"),
    HAM(14000, 14350, MOD_USB, STEP_1_0kHz, "20m"),
    HAM(18068, 18168, MOD_USB, STEP_1_0kHz, "17m"),
    HAM(21000, 21450, MOD_USB, STEP_1_0kHz, "15m"),
    HAM(24890, 24990, MOD_USB, STEP_1_0kHz, "12m"),
    HAM(28000, 29700, MOD_USB, STEP_1_0kHz, "10m"),
    /* службы */
    SVC(26965, 27405, MOD_AM, STEP_10_0kHz, "CB"),
    SVC(118000, 136000, MOD_AM, STEP_25_0kHz, "AIR"),
    SVC(144000, 146000, MOD_FM, STEP_25_0kHz, "2m"),
    SVC(156000, 163000, MOD_FM, STEP_25_0kHz, "MARINE"),
    SVC(430000, 440000, MOD_FM, STEP_25_0kHz, "70cm"),
    SVC(446000, 446200, MOD_FM, STEP_25_0kHz, "PMR446"),
    /* общие: без зацикливания и без группы */
    {100, 30000, MOD_AM, STEP_5_0kHz, "HF", CAT_NONE, false},
    {30000, 1340000, MOD_FM, STEP_25_0kHz, "V/UHF", CAT_NONE, false},
};
#define PLAN_N (sizeof(PLAN) / sizeof(PLAN[0]))

/* последняя частота в каждом диапазоне — при возврате SIDE1/2 */
static uint32_t s_bandMem[PLAN_N];
/* последний диапазон, где были в группе — при переключении группы */
static int8_t s_groupLast[CAT_COUNT] = {-1, -1, -1};

uint8_t Band_Count(void) { return (uint8_t)PLAN_N; }

const BandPlan &Band_At(uint8_t i) { return PLAN[i]; }

int8_t Band_Index(uint32_t f) {
  uint32_t khz = f / 100; /* 10 Гц → кГц */
  for (uint8_t i = 0; i < PLAN_N; i++)
    if (khz >= PLAN[i].startKhz && khz < PLAN[i].endKhz)
      return (int8_t)i;
  return -1;
}

const BandPlan *Band_Find(uint32_t f) {
  int8_t i = Band_Index(f);
  return i < 0 ? nullptr : &PLAN[i];
}

const char *Band_Name(uint32_t f) {
  const BandPlan *b = Band_Find(f);
  return b ? b->name : "";
}

bool Band_Range(uint32_t f, uint32_t *start, uint32_t *end) {
  const BandPlan *b = Band_Find(f);
  if (!b)
    return false;
  if (start)
    *start = (uint32_t)b->startKhz * KHZ;
  if (end)
    *end = (uint32_t)b->endKhz * KHZ;
  return true;
}

uint32_t Band_Mem(uint8_t i) { return s_bandMem[i]; }

void Band_MemSet(uint8_t i, uint32_t f) { s_bandMem[i] = f; }

int8_t Band_GroupLast(uint8_t cat) { return s_groupLast[cat]; }

void Band_GroupLastSet(uint8_t cat, int8_t i) { s_groupLast[cat] = i; }
