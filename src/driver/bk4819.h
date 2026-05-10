#ifndef BK4819_H
#define BK4819_H

#include <stdint.h>
#include <stdbool.h>
#include "bk4819-regs.h"

#define BK4819_F_MIN 1588000U
#define BK4819_F_MAX 134000000U
#define AUTO_GAIN_INDEX 0

typedef enum {
    MOD_FM, MOD_AM, MOD_LSB, MOD_USB, MOD_BYP, MOD_RAW, MOD_WFM
} ModulationType;

typedef enum {
    FILTER_VHF, FILTER_UHF, FILTER_OFF
} Filter;

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

typedef struct {
    uint16_t regValue;
    uint8_t  gainDb;
} Gain;

extern const Gain gainTable[32];

void BK4819_Init(void);
void BK4819_Idle(void);

void BK4819_WriteRegister(uint8_t reg, uint16_t data);
uint16_t BK4819_ReadRegister(uint8_t reg);

void BK4819_TuneTo(uint32_t freq, bool precise);
uint16_t BK4819_TuneToAndWaitRSSI(uint32_t freq);
void BK4819_SetModulation(ModulationType mod);
void BK4819_SetFilterBandwidth(BK4819_FilterBandwidth_t bw);
void BK4819_SetAGC(bool useDefault, uint8_t gainIndex);
void BK4819_SetAFC(uint8_t level);

uint16_t BK4819_GetRSSI(void);
uint16_t BK4819_GetSNR(void);
uint16_t BK4819_GetNoise(void);
uint16_t BK4819_GetGlitch(void);
uint16_t BK4819_GetLnaPeakRSSI(void);
uint16_t BK4819_GetAgcRSSI(void);

bool BK4819_IsSquelchOpen(void);
void BK4819_Squelch(uint8_t sq, uint8_t openTime, uint8_t closeTime);
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
void BK4819_PlayDTMFString(const char *str, bool bAuto, uint16_t first, uint16_t tick, uint16_t gap, uint16_t tail);
void BK4819_EnableDTMF(void);
void BK4819_DisableDTMF(void);
void BK4819_PlayRogerTiny(void);
void BK4819_GenTail(uint8_t tail);

void BK4819_SetCTCSSFrequency(uint16_t freq);
void BK4819_SetCDCSSCodeWord(uint32_t codeWord);
void BK4819_SetScrambler(uint8_t scrambler);
void BK4819_SetTailDetection(uint16_t freq);

void BK4819_TurnsOffTones_TurnsOnRX(void);
void BK4819_ExitTxMute(void);
void BK4819_EnterTxMute(void);

#endif
