#include "bands.h"
#include "driver/board.h"
#include "helper/store.h"
#include "misc.h"
#include "radio.h"

#include <stdio.h>
#include <string.h>

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

/* Заводской план: шаблон для первой правки и запасной вариант, если
 * пользовательский список пуст. Первое совпадение выигрывает; общие
 * диапазоны (loop=false) — в конце. */
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
static_assert(PLAN_N <= BAND_MAX, "PLAN must fit BAND_MAX");

/* ─── Пользовательский список ─── */
static BandRec s_user[BAND_MAX];
static uint8_t s_count;
static bool s_inited;
static bool s_dirty;
static uint32_t s_dirtyAt;
static BandPlan s_scratch;

/* последняя частота в каждом диапазоне — при возврате SIDE1/2 */
static uint32_t s_bandMem[BAND_MAX];
/* последний диапазон, где были в группе — при переключении группы */
static int8_t s_groupLast[CAT_COUNT] = {-1, -1, -1};

#define BAND_SAVE_DELAY_MS 2500u

static uint8_t catColor(uint8_t cat) {
  switch (cat) {
  case CAT_BC:
    return 3; /* жёлтый */
  case CAT_HAM:
    return 1; /* зелёный */
  case CAT_SVC:
    return 2; /* голубой */
  default:
    return 2; /* голубой: общие HF/V-UHF не должны выглядеть «неактивными» */
  }
}

static void nameCopy(char *dst, const char *src) {
  if (!src)
    src = "";
  strncpy(dst, src, BAND_NAME_LEN - 1);
  dst[BAND_NAME_LEN - 1] = 0;
}

static bool sanitizeRec(BandRec *r) {
  if (r->endKhz <= r->startKhz || r->startKhz < 1 || r->endKhz > 1340000u)
    return false;
  if (r->mod > MOD_WFM)
    r->mod = MOD_FM;
  if (r->step >= STEP_COUNT)
    r->step = STEP_25_0kHz;
  if (r->cat != CAT_NONE && r->cat >= CAT_COUNT)
    r->cat = CAT_NONE;
  if (r->color >= 8)
    r->color = catColor(r->cat);
  if (r->squelch > 10 && r->squelch != BAND_SQL_NONE)
    r->squelch = BAND_SQL_NONE;
  r->name[BAND_NAME_LEN - 1] = 0;
  if (!r->name[0])
    nameCopy(r->name, "BAND");
  return true;
}

static void seedFactory(void) {
  s_count = 0;
  for (uint8_t i = 0; i < PLAN_N && s_count < BAND_MAX; ++i) {
    const BandPlan &p = PLAN[i];
    BandRec *r = &s_user[s_count++];
    memset(r, 0, sizeof(*r));
    r->startKhz = p.startKhz;
    r->endKhz = p.endKhz;
    r->corr10 = 0;
    r->mod = p.mod;
    r->step = p.step;
    r->cat = p.cat;
    r->color = catColor(p.cat);
    r->squelch = BAND_SQL_NONE;
    nameCopy(r->name, p.name);
  }
}

static void clearMemory(void) {
  memset(s_bandMem, 0, sizeof(s_bandMem));
  for (uint8_t c = 0; c < CAT_COUNT; ++c)
    s_groupLast[c] = -1;
}

void Band_Init(void) {
  if (s_inited)
    return;
  s_inited = true;
  s_count = 0;
  s_dirty = false;
  clearMemory();

#ifdef HOST_PREVIEW
  seedFactory();
#else
  uint16_t n = 0;
  if (Store_BandsLoad(&n)) {
    for (uint16_t i = 0; i < n && s_count < BAND_MAX; ++i) {
      BandRec r;
      if (!Store_BandsRead(i, &r))
        break;
      if (sanitizeRec(&r))
        s_user[s_count++] = r;
    }
  }
  if (!s_count)
    seedFactory();
#endif
}

void Band_Reset(void) {
  s_inited = true;
  s_dirty = false;
  clearMemory();
  seedFactory();
}

static void ensureInit(void) {
  if (!s_inited)
    Band_Init();
}

#ifndef HOST_PREVIEW
static bool saveAll(void) {
  if (!Store_BandsWriteBegin())
    return false;
  for (uint8_t i = 0; i < s_count; ++i) {
    if (!Store_BandsWritePut(&s_user[i])) {
      Store_BandsWriteAbort();
      return false;
    }
  }
  if (!Store_BandsWriteCommit(s_count)) {
    Store_BandsWriteAbort();
    return false;
  }
  return true;
}
#endif

void Band_Touch(void) {
  s_dirty = true;
  s_dirtyAt = millis();
}

void Band_Poll(void) {
  if (!s_dirty)
    return;
  if (millis() - s_dirtyAt < BAND_SAVE_DELAY_MS)
    return;
#ifdef HOST_PREVIEW
  s_dirty = false;
#else
  if (saveAll())
    s_dirty = false;
  else
    s_dirtyAt = millis();
#endif
}

uint8_t Band_Count(void) {
  ensureInit();
  return s_count;
}

const BandPlan &Band_At(uint8_t i) {
  ensureInit();
  memset(&s_scratch, 0, sizeof(s_scratch));
  s_scratch.name = "";
  if (i >= s_count)
    return s_scratch;
  const BandRec &r = s_user[i];
  s_scratch.startKhz = r.startKhz;
  s_scratch.endKhz = r.endKhz;
  s_scratch.mod = r.mod;
  s_scratch.step = r.step;
  s_scratch.name = r.name;
  s_scratch.cat = r.cat;
  s_scratch.loop = (r.cat != CAT_NONE);
  s_scratch.corr10 = r.corr10;
  s_scratch.squelch = r.squelch;
  s_scratch.color = r.color;
  return s_scratch;
}

int8_t Band_Index(uint32_t f) {
  ensureInit();
  uint32_t khz = f / 100; /* 10 Гц → кГц */
  for (uint8_t i = 0; i < s_count; i++)
    if (khz >= s_user[i].startKhz && khz < s_user[i].endKhz)
      return (int8_t)i;
  return -1;
}

const BandPlan *Band_Find(uint32_t f) {
  int8_t i = Band_Index(f);
  return i < 0 ? nullptr : &Band_At((uint8_t)i);
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

int16_t Band_Corr10(uint32_t f) {
  const BandPlan *b = Band_Find(f);
  return b ? b->corr10 : 0;
}

uint8_t Band_Squelch(uint32_t f) {
  const BandPlan *b = Band_Find(f);
  return b ? b->squelch : BAND_SQL_NONE;
}

uint8_t Band_Color(uint32_t f) {
  const BandPlan *b = Band_Find(f);
  return b ? b->color : 7;
}

const BandRec *Band_Rec(uint16_t i) {
  ensureInit();
  return i < s_count ? &s_user[i] : nullptr;
}

BandRec *Band_RecEdit(uint16_t i) {
  ensureInit();
  return i < s_count ? &s_user[i] : nullptr;
}

int16_t Band_Add(const BandRec *rec) {
  ensureInit();
  if (!rec || s_count >= BAND_MAX)
    return -1;
  BandRec r = *rec;
  if (!sanitizeRec(&r))
    return -1;
  s_user[s_count] = r;
  Band_Touch();
  return (int16_t)s_count++;
}

bool Band_Delete(uint16_t i) {
  ensureInit();
  if (i >= s_count || s_count <= 1)
    return false;
  for (uint16_t k = i; k + 1 < s_count; ++k)
    s_user[k] = s_user[k + 1];
  s_count--;
  clearMemory(); /* индексы сдвинулись — память диапазонов/групп не годна */
  Band_Touch();
  return true;
}

uint32_t Band_Mem(uint8_t i) {
  return i < BAND_MAX ? s_bandMem[i] : 0;
}

void Band_MemSet(uint8_t i, uint32_t f) {
  if (i < BAND_MAX)
    s_bandMem[i] = f;
}

int8_t Band_GroupLast(uint8_t cat) {
  if (cat >= CAT_COUNT)
    return -1;
  int8_t v = s_groupLast[cat];
  if (v < 0 || v >= (int8_t)Band_Count() || s_user[v].cat != cat)
    return -1; /* диапазон удалён или сменил категорию */
  return v;
}

void Band_GroupLastSet(uint8_t cat, int8_t i) {
  if (cat >= CAT_COUNT)
    return;
  s_groupLast[cat] = (i >= 0 && i < (int8_t)Band_Count()) ? i : -1;
}
