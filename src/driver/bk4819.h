#ifndef BK4819_H
#define BK4819_H

#include "../helper/measurements.h"
#include "bk4819-regs.h"
#include <stdbool.h>
#include <stdint.h>

#define BK4819_F_MIN 1588000U
#define BK4819_F_MAX 134000000U
#define AUTO_GAIN_INDEX 0

typedef enum {
  BK4819_AF_MUTE,
  BK4819_AF_FM,
  BK4819_AF_ALAM, // tone
  BK4819_AF_BEEP, // for tx
  BK4819_AF_RAW,  // (ssb without if filter = raw in sdr sharp)
  BK4819_AF_USB,  // (or ssb = lsb and usb at the same time)
  BK4819_AF_CTCO, // ctcss/dcs (fm with narrow filters for ctcss/dcs)
  BK4819_AF_AM,
  BK4819_AF_FSKO, // fsk out test with special fsk filters (need reg58 fsk on to
                  // give sound on speaker )
  BK4819_AF_BYPASS, // (fm without filter = discriminator output)
} BK4819_AF_Type_t;

typedef enum {
  MOD_FM,
  MOD_AM,
  MOD_LSB,
  MOD_USB,
  MOD_BYP,
  MOD_RAW,
  MOD_WFM,
} ModulationType;

typedef enum {
  XTAL_0_13M,
  XTAL_1_19_2M,
  XTAL_2_26M,
  XTAL_3_38_4M,
} XtalMode;

typedef enum {
  BK4819_FILTER_BW_6k,
  BK4819_FILTER_BW_7k,
  BK4819_FILTER_BW_9k,
  BK4819_FILTER_BW_10k,
  BK4819_FILTER_BW_12k,
  BK4819_FILTER_BW_14k,
  BK4819_FILTER_BW_17k,
  BK4819_FILTER_BW_20k,
  BK4819_FILTER_BW_23k,
  BK4819_FILTER_BW_26k,
} BK4819_FilterBandwidth_t;

typedef enum {
  SQUELCH_RSSI_NOISE_GLITCH,
  SQUELCH_RSSI_GLITCH,
  SQUELCH_RSSI_NOISE,
  SQUELCH_RSSI,
} SquelchType;

typedef enum {
  FILTER_HF = 0x40 >> BK4819_GPIO6_HF_RX,
  FILTER_VHF = 0x40 >> BK4819_GPIO2_VHF_RX,
  FILTER_UHF = 0x40 >> BK4819_GPIO1_UHF_RX,
  FILTER_800 = 0x40 >> BK4819_GPIO5_UHF800_RX,
} Filter;

typedef struct {
  uint16_t regValue;
  uint8_t gainDb;
} Gain;

extern const Gain gainTable[32];

void BK4819_Init(void);
void BK4819_Idle(void);

void BK4819_WriteRegister(uint8_t reg, uint16_t data);
uint16_t BK4819_ReadRegister(uint8_t reg);

uint32_t BK4819_GetFrequency(void);
void BK4819_TuneTo(uint32_t freq, bool precise);
void BK4819_SetModulation(ModulationType mod);
void BK4819_SetFilterBandwidth(BK4819_FilterBandwidth_t bw);
void BK4819_SetAGC(bool useDefault, uint8_t gainIndex);
void BK4819_SetAFC(uint8_t level);
uint8_t BK4819_GetAFC();

uint16_t BK4819_GetRSSI(void);
uint8_t BK4819_GetSNR(void);
uint8_t BK4819_GetNoise(void);
uint8_t BK4819_GetGlitch(void);
uint16_t BK4819_GetLnaPeakRSSI(void);
uint16_t BK4819_GetAgcRSSI(void);

bool BK4819_IsSquelchOpen(void);
void BK4819_SetupSquelch(SQL sq, uint8_t delayO, uint8_t delayC);
void BK4819_SquelchType(SquelchType t);

void BK4819_RX_TurnOn(void);
void BK4819_SetupPowerAmplifier(uint8_t power, uint32_t freq);
void BK4819_PrepareTransmit(void);
void BK4819_ToggleGpioOut(uint8_t pin, bool on);
void BK4819_ToggleAFDAC(bool on);
void BK4819_ToggleAFBit(bool on);

void BK4819_EnterSubAu(void);
void BK4819_ExitSubAu(void);
void BK4819_EnterDTMF_TX(bool bTail);
void BK4819_ExitDTMF_TX(bool bTail);
void BK4819_PlayDTMFString(const char *str, bool bAuto, uint16_t first,
                           uint16_t tick, uint16_t gap, uint16_t tail);
void BK4819_EnableDTMF(void);
void BK4819_DisableDTMF(void);
void BK4819_PlayRogerTiny(void);
void BK4819_GenTail(uint8_t tail);

typedef enum {
  F_SC_T_0_2s,
  F_SC_T_0_4s,
  F_SC_T_0_8s,
  F_SC_T_1_6s,
  F_SC_T_COUNT
} FreqScanTime;

bool BK4819_GetFrequencyScanResult(uint32_t *pFrequency);
void BK4819_DisableFrequencyScan(void);
void BK4819_EnableFrequencyScanEx2(FreqScanTime t, uint16_t hz);
void BK4819_SelectFilterEx(Filter filter);

void BK4819_SetCTCSSFrequency(uint16_t freq);
void BK4819_SetCDCSSCodeWord(uint32_t codeWord);
void BK4819_SetScrambler(uint8_t scrambler);
void BK4819_SetTailDetection(uint16_t freq);

void BK4819_TurnsOffTones_TurnsOnRX(void);
void BK4819_ExitTxMute(void);
void BK4819_EnterTxMute(void);

uint16_t BK4819_GetFilter();
void BK4819_ToggleFilter(Filter flt, bool on);
void BK4819_SelectFilterByFrequency(uint32_t f);
void BK4819_SelectB(bool on);

#endif
