#pragma once
#include "driver/bk4819.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { RADIO_BK4819A, RADIO_BK4819B, RADIO_SI4732 } Radio;
typedef enum {
  STEP_0_02kHz,
  STEP_0_05kHz,
  STEP_0_5kHz,
  STEP_1_0kHz,
  STEP_2_5kHz,
  STEP_5_0kHz,
  STEP_6_25kHz,
  STEP_8_33kHz,
  STEP_9_0kHz,
  STEP_10_0kHz,
  STEP_12_5kHz,
  STEP_25_0kHz,
  STEP_50_0kHz,
  STEP_100_0kHz,
  STEP_500_0kHz,
  STEP_COUNT,
} Step;

typedef struct {
  uint32_t rxF : 27;
  Step step : 4;
  uint8_t modulation : 4;
  uint8_t bw : 4;
  Radio radio : 2;
  bool radioManual : 1; /* приёмник выбран вручную, не по частоте */
  bool monitor : 1; /* VFO закреплён за фоновым мониторингом */
  uint8_t gainIndex : 5;
} __attribute__((packed)) VFO;

#define DEVICE_VFO_COUNT 3
#define VFO_CH_NAME_LEN 8
#define VFO_CH_VALID 1u
#define VFO_CH_MODE 2u /* VFO сейчас работает как канал */

/* Канал, закреплённый за VFO для multiwatch. Хранится отдельно от частоты
 * VFO: свой rxF у VFO не меняется, пока канал не перехвачен или частоту
 * явно не скопировали в VFO. */
typedef struct __attribute__((packed)) {
  uint32_t f;        /* частота, ед. 10 Гц */
  uint8_t modulation; /* MOD_* */
  uint8_t bw;         /* 0..9 */
  uint8_t radio;      /* Radio */
  uint8_t flags;      /* bit0 — канал назначен */
  char name[VFO_CH_NAME_LEN];
} VFOChannel;

extern VFO vfos[DEVICE_VFO_COUNT];
extern VFOChannel vfoChannels[DEVICE_VFO_COUNT];
extern uint8_t currentVfo;

extern Filter filters[];
extern int8_t filterIndex; /* -1 = AUTO, 0..3 = filters[] */

extern const uint16_t StepFrequencyTable[15];
extern const char *const STEP_NAMES[STEP_COUNT];

void Radio_Init(void);
/* Автовыбор приёмника по частоте (10 Гц) и модуляции */
Radio Radio_PickDevice(uint32_t f, uint8_t mod);
/* AUTO ↔ другой приёмник, если он поддерживает текущую частоту */
void Radio_NextReceiver(void);
void Radio_TuneTo(uint32_t f, bool precise);
void Radio_TuneToAuto(uint32_t f);
/* модуляция и шаг активного VFO по плану диапазонов; false — вне плана */
bool Radio_ApplyBandPlan(uint32_t f);
void Radio_NextBand(int8_t dir);        /* +1/-1: следующий/пред. диапазон группы */
void Radio_NextGroup(void);
void Radio_TuneStep(int8_t dir);
void Radio_NextVfo(void);
void Radio_SwitchToVfo(uint8_t idx);
/* Выполнить отложенное применение VFO — вызывать после отрисовки */
void Radio_ApplyPending(void);
void Radio_ToggleMonitor(uint8_t idx);
/* Канал за VFO: назначение/снятие, чтение и копирование частоты в VFO.
 * Назначение не трогает rxF; multiwatch слушает канал, пока VFO закреплён. */
const VFOChannel *VFO_GetChannel(uint8_t idx);
bool VFO_HasChannel(uint8_t idx);
void VFO_AssignChannel(uint8_t idx, uint32_t f, uint8_t mod, uint8_t bw,
                       uint8_t radio, const char *name);
void VFO_ClearChannel(uint8_t idx);
bool VFO_CopyChannelToVfo(uint8_t idx);
/* Канальный/частотный режим VFO. Включение запоминает частотное состояние
 * VFO и настраивает его на канал; выключение возвращает частоту VFO. */
bool VFO_ChannelMode(uint8_t idx);
bool VFO_SetChannelMode(uint8_t idx, bool on, bool retune = true);
bool VFO_ToggleChannelMode(uint8_t idx);
/* Восстановить канальный режим после загрузки состояния из флеш. */
void VFO_ApplySavedChannelModes(void);
/* Частотное состояние VFO: в канальном режиме — сохранённое до входа. */
const VFO *VFO_FrequencyState(uint8_t idx);
void Radio_NextFilter(void);
/* Вернуть AUTO-фильтр по частоте (режимы сканирования не наследуют ручной) */
void Radio_FilterAuto(void);
void Radio_Update(void);
void Radio_ScanAudio(bool on); /* открыть/закрыть звук активного BK */
/* Звук SI4732 (вещательный КВ-сканер): ведёт учёт siMuted/AF_MUTE, чтобы
 * после выхода VFO снова открыл звук по своим правилам. */
void Radio_SiAudio(bool on);
void Radio_SetSquelch(uint8_t level);
void Radio_ToggleSquelchForce(void);
void Radio_NextStep(void);

extern uint8_t squelchLevel;
