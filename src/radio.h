#pragma once
#include "driver/bk4819.h"
#include <stdbool.h>
#include <stdint.h>

#define MHZ 100000

typedef enum { RADIO_BK4819A, RADIO_BK4819B, RADIO_SI4732 } Radio;
typedef enum {
  OFFSET_NONE,
  OFFSET_PLUS,
  OFFSET_MINUS,
  OFFSET_FREQ
} OffsetDirection;
typedef enum { TX_POW_ULOW, TX_POW_LOW, TX_POW_MID, TX_POW_HIGH } TXOutputPower;
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
  uint8_t value;
  uint8_t type : 4;
} Code;
typedef struct {
  Code rx;
  Code tx;
} CodeRXTX;
typedef struct {
  uint8_t value : 4;
  uint8_t type : 2;
} Squelch;

typedef struct {
  uint16_t scanlists;
  uint16_t channel;
  char name[10];
  uint32_t rxF : 27;
  int32_t ppm : 5;
  uint32_t txF : 27;
  OffsetDirection offsetDir : 2;
  bool allowTx : 1;
  uint8_t reserved2 : 2;
  Step step : 4;
  uint8_t modulation : 4;
  uint8_t bw : 4;
  Radio radio : 2;
  TXOutputPower power : 2;
  uint8_t mic : 4;
  uint8_t scrambler : 4;
  Squelch squelch;
  CodeRXTX code;
  bool fixedBoundsMode : 1;
  bool isChMode : 1;
  bool monitor : 1; /* VFO закреплён за фоновым мониторингом */
  uint8_t reserved3 : 1;
  uint8_t gainIndex : 5;
  uint8_t deviation;
  uint32_t upconverter;
} __attribute__((packed)) VFO;

#define DEVICE_VFO_COUNT 3

extern VFO vfos[DEVICE_VFO_COUNT];
extern uint8_t currentVfo;

extern Filter filters[];
extern const char *FILTER_NAMES[];
extern uint8_t filterIndex;

extern const uint16_t StepFrequencyTable[15];

void Radio_Init(void);
/* Автовыбор приёмника по частоте (10 Гц) и модуляции */
Radio Radio_PickDevice(uint32_t f, uint8_t mod);
void Radio_TuneTo(uint32_t f, bool precise);
void Radio_TuneToAuto(uint32_t f);
/* модуляция и шаг активного VFO по плану диапазонов; false — вне плана */
bool Radio_ApplyBandPlan(uint32_t f);
const char *Radio_BandName(uint32_t f); /* "40m", "AIR"...; "" вне плана */
void Radio_NextBand(int8_t dir);        /* +1/-1: следующий/пред. диапазон */
void Radio_TuneStep(int8_t dir);
void Radio_NextVfo(void);
void Radio_SwitchToVfo(uint8_t idx);
/* Выполнить отложенное применение VFO — вызывать после отрисовки */
void Radio_ApplyPending(void);
void Radio_ToggleMonitor(uint8_t idx);
void Radio_NextFilter(void);
void Radio_ToggleFilter(void);
void Radio_Update(void);
void Radio_ScanAudio(bool on); /* открыть/закрыть звук активного BK */
void Radio_SetSquelch(uint8_t level);
void Radio_NextStep(void);
void Radio_PrevStep(void);
void Radio_NextFilter(void);
void Radio_ToggleFilter(void);

extern uint8_t squelchLevel;
