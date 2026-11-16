#include "radio.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/si473x.h"
#include "helper/log.h"
#include "helper/measurements.h"
#include "ui/graphics.h"
#include <string.h>

VFO vfos[DEVICE_VFO_COUNT] = {{}, {}, {}};

uint8_t currentVfo = 0;

Filter filters[] = {FILTER_HF, FILTER_VHF, FILTER_UHF, FILTER_800};
const char *FILTER_NAMES[] = {"HF", "VHF", "UHF", "800"};
uint8_t filterIndex = 0;

const uint16_t StepFrequencyTable[STEP_COUNT] = {
    2,   5,    50,   100,  250,  500,   625,   833,
    900, 1000, 1250, 2500, 5000, 10000, 50000,
};

/* Названия шагов для отображения */
const char *const STEP_NAMES[STEP_COUNT] = {
    "20Hz", "50Hz", "500Hz", "1k",  "2.5k", "5k",   "6.25k", "8.33k",
    "9k",   "10k",  "12.5k", "25k", "50k",  "100k", "500k",
};

uint8_t squelchLevel = 4;

static bool bkDacOn[2] = {false, false};
static bool siMuted = true; /* SI стартует замьюченным (громкость 0) */
static bool afMuteOn = false;
static bool s_pendingApply; /* SwitchToVfo отложил тяжёлое применение */
static bool s_bootMute;     /* старт: не открывать звук до первого опроса шумодава */

/* Как часто BK «бегает» по закреплённым VFO и сколько сидит на каждом */
#define MON_DWELL_MS 120
#define MON_SETTLE_MS 8
static uint32_t s_lastMon;

static ModulationType Radio_AutoModulation(uint32_t f) {
  if (f >= 150000 && f < 2000000) return MOD_AM;
  if (f >= 2000000 && f <= 30000000) return MOD_AM;
  if (f > 30000000 && f < 80000000)  return MOD_FM;
  if (f >= 88000000 && f <= 108000000) return MOD_WFM;
  return MOD_FM;
}

/* ── Выбор приёмника ───────────────────────────────────────────
 * f — в единицах 10 Гц.
 * SI4732: AM/SSB ниже 30 МГц. После первой загрузки SSB-патча чип
 * живёт в патч-режиме постоянно (патч покрывает AM+SSB) — переходы
 * между ними без re-power и без повторной 10-секундной заливки.
 * WFM 74–108 — на BK4819: мгновенное переключение, а SI для WFM
 * требовал бы выхода из патч-режима (= перезаливка патча каждый раз).
 * BK4819B: HF-тракт ниже 30 МГц (всё, что не взял SI).
 * BK4819A: всё остальное (V/UHF + FM-вещание). */
Radio Radio_PickDevice(uint32_t f, uint8_t mod) {
  if (f < 3000000 && (mod == MOD_AM || mod == MOD_LSB || mod == MOD_USB))
    return RADIO_SI4732;
  if (f < 3000000)
    return RADIO_BK4819B;
  return RADIO_BK4819A;
}

static SI47XX_MODE modToSi(uint8_t mod) {
  switch (mod) {
    case MOD_AM:  return SI47XX_AM;
    case MOD_LSB: return SI47XX_LSB;
    case MOD_USB: return SI47XX_USB;
    default:      return SI47XX_FM;
  }
}

/* Включает SI4732 при первом использовании; держит его в патч-режиме
   (патч покрывает AM+SSB): повторной заливки между AM↔SSB нет.
   Переходы внутри патч-режима — только смена глобалки si4732mode
   (драйвер сам кладёт её в аргументы команд). Re-power только если
   чип не в патче, а нужен SSB. При отсутствии чипа — откат на BK. */
/* SI4732: питание и нужный continuous-патч через SI47XX_Acquire.
 * Переходы AM↔SSB внутри патча — мгновенные; смена патча — re-power
 * (~1-2 с). При отсутствии чипа — откат VFO на BK. */
static void siEnsure(VFO *v) {
  SI47XX_MODE want = modToSi(v->modulation);
  if (!SI47XX_Acquire(want)) {
    Log("SI4732: not responding, fallback to BK");
    v->radio = Radio_PickDevice(v->rxF, MOD_FM);
  }
}

/* ── Аудиотракт ────────────────────────────────────────────────
 * BK AF DAC — по кристаллу активного VFO (звук только активного).
 * SI4732 суммируется в тракт аналогово: слышен, когда активный VFO
   на нём (громкость 63), иначе глушим SetVolume(0) — чип живым.
 * AF_MUTE — общий гейт усилителя: для SI всегда открыт (шумодава
   у SI пока нет), для BK — по squelch. */
static void applyMute(bool sqOpen) {
  Radio ar = vfos[currentVfo].radio;
  bool activeSi = (ar == RADIO_SI4732);

  if (activeSi == siMuted) {
    SI47XX_SetVolume(activeSi ? 63 : 0);
    siMuted = !activeSi;
  }

  uint8_t chip = (ar == RADIO_BK4819B) ? 1 : 0;
  bool wantBk = !activeSi && sqOpen;
  for (uint8_t c = 0; c < 2; c++) {
    bool want = (c == chip) && wantBk;
    if (want != bkDacOn[c]) {
      BK4819_SelectChip(c);
      BK4819_ToggleAFDAC(want);
      bkDacOn[c] = want;
    }
  }

  bool unmute = activeSi ? true : sqOpen;
  if (unmute != afMuteOn) {
    gpio_pin_write(&PIN_AF_MUTE, unmute);
    afMuteOn = unmute;
  }
}

static uint8_t vfoChip(uint8_t idx) {
  return (vfos[idx].radio == RADIO_BK4819B) ? 1 : 0;
}

/* precise=true: полное применение (смена VFO, ручной ввод).
   precise=false: только частота (сканирование, мониторинг). */
static void applyVfo(bool precise) {
  VFO *v = &vfos[currentVfo];
  v->radio = Radio_PickDevice(v->rxF, v->modulation);

  if (v->radio == RADIO_SI4732) {
    siEnsure(v); /* может откатить v->radio на BK */
    if (v->radio == RADIO_SI4732) {
      SI47XX_TuneTo(v->rxF); /* единицы 10 Гц — как и раньше */
      SI47XX_SetAutomaticGainControl(1, 0);
    }
  }

  if (v->radio != RADIO_SI4732) {
    uint8_t chip = vfoChip(currentVfo);
    BK4819_SelectChip(chip);
    BK4819_SelectFilterByFrequency(v->rxF);
    BK4819_TuneTo(v->rxF, precise);
    if (precise) {
      /* WFM на BK — широкополосный FM (BW до 18k вместо 12k) */
      ModulationType m = (v->modulation == MOD_WFM) ? MOD_FM
                         : static_cast<ModulationType>(v->modulation);
      BK4819_SetModulation(m);
      BK4819_SetFilterBandwidth(
          static_cast<BK4819_FilterBandwidth_t>(v->bw));
      BK4819_SetupSquelch(GetSql(squelchLevel), 1, 1);
      audio_path_set(chip);
    }
  }

  if (precise)
    applyMute(!s_bootMute);
}

/* ── Мониторинг: один BK крутится по закреплённым VFO ──────────
 * Кандидаты — VFO с флагом monitor, НЕ активные, на BK и на кристалле,
 * отличном от активного (один кристалл = один частотный тракт).
 * По открывшемуся шумодаву монитор становится активным VFO. */
static void monitorTick(void) {
  uint32_t now = millis();
  if (now - s_lastMon < MON_DWELL_MS)
    return;
  s_lastMon = now;

  Radio ar = vfos[currentVfo].radio;
  uint8_t cand[DEVICE_VFO_COUNT];
  uint8_t n = 0;
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    if (i == currentVfo || !vfos[i].monitor)
      continue;
    vfos[i].radio = Radio_PickDevice(vfos[i].rxF, vfos[i].modulation);
    if (vfos[i].radio == RADIO_SI4732 || vfos[i].radio == ar)
      continue;
    cand[n++] = i;
  }
  if (!n)
    return;

  static uint8_t slot;
  slot = (slot + 1) % n;
  uint8_t i = cand[slot];
  VFO *m = &vfos[i];
  uint8_t chip = vfoChip(i);

  BK4819_SelectChip(chip);
  BK4819_TuneTo(m->rxF, false);
  BK4819_SetupSquelch(GetSql(squelchLevel), 1, 1);
  delay_ms(MON_SETTLE_MS);
  bool open = BK4819_IsSquelchOpen();
  if (bkDacOn[chip]) { /* монитор не должен звучать */
    BK4819_ToggleAFDAC(false);
    bkDacOn[chip] = false;
  }

  if (open) {
    Log("mon: vfo %u sq open", i);
    currentVfo = i;
    applyVfo(true);
  }
}

/* ── Public API ─────────────────────────────────────────────── */

void Radio_Init(void) {
  /* Идемпотентно: повторные входы в режимы не сбрасывают VFO
     и не переинициализируют кристаллы */
  static bool inited;
  if (inited)
    return;
  inited = true;

  memset(vfos, 0, sizeof(vfos));

  /* Дефолты без SI на старте: SSB-патч грузится лениво (один раз за
     включение) при первом переходе на КВ, чтобы старт был мгновенным */
  vfos[0].rxF = 14550000; /* 145.5 МГц FM → BK A */
  vfos[0].step = STEP_25_0kHz;
  vfos[0].modulation = MOD_FM;
  vfos[1].rxF = 14200000; /* 14.2 МГц USB → SI4732 (лениво) */
  vfos[1].step = STEP_1_0kHz;
  vfos[1].modulation = MOD_USB;
  vfos[2].rxF = 10480000; /* 104.8 МГц WFM → BK A */
  vfos[2].step = STEP_100_0kHz;
  vfos[2].modulation = MOD_WFM;
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    vfos[i].bw = BK4819_FILTER_BW_12k;
    vfos[i].radio = Radio_PickDevice(vfos[i].rxF, vfos[i].modulation);
  }

  for (uint8_t c = 0; c < 2; c++) {
    BK4819_SelectChip(c);
    BK4819_Init();
    BK4819_RX_TurnOn();
    BK4819_SetAFC(0);
    BK4819_SetAGC(true, 1);
    /* RX_TurnOn включает AF DAC — выключаем, учёт bkDacOn = false верен.
       Иначе на старте шипит до первого опроса шумодава. */
    BK4819_ToggleAFDAC(false);
  }

  /* RST и бит-банг I2C SI4732 — без этого чип в ресете, IsAlive = false */
  SI47XX_Init();

  currentVfo = 0;
  s_pendingApply = false;
  siMuted = true; /* SI ещё не включён: при включении включим громкость */
  bkDacOn[0] = bkDacOn[1] = false;
  s_bootMute = true;
  applyVfo(true);
  s_bootMute = false;
}

/* Каждый update-цикл: шумодав активного VFO + мониторинг. */
void Radio_Update(void) {
  Radio ar = vfos[currentVfo].radio;
  bool sqOpen = true;
  if (ar != RADIO_SI4732) {
    BK4819_SelectChip(vfoChip(currentVfo));
    sqOpen = (squelchLevel == 0) || BK4819_IsSquelchOpen();
  }
  applyMute(sqOpen);

  monitorTick();

  /* monitorTick мог переключить кристаллы — вернём контекст активного */
  if (vfos[currentVfo].radio != RADIO_SI4732)
    BK4819_SelectChip(vfoChip(currentVfo));
}

/* Диагностика шины BK: реальное чтение REG_30 и RSSI каждого чипа.
   0xFFFF/0x0000 в r30 — чтение по SPI не работает (RMW REG_30 в TuneTo
   тогда портит конфигурацию приёмника). */
void Radio_DiagBk(void) {
  for (uint8_t c = 0; c < 2; c++) {
    BK4819_SelectChip(c); /* сбрасывает кэш REG_30 → читаем по шине */
    uint16_t r30 = BK4819_ReadRegister(BK4819_REG_30);
    uint16_t r1 = BK4819_GetRSSI();
    uint16_t r2 = BK4819_GetRSSI();
    Log("BK%u r30=%04X rssi=%u/%u", c, r30, r1, r2);
  }
  if (vfos[currentVfo].radio != RADIO_SI4732)
    BK4819_SelectChip(vfoChip(currentVfo));
}

/* Звук сканера: тот же путь, что и у VFO — учёт DAC/AF_MUTE общий,
   иначе после выхода из сканера VFO думает, что DAC включён, и молчит. */
void Radio_ScanAudio(bool on) { applyMute(on); }

void Radio_SetSquelch(uint8_t level) {
  squelchLevel = level;
  for (uint8_t c = 0; c < 2; c++) {
    BK4819_SelectChip(c);
    BK4819_SetupSquelch(GetSql(level), 1, 1);
  }
}

void Radio_TuneTo(uint32_t f, bool precise) {
  vfos[currentVfo].rxF = f;
  applyVfo(precise);
}

void Radio_TuneToAuto(uint32_t f) {
  vfos[currentVfo].modulation = Radio_AutoModulation(f);
  vfos[currentVfo].rxF = f;
  applyVfo(true);
}

void Radio_TuneStep(int8_t dir) {
  uint32_t step = StepFrequencyTable[vfos[currentVfo].step];
  Radio_TuneTo(vfos[currentVfo].rxF + step * dir, false);
}

void Radio_NextVfo(void) { Radio_SwitchToVfo(currentVfo + 1); }

void Radio_SwitchToVfo(uint8_t idx) {
  currentVfo = idx % DEVICE_VFO_COUNT;
  /* Тяжёлое применение (инициализация кристалла, SSB-патч SI4732)
     откладываем — экран успеет перерисовать новый активный ряд,
     радио догоняет через Radio_ApplyPending. */
  s_pendingApply = true;
}

/* Вызывать ПОСЛЕ отрисовки (из render режима): выполняет отложенное
   применение VFO, не блокируя перерисовку. */
void Radio_ApplyPending(void) {
  if (s_pendingApply) {
    s_pendingApply = false;
    uint32_t t0 = millis();
    applyVfo(true);
    Log("vfo apply %lu ms", (unsigned long)(millis() - t0));
  }
}

void Radio_ToggleMonitor(uint8_t idx) {
  if (idx == currentVfo)
    return; /* активный не мониторится — он и так на своём приёмнике */
  vfos[idx].monitor = !vfos[idx].monitor;
  Log("mon: vfo %u %s", idx, vfos[idx].monitor ? "on" : "off");
}

void Radio_NextStep(void) {
  VFO *v = &vfos[currentVfo];
  v->step = static_cast<Step>((v->step + 1) % STEP_COUNT);
}

void Radio_PrevStep(void) {
  VFO *v = &vfos[currentVfo];
  v->step = static_cast<Step>((v->step + STEP_COUNT - 1) % STEP_COUNT);
}

void Radio_NextFilter(void) { filterIndex = (filterIndex + 1) % 4; }

void Radio_ToggleFilter(void) {
  BK4819_ToggleFilter(filters[filterIndex],
                      !(BK4819_GetFilter() & filters[filterIndex]));
}
