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

/* Результат скана субтонов (REG_68/69/6A). */
typedef enum {
  BK4819_CSS_RESULT_NOT_FOUND,
  BK4819_CSS_RESULT_CTCSS,
  BK4819_CSS_RESULT_CDCSS,
} BK4819_CssScanResult_t;

typedef struct {
  uint16_t regValue;
  uint8_t gainDb;
} Gain;

extern const Gain gainTable[32];

void BK4819_Init(void);
void BK4819_Idle(void);
/* Погасить/вернуть тракт приёма (RX_LINK/AF/RX DSP) на время SPI-кадра
 * дисплея: PLL/VCO остаются, частота не меняется. */
void BK4819_RxSuspend(void);
void BK4819_RxResume(void);

void BK4819_WriteRegister(uint8_t reg, uint16_t data);
uint16_t BK4819_ReadRegister(uint8_t reg);
/* Чтение по шине, минуя кэш REG_30 (для отладки). */
uint16_t BK4819_ReadRegisterRaw(uint8_t reg);

uint32_t BK4819_GetFrequency(void);
void BK4819_TuneTo(uint32_t freq, bool precise);
void BK4819_SetModulation(ModulationType mod);
void BK4819_SetFilterBandwidth(BK4819_FilterBandwidth_t bw);
void BK4819_SetAGC(bool useDefault, uint8_t gainIndex);
uint8_t BK4819_GetGainDb(uint8_t gainIndex);
void BK4819_SetAFC(uint8_t level);
uint8_t BK4819_GetAFC();
/* Текущая поправка AFC: знаковое значение REG_6D (как в k1-fw). */
int16_t BK4819_GetAFCValue(void);

/* CTCSS/DCS: включить скан субтонов, затем прочитать результат.
 * Скан неблокирующий: между Enable и чтением REG_68/69 нужно ~0.6 с. */
void BK4819_EnableCxCSSScan(void);
void BK4819_DisableCxCSSScan(void);
BK4819_CssScanResult_t BK4819_GetCxCSSScanResult(uint32_t *pCdcssFreq,
                                                 uint16_t *pCtcssFreq);
void BK4819_EnableDTMF(void);
void BK4819_DisableDTMF(void);
uint8_t BK4819_GetDTMF_5TONE_Code(void);

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
/* Вернуть RX-тракт после TX: REG_36/37 и REG_30 с импульсом VCO_CALIB. */
void BK4819_RxEnable(void);
void BK4819_SetupPowerAmplifier(uint8_t power, uint32_t freq);
void BK4819_PrepareTransmit(void);
/* TX deviation (младшие 12 бит REG_40, значение уже *10). */
void BK4819_SetDeviation(uint16_t dev10);
/* TX CTCSS tail detection для STE (частота в единицах 10 Гц). */
void BK4819_SetTailDetection(uint32_t freq10);
void BK4819_ToggleGpioOut(uint8_t pin, bool on);
/* Полное состояние REG_33 (GPIO out) — для TX-гейтов и их снятия. */
uint16_t BK4819_GetGpioOutState(void);
void BK4819_SetGpioOutState(uint16_t state);
/* Сбросить кэш частоты после прямой записи REG_38/39. */
void BK4819_InvalidateFrequencyCache(void);
void BK4819_ToggleAFDAC(bool on);
void BK4819_ToggleAFBit(bool on);

void BK4819_EnterSubAu(void);
void BK4819_ExitSubAu(void);
void BK4819_TurnsOffTones_TurnsOnRX(void);
void BK4819_EnableFrequencyScan(void);
/* REG_32 <15:14>: 0 = 0.2 с, 1 = 0.4 с, 2 = 0.8 с, 3 = 1.6 с. */
typedef enum {
  FREQ_SCAN_200MS,
  FREQ_SCAN_400MS,
  FREQ_SCAN_800MS,
  FREQ_SCAN_1600MS,
  F_SC_T_COUNT
} FreqScanTime;
void BK4819_EnableFrequencyScanEx(FreqScanTime time);
void BK4819_EnableFrequencyScanEx2(FreqScanTime time, uint16_t hz);
void BK4819_DisableFrequencyScan(void);
void BK4819_SelectChip(uint8_t chip);
void BK4819_SelectB(bool on);
void BK4819_SelectFilterByFrequency(uint32_t f);
/* Ровно один фильтр (HF/VHF/UHF/800), без выбора по частоте. */
void BK4819_SelectFilter(Filter flt);
/* Ручной фильтр: on=true — держать flt независимо от частоты,
 * on=false — вернуть автоматический выбор по частоте. */
void BK4819_SetFilterOverride(Filter flt, bool on);
void BK4819_InvalidateFilterCache(void);
void BK4819_ToggleFilter(Filter flt, bool on);
void BK4819_SelectFilterEx(Filter filter);
uint16_t BK4819_GetFilter(void);
bool BK4819_GetFrequencyScanResult(uint32_t *pFrequency);

#endif
