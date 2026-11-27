#include "settings.h"

#include "measurements.h"
#include "store.h"
#include "unit_fmt.h"

#include "../driver/bk4819.h"
#include "../driver/st7789.h"
#include "../radio.h"

#include <stdio.h>
#include <string.h>

int32_t gSettings[SET_COUNT];

static const char *const YES_NO[] = {"No", "Yes"};
static const char *const ON_OFF[] = {"Off", "On"};
static const char *const BL_SQL[] = {"Off", "On", "Open"};
static const char *const CH_DISPLAY[] = {"Name+F", "F", "Name"};
static const char *const MAIN_APP[] = {"VFO", "BAND", "Scanner", "Analyser"};
static const char *const BAT_TYPE[] = {"1400", "1600", "2200", "3500"};
static const char *const BAT_STYLE[] = {"Clean", "Percent", "Volt"};
static const char *const FC_TIME[] = {"0.2s", "0.4s", "0.8s", "1.6s"};
static const char *const MULTIWATCH[] = {"Off", "On", "Switch", "Extra"};
static const char *const TX_POWER[] = {"Low", "Mid", "High"};
static const char *const GAMMA_NAMES[] = {"G2.2", "G1.8", "G2.5", "G1.0"};
static const char *const TX_TIME[] = {"Off", "30s", "60s", "2m", "5m"};
static const uint32_t TX_TIMEOUTS[] = {0, 30000, 60000, 120000, 300000};
static const char *const SCAN_TIMEOUT_NAMES[] = {
    "0",    "100ms", "250ms", "500ms", "1s",  "2s",  "5s",
    "10s",  "30s",   "1m",    "2m",    "5m",  "None",
};
static const uint32_t SCAN_TIMEOUTS[] = {
    0,    100,  250,  500,  1000, 2000, 5000,
    10000, 30000, 60000, 120000, 300000, 0xFFFFFFFFu,
};

static const uint8_t BL_TIME_VALUES[] = {0, 5, 10, 20, 60, 120, 255};

static char s_valueBuf[16];

void Settings_LoadDefaults(void) {
  memset(gSettings, 0, sizeof(gSettings));
  gSettings[SET_EEPROMTYPE] = 6; /* unknown */
  gSettings[SET_BATSAVE] = 4;
  gSettings[SET_BACKLIGHT] = 3; /* 20 s */
  gSettings[SET_CHDISPLAYMODE] = 0;
  gSettings[SET_DTMFDECODE] = 1;
  gSettings[SET_BRIGHTNESS_H] = 8;
  gSettings[SET_BRIGHTNESS_L] = 2;
  gSettings[SET_GAMMA] = 0;
  gSettings[SET_MAINAPP] = 0;
  gSettings[SET_TX_ENABLE] = 0; /* передача выключена по умолчанию */
  gSettings[SET_TXPOWER] = 0;   /* Low — безопасный уровень */
  gSettings[SET_TXTIME] = 2;    /* 60 s TOT */
  gSettings[SET_MIC] = 10;
  gSettings[SET_SQOPENEDTIMEOUT] = 12; /* None — как в k1-fw */
  gSettings[SET_SQCLOSEDTIMEOUT] = 5;  /* 2 s */
  gSettings[SET_SQLOPENTIME] = 1;
  gSettings[SET_SQLCLOSETIME] = 1;
  gSettings[SET_SKIPGARBAGEFREQUENCIES] = 1;
  gSettings[SET_BACKLIGHTONSQUELCH] = 1;
  gSettings[SET_BATTERYCALIBRATION] = 2184;
  gSettings[SET_BATTERYTYPE] = 1;
  gSettings[SET_BATTERYSTYLE] = 1;
  gSettings[SET_DEVIATION] = 130;
  gSettings[SET_STE] = 1;
  gSettings[SET_SHOWLEVELINVFO] = 1;
  gSettings[SET_FCTIME] = 0;
  gSettings[SET_AF_RX_300] = 4;
  gSettings[SET_AF_RX_3K] = 4;
  gSettings[SET_AF_TX_300] = 4;
  gSettings[SET_AF_TX_3K] = 4;
  gSettings[SET_SCANDELAY] = 1800;
}

int32_t Settings_Get(Setting s) {
  if (s < 0 || s >= SET_COUNT)
    return 0;
  return gSettings[s];
}

static int32_t gammaCurve(void) {
  int32_t v = Settings_Get(SET_GAMMA);
  return (v >= 0 && v < 4) ? v : 0;
}

bool Settings_IsNa(Setting s) {
  switch (s) {
  case SET_TX_ENABLE:
  case SET_TXPOWER:
  case SET_TXTIME:
  case SET_PTT_LOCK:
  case SET_BUSYCHANNELTXLOCK:
  case SET_MIC:
  case SET_BACKLIGHT:
  case SET_CHDISPLAYMODE:
  case SET_DTMFDECODE:
  case SET_BRIGHTNESS_H:
  case SET_BRIGHTNESS_L:
  case SET_GAMMA:
  case SET_MAINAPP:
  case SET_SQOPENEDTIMEOUT:
  case SET_SQCLOSEDTIMEOUT:
  case SET_SQLOPENTIME:
  case SET_SQLCLOSETIME:
  case SET_SKIPGARBAGEFREQUENCIES:
  case SET_SCANDELAY:
  case SET_BATTERYSTYLE:
  case SET_SHOWLEVELINVFO:
  case SET_BOUND240_280:
  case SET_FCTIME:
  case SET_SI4732POWEROFF:
  case SET_UPCONVERTER:
  case SET_STE:
  case SET_DEVIATION:
    return false;
  default:
    return true;
  }
}

static int32_t settingMax(Setting s) {
  switch (s) {
  case SET_EEPROMTYPE:
    return 6;
  case SET_BACKLIGHT:
    return 7;
  case SET_CHDISPLAYMODE:
    return 3;
  case SET_MAINAPP:
    return 4;
  case SET_SQOPENEDTIMEOUT:
  case SET_SQCLOSEDTIMEOUT:
    return 13;
  case SET_SQLOPENTIME:
    return 8;
  case SET_SQLCLOSETIME:
    return 4;
  case SET_BRIGHTNESS_H:
  case SET_BRIGHTNESS_L:
    return 9;
  case SET_GAMMA:
    return 4;
  case SET_BATTERYTYPE:
    return 4;
  case SET_BATTERYSTYLE:
    return 3;
  case SET_FCTIME:
    return 4;
  case SET_MULTIWATCH:
    return 4;
  case SET_TXPOWER:
    return 3;
  case SET_BACKLIGHTONSQUELCH:
    return 3;
  case SET_AF_RX_300:
  case SET_AF_RX_3K:
  case SET_AF_TX_300:
  case SET_AF_TX_3K:
    return 9;
  case SET_DEVIATION:
    return 256;
  case SET_MIC:
    return 32;
  case SET_TXTIME:
    return 5;
  case SET_SCANDELAY:
    return 10001; /* 100..10000 мкс */
  case SET_UPCONVERTER:
    return 1340000000; /* Гц, шаг 1 МГц */
  default:
    return 2; /* флаги Off/On */
  }
}

static void settingsApply(Setting s) {
  switch (s) {
  case SET_BACKLIGHT:
  case SET_BRIGHTNESS_H:
  case SET_BRIGHTNESS_L:
    st7789_backlight_set(Settings_BacklightHighDuty());
    break;
  case SET_GAMMA:
    st7789_set_gamma(Settings_GammaCurve());
    break;
  case SET_SQLOPENTIME:
  case SET_SQLCLOSETIME:
    Radio_SetSquelch(squelchLevel);
    break;
  case SET_BOUND240_280:
    if (filterIndex < 0)
      BK4819_SelectFilterByFrequency(vfos[currentVfo].rxF);
    break;
  case SET_UPCONVERTER:
  case SET_SI4732POWEROFF:
    Radio_TuneTo(vfos[currentVfo].rxF, true);
    break;
  case SET_DEVIATION: {
    uint16_t dev = (uint16_t)Settings_Get(SET_DEVIATION) * 10u;
    BK4819_SelectChip(0);
    BK4819_SetDeviation(dev);
    break;
  }
  default:
    break;
  }
}

void Settings_Set(Setting s, int32_t v) {
  if (s < 0 || s >= SET_COUNT || Settings_IsNa(s))
    return;
  int32_t max = settingMax(s);
  if (v < 0)
    v = 0;
  if (v >= max)
    v = max - 1;
  if (gSettings[s] == v)
    return;
  gSettings[s] = v;
  settingsApply(s);
  Store_MarkDirty();
}

void Settings_IncDec(Setting s, bool inc) {
  if (Settings_IsNa(s))
    return;
  int32_t max = settingMax(s);
  if (s == SET_UPCONVERTER) {
    /* 1 МГц: 1.34 ГГц делится на шаг без остатка. */
    int32_t v = (int32_t)AdjustU((uint32_t)Settings_Get(s), 0, (uint32_t)max,
                                 inc ? 1000000 : -1000000);
    Settings_Set(s, v);
    return;
  }
  if (s == SET_SCANDELAY) {
    /* Шаг 100 мкс (как в k1-fw); по краям — переход на другой конец. */
    int32_t v = Settings_Get(s) + (inc ? 100 : -100);
    if (v < 100)
      v = 10000;
    if (v > 10000)
      v = 100;
    Settings_Set(s, v);
    return;
  }
  int32_t cur = Settings_Get(s);
  if (s == SET_GAMMA && (cur < 0 || cur >= max))
    cur = 0;
  int32_t v = (int32_t)IncDecU((uint32_t)cur, 0, (uint32_t)max, inc);
  Settings_Set(s, v);
}

const char *Settings_ValueString(Setting s) {
  if (s < 0 || s >= SET_COUNT || Settings_IsNa(s))
    return "N/A";
  int32_t v = Settings_Get(s);
  switch (s) {
  case SET_CHDISPLAYMODE:
    return CH_DISPLAY[v % 3];
  case SET_MAINAPP:
    return MAIN_APP[v % 4];
  case SET_SQOPENEDTIMEOUT:
  case SET_SQCLOSEDTIMEOUT:
    return SCAN_TIMEOUT_NAMES[v % 13];
  case SET_BATTERYTYPE:
    return BAT_TYPE[v % 4];
  case SET_BATTERYSTYLE:
    return BAT_STYLE[v % 3];
  case SET_FCTIME:
    return FC_TIME[v % 4];
  case SET_MULTIWATCH:
    return MULTIWATCH[v % 4];
  case SET_BACKLIGHTONSQUELCH:
    return BL_SQL[v % 3];
  case SET_TXPOWER:
    return TX_POWER[v % 3];
  case SET_TXTIME:
    return TX_TIME[v % 5];
  case SET_BACKLIGHT:
    if (BL_TIME_VALUES[v] == 0)
      return ON_OFF[0];
    if (BL_TIME_VALUES[v] == 255)
      return ON_OFF[1];
    snprintf(s_valueBuf, sizeof(s_valueBuf), "%us", BL_TIME_VALUES[v]);
    break;
  case SET_TX_ENABLE:
  case SET_PTT_LOCK:
  case SET_BUSYCHANNELTXLOCK:
    return ON_OFF[v ? 1 : 0];
  case SET_DTMFDECODE:
  case SET_SKIPGARBAGEFREQUENCIES:
  case SET_SHOWLEVELINVFO:
  case SET_STE:
  case SET_SI4732POWEROFF:
    return YES_NO[v ? 1 : 0];
  case SET_BOUND240_280:
    return v ? "280" : "240";
  case SET_UPCONVERTER:
    if (v == 0)
      return "Off";
    fmt_mhz3(s_valueBuf, sizeof(s_valueBuf), (uint32_t)v / 10u);
    break;
  case SET_SQLOPENTIME:
  case SET_SQLCLOSETIME:
    snprintf(s_valueBuf, sizeof(s_valueBuf), "%ums", (unsigned)(v * 5));
    break;
  case SET_SCANDELAY:
    snprintf(s_valueBuf, sizeof(s_valueBuf), "%uus", (unsigned)v);
    break;
  case SET_BRIGHTNESS_H:
  case SET_BRIGHTNESS_L:
  case SET_MIC:
    snprintf(s_valueBuf, sizeof(s_valueBuf), "%u", (unsigned)v);
    break;
  case SET_GAMMA:
    return GAMMA_NAMES[gammaCurve()];
  default:
    snprintf(s_valueBuf, sizeof(s_valueBuf), "%ld", (long)v);
    break;
  }
  return s_valueBuf;
}

uint32_t Settings_BacklightDimMs(void) {
  uint8_t idx = (uint8_t)Settings_Get(SET_BACKLIGHT);
  if (idx >= sizeof(BL_TIME_VALUES))
    idx = 0;
  uint8_t sec = BL_TIME_VALUES[idx];
  if (sec == 0 || sec == 255)
    return 0;
  return (uint32_t)sec * 1000u;
}

uint16_t Settings_BacklightHighDuty(void) {
  uint16_t v = (uint16_t)Settings_Get(SET_BRIGHTNESS_H);
  if (v > 8)
    v = 8;
  return (uint16_t)((uint32_t)v * ST7789_BACKLIGHT_MAX / 8u);
}

uint16_t Settings_BacklightLowDuty(void) {
  uint16_t v = (uint16_t)Settings_Get(SET_BRIGHTNESS_L);
  if (v > 8)
    v = 8;
  return (uint16_t)((uint32_t)v * ST7789_BACKLIGHT_MAX / 8u);
}

uint32_t Settings_ScanTimeoutMs(Setting s) {
  int32_t idx = Settings_Get(s);
  if (idx < 0 || idx >= (int32_t)(sizeof(SCAN_TIMEOUTS) / sizeof(SCAN_TIMEOUTS[0])))
    return 0;
  return SCAN_TIMEOUTS[idx];
}

uint32_t Settings_TxTimeoutMs(void) {
  int32_t idx = Settings_Get(SET_TXTIME);
  if (idx < 0 || idx >= (int32_t)(sizeof(TX_TIMEOUTS) / sizeof(TX_TIMEOUTS[0])))
    return 60000;
  return TX_TIMEOUTS[idx];
}

uint8_t Settings_MainApp(void) {
  int32_t v = Settings_Get(SET_MAINAPP);
  if (v < 0 || v > 3)
    return 0;
  return (uint8_t)v;
}

uint8_t Settings_GammaCurve(void) { return (uint8_t)gammaCurve(); }
