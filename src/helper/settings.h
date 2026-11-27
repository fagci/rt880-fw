#ifndef RT880_SETTINGS_H
#define RT880_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

/* Пользовательские настройки системы. Порядок и смысл пунктов повторяют
 * ../k1-fw/src/settings.h; не поддержанное железом RT-880 помечается N/A
 * (Settings_IsNa) и не меняет поведение. */
typedef enum {
  SET_EEPROMTYPE,
  SET_BATSAVE,
  SET_TX_ENABLE, /* раньше VOX: слот сохранён, чтобы не менять Store v3 */
  SET_BACKLIGHT,
  SET_TXTIME,
  SET_CURRENTSCANLIST,
  SET_TXPOWER, /* раньше Roger: слот сохранён, чтобы не менять Store v3 */
  SET_SCANMODE,
  SET_CHDISPLAYMODE,
  SET_BEEP,
  SET_KEYLOCK,
  SET_PTT_LOCK,
  SET_BUSYCHANNELTXLOCK,
  SET_STE,
  SET_REPEATERSTE,
  SET_DTMFDECODE,
  SET_BRIGHTNESS_H,
  SET_BRIGHTNESS_L,
  SET_GAMMA, /* раньше Contrast (ST7565): слот сохранён, чтобы не менять Store v3 */
  SET_MAINAPP,
  SET_SQOPENEDTIMEOUT,
  SET_SQCLOSEDTIMEOUT,
  SET_SQLOPENTIME,
  SET_SQLCLOSETIME,
  SET_SKIPGARBAGEFREQUENCIES,
  SET_ACTIVEVFO,
  SET_BACKLIGHTONSQUELCH,
  SET_BATTERYCALIBRATION,
  SET_BATTERYTYPE,
  SET_BATTERYSTYLE,
  SET_UPCONVERTER,
  SET_DEVIATION,
  SET_MIC,
  SET_SHOWLEVELINVFO,
  SET_ALWAYS_RSSI,
  SET_BOUND240_280,
  SET_NOLISTEN,
  SET_SI4732POWEROFF,
  SET_TONELOCAL,
  SET_FCTIME,
  SET_MULTIWATCH,
  SET_FREQ_CORRECTION,
  SET_INVERT_BUTTONS, /* удалено: слот сохранён, чтобы не менять Store v3 */
  SET_AF_RX_300,
  SET_AF_RX_3K,
  SET_AF_TX_300,
  SET_AF_TX_3K,
  SET_SCANDELAY,
  SET_COUNT
} Setting;

extern int32_t gSettings[SET_COUNT];

void Settings_LoadDefaults(void);
int32_t Settings_Get(Setting s);
void Settings_Set(Setting s, int32_t v);
void Settings_IncDec(Setting s, bool inc);
bool Settings_IsNa(Setting s);
/* Текущее значение для меню; буфер должен жить до следующего вызова. */
const char *Settings_ValueString(Setting s);

/* Время автозатемнения и ШИМ подсветки (0 — не затемнять). */
uint32_t Settings_BacklightDimMs(void);
uint16_t Settings_BacklightHighDuty(void);
uint16_t Settings_BacklightLowDuty(void);
/* Таймаут HOLD сканера; 0xFFFFFFFF — не завершать по времени. */
uint32_t Settings_ScanTimeoutMs(Setting s);
/* Ограничение времени передачи (TOT); 0 — без ограничения. */
uint32_t Settings_TxTimeoutMs(void);
/* Стартовый режим из Main app. */
uint8_t Settings_MainApp(void);
/* Кривая гаммы ST7789: 0 — G2.2, 1 — G1.8, 2 — G2.5, 3 — G1.0. */
uint8_t Settings_GammaCurve(void);

#endif /* RT880_SETTINGS_H */
