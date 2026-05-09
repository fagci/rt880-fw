#include "bk4819.h"
#include "at32f423.h"

/* BK4819 #1: SCN PA8, SDA PC9, SCK PF6 */
#define BK1_SCN_PIN GPIO_PINS_8
#define BK1_SCN_PORT GPIOA
#define BK1_SDA_PIN GPIO_PINS_9
#define BK1_SDA_PORT GPIOC
#define BK1_SCK_PIN GPIO_PINS_6
#define BK1_SCK_PORT GPIOF

/* BK4819 #2: SCN PC7, SDA PC6, SCK PC8 */
#define BK2_SCN_PIN GPIO_PINS_7
#define BK2_SCN_PORT GPIOC
#define BK2_SDA_PIN GPIO_PINS_6
#define BK2_SDA_PORT GPIOC
#define BK2_SCK_PIN GPIO_PINS_8
#define BK2_SCK_PORT GPIOC

#define PIN_SET(port, pin) ((port)->scr = (pin))
#define PIN_CLR(port, pin) ((port)->clr = (pin))
#define PIN_GET(port, pin) ((port)->idt & (pin))

static struct {
  gpio_type *scn_port;
  uint16_t scn_pin;
  gpio_type *sda_port;
  uint16_t sda_pin;
  gpio_type *sck_port;
  uint16_t sck_pin;
} g_bk = {0};

static void bk_delay(void) {
  volatile uint32_t i;
  for (i = 0; i < 4; i++)
    ;
}

static void gpio_set_output(gpio_type *port, uint16_t pin) {
  gpio_init_type g;
  gpio_default_para_init(&g);
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_mode = GPIO_MODE_OUTPUT;
  g.gpio_pins = pin;
  g.gpio_pull = GPIO_PULL_NONE;
  gpio_init(port, &g);
}

static void sda_input(void) {
  gpio_init_type g;
  gpio_default_para_init(&g);
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_mode = GPIO_MODE_INPUT;
  g.gpio_pins = g_bk.sda_pin;
  g.gpio_pull = GPIO_PULL_NONE;
  gpio_init(g_bk.sda_port, &g);
}

static void sda_output(void) { gpio_set_output(g_bk.sda_port, g_bk.sda_pin); }

void BK4819_SelectChip(uint8_t chip) {
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOF_PERIPH_CLOCK, TRUE);

  if (chip == 0) {
    g_bk.scn_port = BK1_SCN_PORT;
    g_bk.scn_pin = BK1_SCN_PIN;
    g_bk.sda_port = BK1_SDA_PORT;
    g_bk.sda_pin = BK1_SDA_PIN;
    g_bk.sck_port = BK1_SCK_PORT;
    g_bk.sck_pin = BK1_SCK_PIN;
  } else {
    g_bk.scn_port = BK2_SCN_PORT;
    g_bk.scn_pin = BK2_SCN_PIN;
    g_bk.sda_port = BK2_SDA_PORT;
    g_bk.sda_pin = BK2_SDA_PIN;
    g_bk.sck_port = BK2_SCK_PORT;
    g_bk.sck_pin = BK2_SCK_PIN;
  }

  gpio_set_output(g_bk.scn_port, g_bk.scn_pin);
  gpio_set_output(g_bk.sda_port, g_bk.sda_pin);
  gpio_set_output(g_bk.sck_port, g_bk.sck_pin);

  PIN_SET(g_bk.scn_port, g_bk.scn_pin);
  PIN_SET(g_bk.sda_port, g_bk.sda_pin);
  PIN_SET(g_bk.sck_port, g_bk.sck_pin);
}

static void write_u8(uint8_t data) {
  for (int i = 0; i < 8; i++) {
    if (data & 0x80)
      PIN_SET(g_bk.sda_port, g_bk.sda_pin);
    else
      PIN_CLR(g_bk.sda_port, g_bk.sda_pin);
    bk_delay();
    PIN_CLR(g_bk.sck_port, g_bk.sck_pin);
    bk_delay();
    PIN_SET(g_bk.sck_port, g_bk.sck_pin);
    bk_delay();
    data <<= 1;
  }
}

static void write_u16(uint16_t data) {
  for (int i = 0; i < 16; i++) {
    if (data & 0x8000)
      PIN_SET(g_bk.sda_port, g_bk.sda_pin);
    else
      PIN_CLR(g_bk.sda_port, g_bk.sda_pin);
    bk_delay();
    PIN_CLR(g_bk.sck_port, g_bk.sck_pin);
    bk_delay();
    PIN_SET(g_bk.sck_port, g_bk.sck_pin);
    bk_delay();
    data <<= 1;
  }
}

static uint16_t read_u16(void) {
  sda_input();
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) {
    PIN_SET(g_bk.sck_port, g_bk.sck_pin);
    bk_delay();
    PIN_CLR(g_bk.sck_port, g_bk.sck_pin);
    bk_delay();
    v <<= 1;
    if (PIN_GET(g_bk.sda_port, g_bk.sda_pin))
      v |= 1;
  }
  sda_output();
  return v;
}

void BK4819_WriteRegister(uint8_t reg, uint16_t data) {
  PIN_SET(g_bk.scn_port, g_bk.scn_pin);
  PIN_SET(g_bk.sck_port, g_bk.sck_pin);
  bk_delay();
  PIN_CLR(g_bk.scn_port, g_bk.scn_pin);
  write_u8(reg);
  write_u16(data);
  PIN_SET(g_bk.scn_port, g_bk.scn_pin);
  bk_delay();
  PIN_SET(g_bk.sck_port, g_bk.sck_pin);
  PIN_SET(g_bk.sda_port, g_bk.sda_pin);
}

uint16_t BK4819_ReadRegister(uint8_t reg) {
  PIN_SET(g_bk.scn_port, g_bk.scn_pin);
  PIN_SET(g_bk.sck_port, g_bk.sck_pin);
  bk_delay();
  PIN_CLR(g_bk.scn_port, g_bk.scn_pin);
  write_u8(reg | 0x80);
  uint16_t v = read_u16();
  PIN_SET(g_bk.scn_port, g_bk.scn_pin);
  bk_delay();
  PIN_SET(g_bk.sck_port, g_bk.sck_pin);
  PIN_SET(g_bk.sda_port, g_bk.sda_pin);
  return v;
}

static uint16_t gGpioOutState = 0x9000;
static uint8_t gSelectedFilter = 255;
static uint32_t gFreqCacheLow = 0;
static uint32_t gFreqCacheHigh = 0;
static ModulationType gLastModulation = 255;

static const uint16_t MOD_TYPE_REG47_VALUES[] = {
    [MOD_FM] = 0,  [MOD_AM] = 4,  [MOD_LSB] = 3, [MOD_USB] = 3,
    [MOD_BYP] = 8, [MOD_RAW] = 5, [MOD_WFM] = 0,
};

static const uint8_t sqTypeValues[4] = {0x88, 0xAA, 0xCC, 0xFF};
static const uint8_t DTMF_COEFFS[] = {111, 107, 103, 98, 80,  71,  58,  44,
                                      65,  55,  37,  23, 228, 203, 181, 159};

const Gain gainTable[32] = {
    {0x10, 90},  {0x1, 88},   {0x9, 87},   {0x2, 83},   {0xA, 81},
    {0x12, 79},  {0x2A, 77},  {0x32, 75},  {0x3A, 70},  {0x20B, 68},
    {0x213, 64}, {0x21B, 62}, {0x214, 59}, {0x21C, 56}, {0x22D, 52},
    {0x23C, 50}, {0x23D, 48}, {0x255, 44}, {0x25D, 42}, {0x275, 39},
    {0x295, 33}, {0x2B6, 31}, {0x354, 28}, {0x36C, 23}, {0x38C, 20},
    {0x38D, 17}, {0x3B5, 13}, {0x3B6, 9},  {0x3D6, 8},  {0x3BF, 3},
    {0x3DF, 2},  {0x3FF, 0},
};

static inline uint16_t scale_freq(const uint16_t freq) {
  return ((uint32_t)freq * 1353245u + (1u << 16)) >> 17;
}

void BK4819_Idle(void) { BK4819_WriteRegister(BK4819_REG_30, 0x0000); }

void BK4819_Init(void) {
  PIN_SET(g_bk.scn_port, g_bk.scn_pin);
  PIN_SET(g_bk.sck_port, g_bk.sck_pin);
  PIN_SET(g_bk.sda_port, g_bk.sda_pin);

  BK4819_WriteRegister(BK4819_REG_00, 0x8000);
  BK4819_WriteRegister(BK4819_REG_00, 0x0000);
  BK4819_WriteRegister(BK4819_REG_37, 0x1D0F);

  gGpioOutState = 1 << 4;
  BK4819_WriteRegister(BK4819_REG_33, gGpioOutState);

  BK4819_SetupPowerAmplifier(0, 0);

  BK4819_WriteRegister(0x1E, 0x4c58);
  BK4819_WriteRegister(BK4819_REG_1F, 0x5454);

  BK4819_SetAGC(true, 0);

  BK4819_WriteRegister(BK4819_REG_3F, 0);
  BK4819_WriteRegister(BK4819_REG_3E, 0xA037);

  for (uint8_t i = 0; i < 16; i++)
    BK4819_WriteRegister(0x09, (i << 12) | DTMF_COEFFS[i]);

  BK4819_WriteRegister(BK4819_REG_48,
                       (11u << 12) | (0 << 10) | (58 << 4) | (8 << 0));

  rt880_delay_ms(10);
  BK4819_WriteRegister(BK4819_REG_3F, 0);

  BK4819_WriteRegister(BK4819_REG_19, 0x1041);
  BK4819_WriteRegister(BK4819_REG_7D, 0xE940 | 10);
  BK4819_WriteRegister(0x74, 0xAF1F);

  BK4819_DisableDTMF();

  BK4819_WriteRegister(0x40, (BK4819_ReadRegister(0x40) & ~(0x7FF)) |
                                 (50 * 10) | (1 << 12));
}

void BK4819_SetAGC(bool useDefault, uint8_t gainIndex) {
  const bool enableAgc = (gainIndex == AUTO_GAIN_INDEX);
  uint16_t regVal = BK4819_ReadRegister(BK4819_REG_7E);

  BK4819_WriteRegister(BK4819_REG_7E, (regVal & ~(1 << 15) & ~(0b111 << 12)) |
                                          (!enableAgc << 15) | (3u << 12) |
                                          (5u << 3) | (6u << 0));

  if (enableAgc)
    BK4819_WriteRegister(BK4819_REG_13, 0x03BE);
  else
    BK4819_WriteRegister(BK4819_REG_13, gainTable[gainIndex].regValue);

  BK4819_WriteRegister(0x12, (3u << 8) | (3u << 5) | (3u << 3) | (4u << 0));
  BK4819_WriteRegister(0x11, (2u << 8) | (3u << 5) | (3u << 3) | (3u << 0));
  BK4819_WriteRegister(0x10, (0u << 8) | (3u << 5) | (3u << 3) | (2u << 0));
  BK4819_WriteRegister(0x14, (0u << 8) | (0u << 5) | (3u << 3) | (1u << 0));

  BK4819_WriteRegister(BK4819_REG_49, (0 << 14) | (84 << 7) | (56 << 0));
  BK4819_WriteRegister(BK4819_REG_7B, 0x8420);
}

void BK4819_SetAFC(uint8_t level) {
  if (level == 0) {
    BK4819_WriteRegister(0x73, (8 - level) << 11);
  } else {
    BK4819_WriteRegister(0x73, (7 << 11) | (1 << 4));
  }
}

static void BK4819_SetVariableCaliper(int32_t var) {
  if (var >= 0) {
    BK4819_WriteRegister(0x72, var);
    BK4819_WriteRegister(0x71, BK4819_ReadRegister(0x71) & ~0x1000);
  } else {
    BK4819_WriteRegister(0x72, -var);
    BK4819_WriteRegister(0x71, BK4819_ReadRegister(0x71) | 0x1000);
  }
}

void BK4819_SetFrequency(uint32_t freq) {
  uint16_t low = freq & 0xFFFF;
  uint16_t high = (freq >> 16) & 0xFFFF;

  if (low != gFreqCacheLow) {
    BK4819_WriteRegister(BK4819_REG_38, low);
    gFreqCacheLow = low;
  }

  if (high != gFreqCacheHigh) {
    BK4819_WriteRegister(BK4819_REG_39, high);
    gFreqCacheHigh = high;
  }
}

void BK4819_TuneTo(uint32_t freq, bool precise) {
  BK4819_SetFrequency(freq);

  uint16_t reg = BK4819_ReadRegister(BK4819_REG_30);

  if (precise)
    BK4819_WriteRegister(BK4819_REG_30, 0x0200);
  else
    BK4819_WriteRegister(BK4819_REG_30, reg & ~BK4819_REG_30_ENABLE_VCO_CALIB);

  BK4819_WriteRegister(BK4819_REG_30, reg);
}

void BK4819_RX_TurnOn(void) {
  BK4819_WriteRegister(BK4819_REG_36, 0x0000);
  BK4819_WriteRegister(BK4819_REG_37, 0x1F0F);
  BK4819_WriteRegister(BK4819_REG_30, 0x0200);

  BK4819_WriteRegister(
      BK4819_REG_30,
      BK4819_REG_30_ENABLE_VCO_CALIB | BK4819_REG_30_DISABLE_UNKNOWN |
          BK4819_REG_30_ENABLE_RX_LINK | BK4819_REG_30_ENABLE_AF_DAC |
          BK4819_REG_30_ENABLE_DISC_MODE | BK4819_REG_30_ENABLE_PLL_VCO |
          BK4819_REG_30_DISABLE_PA_GAIN | BK4819_REG_30_DISABLE_MIC_ADC |
          BK4819_REG_30_DISABLE_TX_DSP | BK4819_REG_30_ENABLE_RX_DSP);
}

void BK4819_SetFilterBandwidth(BK4819_FilterBandwidth_t bw) {
  if (bw > 9)
    return;

  static const uint8_t rf[] = {0, 1, 1, 3, 1, 2, 3, 4, 5, 7};
  static const uint8_t wb[] = {0, 0, 1, 2, 1, 2, 2, 3, 4, 6};
  static const uint8_t af[] = {1, 2, 0, 3, 0, 0, 7, 6, 5, 4};
  static const uint8_t bs[] = {1, 1, 0, 0, 2, 2, 2, 2, 2, 2};

  const uint16_t value = //
      (0u << 15)         //
      | (rf[bw] << 12)   //
      | (wb[bw] << 9)    // weak
      | (af[bw] << 6)    //
      | (bs[bw] << 4)    //
      | (1u << 3)        //
      | (0u << 2)        //
      | (0u << 0);       //

  BK4819_WriteRegister(BK4819_REG_43, value);
}

void BK4819_SetModulation(ModulationType mod) {
  BK4819_WriteRegister(BK4819_REG_47,
                       0x6040 | (MOD_TYPE_REG47_VALUES[mod] << 8));
}

void BK4819_SquelchType(SquelchType t) {
  BK4819_WriteRegister(BK4819_REG_77, sqTypeValues[t] << 8);
}

void BK4819_Squelch(uint8_t sq, uint8_t openTime, uint8_t closeTime) {
  uint16_t v = sq << 8;
  if (openTime)
    v |= ((openTime & 0x0F) << 4);
  if (closeTime)
    v |= (closeTime & 0x0F);
  BK4819_WriteRegister(BK4819_REG_6F, (sq << 8));
  BK4819_WriteRegister(BK4819_REG_4D, 0xA000 | sq);
  BK4819_WriteRegister(BK4819_REG_4E, 0x4000 | sq);
  BK4819_WriteRegister(BK4819_REG_4F, 0x0800 | sq);
  BK4819_WriteRegister(BK4819_REG_78, sq << 8);
}

bool BK4819_IsSquelchOpen(void) {
  return !(BK4819_ReadRegister(BK4819_REG_0C) & (1 << 10));
}

uint16_t BK4819_GetRSSI(void) {
  return BK4819_ReadRegister(BK4819_REG_67) & 0xFF;
}

uint16_t BK4819_GetSNR(void) {
  return BK4819_ReadRegister(BK4819_REG_69) & 0xFF;
}

uint16_t BK4819_GetNoise(void) {
  return BK4819_ReadRegister(BK4819_REG_68) & 0xFF;
}

uint16_t BK4819_GetGlitch(void) {
  return BK4819_ReadRegister(BK4819_REG_65) & 0xFF;
}

uint16_t BK4819_GetLnaPeakRSSI(void) {
  return BK4819_ReadRegister(BK4819_REG_63) & 0xFF;
}

uint16_t BK4819_GetAgcRSSI(void) {
  return BK4819_ReadRegister(BK4819_REG_64) & 0xFF;
}

void BK4819_SetupPowerAmplifier(uint8_t power, uint32_t freq) {
  if (power == 0) {
    BK4819_WriteRegister(BK4819_REG_36, 0x0000);
    BK4819_WriteRegister(BK4819_REG_37, 0x1F0F);
    BK4819_WriteRegister(
        BK4819_REG_30,
        BK4819_REG_30_ENABLE_RX_LINK | BK4819_REG_30_ENABLE_AF_DAC |
            BK4819_REG_30_ENABLE_DISC_MODE |
            (0xF << 15) | /* VCO cal + unknown */
            BK4819_REG_30_ENABLE_PLL_VCO | BK4819_REG_30_ENABLE_RX_DSP);
  }
}

void BK4819_PrepareTransmit(void) {
  BK4819_WriteRegister(BK4819_REG_48, 0);
  BK4819_WriteRegister(BK4819_REG_49, 0);
  BK4819_WriteRegister(BK4819_REG_7B, 0x8420);
}

void BK4819_ToggleGpioOut(uint8_t pin, bool on) {
  (void)pin;
  (void)on;
}

void BK4819_ToggleAFDAC(bool on) {
  uint16_t v = BK4819_ReadRegister(BK4819_REG_30);
  if (on)
    v |= BK4819_REG_30_ENABLE_AF_DAC;
  else
    v &= ~BK4819_REG_30_ENABLE_AF_DAC;
  BK4819_WriteRegister(BK4819_REG_30, v);
}

void BK4819_ToggleAFBit(bool on) {
  uint16_t v = BK4819_ReadRegister(BK4819_REG_31);
  if (on)
    v |= 2;
  else
    v &= ~2;
  BK4819_WriteRegister(BK4819_REG_31, v);
}

void BK4819_EnterSubAu(void) {
  BK4819_WriteRegister(BK4819_REG_51,
                       BK4819_ReadRegister(BK4819_REG_51) & ~0x8000);
}

void BK4819_ExitSubAu(void) {
  BK4819_WriteRegister(BK4819_REG_51,
                       BK4819_ReadRegister(BK4819_REG_51) | 0x8000);
}

void BK4819_EnableDTMF(void) {
  BK4819_WriteRegister(BK4819_REG_21, 0x06D8);
  BK4819_WriteRegister(BK4819_REG_24,
                       BK4819_REG_24_ENABLE | BK4819_REG_24_SELECT_DTMF | 1);
}

void BK4819_DisableDTMF(void) { BK4819_WriteRegister(BK4819_REG_24, 0); }

void BK4819_SetCTCSSFrequency(uint16_t freq) {
  BK4819_WriteRegister(BK4819_REG_51, BK4819_REG_51_ENABLE_CxCSS |
                                          BK4819_REG_51_MODE_CTCSS | (3 << 0));
  BK4819_WriteRegister(BK4819_REG_07,
                       BK4819_REG_07_MODE_CTC1 | scale_freq(freq));
}

void BK4819_SetCDCSSCodeWord(uint32_t codeWord) {
  BK4819_WriteRegister(BK4819_REG_51, BK4819_REG_51_ENABLE_CxCSS |
                                          BK4819_REG_51_MODE_CDCSS | (7 << 0));
  BK4819_WriteRegister(BK4819_REG_07, BK4819_REG_07_MODE_CDCSS);
  BK4819_WriteRegister(BK4819_REG_08, (codeWord >> 0) & 0xFFF);
  BK4819_WriteRegister(BK4819_REG_08, 0x8000 | ((codeWord >> 12) & 0xFFF));
}

void BK4819_SetScrambler(uint8_t scrambler) {
  if (scrambler == 0) {
    uint16_t v = BK4819_ReadRegister(BK4819_REG_48);
    BK4819_WriteRegister(BK4819_REG_48, v & 0xFFFC);
  } else {
    BK4819_WriteRegister(BK4819_REG_48,
                         (BK4819_ReadRegister(BK4819_REG_48) & 0xFFFC) |
                             (scrambler & 3));
    BK4819_WriteRegister(BK4819_REG_46, 0x6000 | ((scrambler & 0x3F) << 4) | 8);
  }
}

void BK4819_SetTailDetection(uint16_t freq) { (void)freq; }

void BK4819_EnterDTMF_TX(bool bTail) { (void)bTail; }

void BK4819_ExitDTMF_TX(bool bTail) { (void)bTail; }

void BK4819_PlayDTMFString(const char *str, bool bAuto, uint16_t first,
                           uint16_t tick, uint16_t gap, uint16_t tail) {
  (void)str;
  (void)bAuto;
  (void)first;
  (void)tick;
  (void)gap;
  (void)tail;
}

void BK4819_PlayRogerTiny(void) {
  BK4819_WriteRegister(BK4819_REG_70, BK4819_REG_70_ENABLE_TONE1 | (10 << 8) |
                                          BK4819_REG_70_ENABLE_TONE2 |
                                          (10 << 0));
}

void BK4819_GenTail(uint8_t tail) { (void)tail; }

void BK4819_TurnsOffTones_TurnsOnRX(void) {
  BK4819_WriteRegister(BK4819_REG_70, 0);
  BK4819_WriteRegister(BK4819_REG_30, BK4819_REG_30_ENABLE_RX_LINK |
                                          BK4819_REG_30_ENABLE_AF_DAC |
                                          BK4819_REG_30_ENABLE_DISC_MODE |
                                          BK4819_REG_30_ENABLE_PLL_VCO |
                                          BK4819_REG_30_ENABLE_RX_DSP);
}

void BK4819_EnterTxMute(void) { BK4819_WriteRegister(BK4819_REG_50, 0xBB20); }

void BK4819_ExitTxMute(void) { BK4819_WriteRegister(BK4819_REG_50, 0x3B20); }
