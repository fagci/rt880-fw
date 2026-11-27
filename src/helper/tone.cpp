#include "tone.h"

#include "settings.h"
#include "../driver/bk4819-regs.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include <stdio.h>
#include <string.h>

#define TONE_SCAN_MS 650       /* REG_51=0x0300 -> результат REG_68/69 */
#define TONE_DTMF_POLL_MS 20   /* опрос REG_0C/REG_02 */
#define TONE_DTMF_HOLD_MS 3000 /* сколько показывать последнюю посылку */
#define TONE_DTMF_MAX 12
#define TONE_TEXT_MAX 20

/* Каталог CTCSS: частота в десятых герца (675 = 67.5 Гц). */
static const uint16_t CTCSS_OPTIONS[50] = {
    670,  693,  719,  744,  770,  797,  825,  854,  885,  915,
    948,  974,  1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273,
    1318, 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679,
    1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928, 1966, 1995,
    2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541};

/* DCS-коды (9 бит, печатаются как три восьмеричные цифры). */
static const uint16_t DCS_OPTIONS[104] = {
    0x0013, 0x0015, 0x0016, 0x0019, 0x001A, 0x001E, 0x0023, 0x0027, 0x0029,
    0x002B, 0x002C, 0x0035, 0x0039, 0x003A, 0x003B, 0x003C, 0x004C, 0x004D,
    0x004E, 0x0052, 0x0055, 0x0059, 0x005A, 0x005C, 0x0063, 0x0065, 0x006A,
    0x006D, 0x006E, 0x0072, 0x0075, 0x007A, 0x007C, 0x0085, 0x008A, 0x0093,
    0x0095, 0x0096, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A9, 0x00AA, 0x00AD,
    0x00B1, 0x00B3, 0x00B5, 0x00B6, 0x00B9, 0x00BC, 0x00C6, 0x00C9, 0x00CD,
    0x00D5, 0x00D9, 0x00DA, 0x00E3, 0x00E6, 0x00E9, 0x00EE, 0x00F4, 0x00F5,
    0x00F9, 0x0109, 0x010A, 0x010B, 0x0113, 0x0119, 0x011A, 0x0125, 0x0126,
    0x012A, 0x012C, 0x012D, 0x0132, 0x0134, 0x0135, 0x0136, 0x0143, 0x0146,
    0x014E, 0x0153, 0x0156, 0x015A, 0x0166, 0x0175, 0x0186, 0x018A, 0x0194,
    0x0197, 0x0199, 0x019A, 0x01AC, 0x01B2, 0x01B4, 0x01C3, 0x01CA, 0x01D3,
    0x01D9, 0x01DA, 0x01DC, 0x01E3, 0x01EC};

static const char DTMF_CHARS[] = "0123456789ABCD*#";

static uint32_t s_freq;
static uint8_t s_chip;
static bool s_hwOn;

static bool s_scanning;
static bool s_scanned;
static uint32_t s_scanAt;

static bool s_hasCtcss;
static uint8_t s_ctcssIdx;
static bool s_hasDcs;
static uint8_t s_dcsIdx;

static char s_dtmf[TONE_DTMF_MAX];
static uint8_t s_dtmfLen;
static uint32_t s_dtmfLastMs;
static uint32_t s_lastPollMs;

static char s_text[TONE_TEXT_MAX];

static uint32_t dcsGolay(uint32_t codeWord) {
  uint32_t word = codeWord;
  for (uint8_t i = 0; i < 12; i++) {
    word <<= 1;
    if (word & 0x1000)
      word ^= 0x08EA;
  }
  return codeWord | ((word & 0x0FFE) << 11);
}

/* DCS_GetGolayCodeWord(CODE_TYPE_REVERSE_DIGITAL, idx). */
static uint32_t dcsCodeword(uint8_t idx) {
  return dcsGolay(DCS_OPTIONS[idx] + 0x800U) ^ 0x7FFFFFU;
}

static uint8_t dcsFind(uint32_t code) {
  for (uint8_t i = 0; i < 23; i++) {
    if (((code >> 9) & 0x7U) == 4) {
      for (uint8_t j = 0; j < 104; j++)
        if (DCS_OPTIONS[j] == (code & 0x1FF) && dcsCodeword(j) == code)
          return j;
    }
    uint32_t shift = code >> 1;
    if (code & 1U)
      shift |= 0x400000U;
    code = shift;
  }
  return 0xFF;
}

static uint8_t ctcssFind(uint16_t code) {
  uint8_t result = 0;
  int smallest = 0x7FFF;
  for (uint8_t i = 0; i < 50; i++) {
    int delta = (int)code - (int)CTCSS_OPTIONS[i];
    if (delta < 0)
      delta = -delta;
    if (delta < smallest) {
      smallest = delta;
      result = i;
    }
  }
  return result;
}

static void clearSubtones(void) {
  s_hasCtcss = false;
  s_hasDcs = false;
}

static void clearAll(void) {
  clearSubtones();
  s_dtmfLen = 0;
  s_dtmf[0] = 0;
  s_text[0] = 0;
}

static void hwDisable(void) {
  if (!s_hwOn)
    return;
  BK4819_SelectChip(s_chip);
  if (s_scanning)
    BK4819_DisableCxCSSScan();
  BK4819_DisableDTMF();
  BK4819_WriteRegister(BK4819_REG_3F, 0);
  s_scanning = false;
  s_hwOn = false;
}

static void hwEnable(uint8_t chip) {
  BK4819_SelectChip(chip);
  BK4819_EnableDTMF();
  BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_3F_DTMF_5TONE_FOUND);
  s_chip = chip;
  s_hwOn = true;
}

static void startScan(void) {
  clearSubtones();
  /* Выбросить результат предыдущей частоты. */
  (void)BK4819_ReadRegister(BK4819_REG_69);
  (void)BK4819_ReadRegister(BK4819_REG_68);
  BK4819_EnableCxCSSScan();
  s_scanning = true;
  s_scanAt = millis();
}

static void finishScan(void) {
  uint32_t cdcss = 0;
  uint16_t ctcss = 0;
  BK4819_CssScanResult_t res =
      BK4819_GetCxCSSScanResult(&cdcss, &ctcss);
  BK4819_DisableCxCSSScan();
  s_scanning = false;
  s_scanned = true;

  if (res == BK4819_CSS_RESULT_CTCSS) {
    s_ctcssIdx = ctcssFind(ctcss);
    s_hasCtcss = true;
  } else if (res == BK4819_CSS_RESULT_CDCSS) {
    s_dcsIdx = dcsFind(cdcss);
    s_hasDcs = true;
  }
}

static void dtmfPush(char c) {
  if (s_dtmfLen >= TONE_DTMF_MAX - 1) {
    memmove(s_dtmf, s_dtmf + 1, TONE_DTMF_MAX - 2);
    s_dtmfLen = TONE_DTMF_MAX - 2;
  }
  s_dtmf[s_dtmfLen++] = c;
  s_dtmf[s_dtmfLen] = 0;
  s_dtmfLastMs = millis();
}

static void refreshText(void) {
  uint32_t now = millis();
  if (s_dtmfLen && (uint32_t)(now - s_dtmfLastMs) <= TONE_DTMF_HOLD_MS) {
    const char *digits = s_dtmf;
    if (s_dtmfLen > 8)
      digits += s_dtmfLen - 8;
    snprintf(s_text, sizeof(s_text), "DTMF %s", digits);
  } else if (s_hasCtcss) {
    uint16_t f = CTCSS_OPTIONS[s_ctcssIdx];
    snprintf(s_text, sizeof(s_text), "CT %u.%u", (unsigned)(f / 10),
             (unsigned)(f % 10));
  } else if (s_hasDcs) {
    if (s_dcsIdx < 104)
      snprintf(s_text, sizeof(s_text), "DCS %03o",
               (unsigned)DCS_OPTIONS[s_dcsIdx]);
    else
      snprintf(s_text, sizeof(s_text), "DCS ?");
  } else {
    s_text[0] = 0;
  }
}

void Tone_Reset(void) {
  hwDisable();
  clearAll();
  s_freq = 0;
  s_scanned = false;
}

void Tone_Update(bool enabled, uint8_t chip, uint32_t freq, bool squelchOpen) {
  uint32_t now = millis();

  if (!enabled) {
    hwDisable();
    clearAll();
    s_freq = 0;
    s_scanned = false;
    return;
  }

  if (!s_hwOn || chip != s_chip) {
    hwDisable();
    hwEnable(chip);
    clearAll();
    s_freq = freq;
    s_scanned = false;
  } else if (freq != s_freq) {
    if (s_scanning) {
      BK4819_DisableCxCSSScan();
      s_scanning = false;
    }
    clearAll();
    s_freq = freq;
    s_scanned = false;
  }

  if (!squelchOpen) {
    /* Сигнал пропал: скан не держим, но результат оставляем на экране. */
    if (s_scanning) {
      BK4819_DisableCxCSSScan();
      s_scanning = false;
    }
    s_scanned = false;
  } else if (!s_scanning && !s_scanned) {
    startScan();
  } else if (s_scanning && (uint32_t)(now - s_scanAt) >= TONE_SCAN_MS) {
    finishScan();
  }

  if (s_dtmfLen && (uint32_t)(now - s_dtmfLastMs) > TONE_DTMF_HOLD_MS) {
    s_dtmfLen = 0;
    s_dtmf[0] = 0;
  }
  if (!Settings_Get(SET_DTMFDECODE)) {
    s_dtmfLen = 0;
    s_dtmf[0] = 0;
  } else if ((uint32_t)(now - s_lastPollMs) >= TONE_DTMF_POLL_MS) {
    s_lastPollMs = now;
    if (BK4819_ReadRegister(BK4819_REG_0C) & 1) {
      BK4819_WriteRegister(BK4819_REG_02, 0);
      uint16_t bits = BK4819_ReadRegister(BK4819_REG_02);
      if (bits & BK4819_REG_02_DTMF_5TONE_FOUND) {
        uint8_t code = BK4819_GetDTMF_5TONE_Code();
        if (code < 16)
          dtmfPush(DTMF_CHARS[code]);
      }
    }
  }
}

void Tone_GetStatus(char *buf, size_t n) {
  if (!buf || !n)
    return;
  refreshText();
  strncpy(buf, s_text, n);
  buf[n - 1] = 0;
}

uint8_t Tone_GetCode(void) {
  if (s_hasCtcss)
    return s_ctcssIdx;
  if (s_hasDcs && s_dcsIdx < 104)
    return (uint8_t)(TONE_CODE_DCS | s_dcsIdx);
  return TONE_CODE_NONE;
}

void Tone_CodeString(uint8_t code, char *buf, size_t n) {
  if (!buf || !n)
    return;
  buf[0] = 0;
  if (code == TONE_CODE_NONE)
    return;
  if (code & TONE_CODE_DCS) {
    uint8_t idx = (uint8_t)(code & 0x7Fu);
    if (idx < 104)
      snprintf(buf, n, "DCS %03o", (unsigned)DCS_OPTIONS[idx]);
    return;
  }
  if (code < 50) {
    uint16_t f = CTCSS_OPTIONS[code];
    snprintf(buf, n, "CT %u.%u", (unsigned)(f / 10), (unsigned)(f % 10));
  }
}
