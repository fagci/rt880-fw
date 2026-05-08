#ifndef SI473X_H
#define SI473X_H

#include <stdint.h>
#include <stdbool.h>

#define SI47XX_F_MIN     15000
#define SI47XX_F_MAX     3000000
#define SI47XX_FM_F_MIN  6400000
#define SI47XX_FM_F_MAX  10800000

typedef enum { SI47XX_FM, SI47XX_AM, SI47XX_LSB, SI47XX_USB, SI47XX_CW } SI47XX_MODE;

typedef enum {
    SI47XX_BW_6_kHz, SI47XX_BW_4_kHz, SI47XX_BW_3_kHz,
    SI47XX_BW_2_kHz, SI47XX_BW_1_kHz, SI47XX_BW_1_8_kHz, SI47XX_BW_2_5_kHz,
} SI47XX_FilterBW;

typedef enum {
    SI47XX_SSB_BW_1_2_kHz, SI47XX_SSB_BW_2_2_kHz, SI47XX_SSB_BW_3_kHz,
    SI47XX_SSB_BW_4_kHz,   SI47XX_SSB_BW_0_5_kHz, SI47XX_SSB_BW_1_0_kHz,
} SI47XX_SsbFilterBW;

#define CMD_POWER_UP        0x01
#define CMD_GET_REV         0x10
#define CMD_POWER_DOWN      0x11
#define CMD_SET_PROPERTY    0x12
#define CMD_GET_PROPERTY    0x13
#define CMD_FM_TUNE_FREQ    0x20
#define CMD_FM_SEEK_START   0x21
#define CMD_FM_TUNE_STATUS  0x22
#define CMD_FM_RSQ_STATUS   0x23
#define CMD_AM_TUNE_FREQ    0x40
#define CMD_AM_SEEK_START   0x41
#define CMD_AM_TUNE_STATUS  0x42
#define CMD_AM_RSQ_STATUS   0x43
#define CMD_AM_AGC_OVERRIDE 0x48

#define PROP_FM_RDS_CONFIG           0x1502
#define PROP_FM_RDS_INT_SOURCE       0x1500
#define PROP_FM_RDS_INT_FIFO_COUNT   0x1501
#define PROP_RX_VOLUME               0x4000
#define PROP_AM_SOFT_MUTE_MAX_ATTENUATION 0x3302
#define PROP_AM_AGC_RELEASE_RATE     0x3301
#define PROP_AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN 0x3303
#define PROP_SSB_MODE                0x0100
#define PROP_SSB_SOFT_MUTE_MAX_ATTENUATION 0x0102

#define STATUS_CTS 0x80

#define FUNC_FM 0x00
#define FUNC_AM 0x01
#define OUT_ANALOG 0x05

void SI47XX_Init(void);
void SI47XX_PowerUp(void);
void SI47XX_PowerDown(void);
void SI47XX_SetFreq(uint16_t freq);
void SI47XX_TuneTo(uint32_t f);
void SI47XX_SwitchMode(SI47XX_MODE mode);
void SI47XX_SetVolume(uint8_t volume);
void SI47XX_SetBandwidth(SI47XX_FilterBW bw, bool amplflt);
void SI47XX_SetSsbBandwidth(SI47XX_SsbFilterBW bw);
void SI47XX_SetAutomaticGainControl(uint8_t agcdis, uint8_t agcidx);
void SI47XX_SetProperty(uint16_t prop, uint16_t value);
uint16_t SI47XX_ReadRegister(uint8_t reg);

#endif
