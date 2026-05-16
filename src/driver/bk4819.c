#include "bk4819.h"
#include "../misc.h"
#include "at32f423.h"
#include "bk4819-regs.h"
#include "board.h"

#define PIN_SET(port, pin) ((port)->scr = (pin))
#define PIN_CLR(port, pin) ((port)->clr = (pin))
#define PIN_GET(port, pin) ((port)->idt & (pin))

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

static const uint16_t MOD_TYPE_REG47_VALUES[] = {
    [MOD_FM] = BK4819_AF_FM,      [MOD_AM] = BK4819_AF_FM,
    [MOD_LSB] = BK4819_AF_USB,    [MOD_USB] = BK4819_AF_USB,
    [MOD_BYP] = BK4819_AF_BYPASS, [MOD_RAW] = BK4819_AF_RAW,
    [MOD_WFM] = BK4819_AF_FM,
};

typedef struct {
  gpio_type *scn_port;
  uint16_t scn_pin;
  gpio_type *sda_port;
  uint16_t sda_pin;
  gpio_type *sck_port;
  uint16_t sck_pin;
  uint16_t gpioOutState;
  uint8_t selectedFilter;
  uint32_t freqCacheLow;
  uint32_t freqCacheHigh;
  ModulationType lastModulation;
  bool reg30_cached;
  uint16_t reg30_cache;
} BK4819_Chip;

static BK4819_Chip g_chips[2] = {
    [0] =
        {
            .gpioOutState = 0x0,
            .selectedFilter = 255,
            .freqCacheLow = 0,
            .freqCacheHigh = 0,
            .lastModulation = 255,
            .reg30_cached = false,
            .reg30_cache = 0,
        },
    [1] =
        {
            .gpioOutState = 0x0,
            .selectedFilter = 255,
            .freqCacheLow = 0,
            .freqCacheHigh = 0,
            .lastModulation = 255,
            .reg30_cached = false,
            .reg30_cache = 0,
        },
};

static BK4819_Chip *g_bk = &g_chips[0];

// период полутакта SPI в нс
#define BK_SPI_HALF_PERIOD_NS 32u // ~4 МГц SPI (безопасно, макс 8 МГц)

static inline void bk_delay(void) {
  // cycles = (CoreClock_MHz * ns) / 1000
  // умножаем сначала, чтобы не терять точность при целочисленном делении
  uint32_t cycles =
      (SystemCoreClock / 1000000UL * BK_SPI_HALF_PERIOD_NS) / 1000u;
  if (cycles == 0)
    cycles = 1; // минимум 1 цикл
  uint32_t start = DWT->CYCCNT;
  while ((DWT->CYCCNT - start) < cycles)
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
static inline void sda_input(void) {
  // mode = 00 (input)
  g_bk->sda_port->cfgr &= ~(3u << (__builtin_ctz(g_bk->sda_pin) * 2));
}

static inline void sda_output(void) {
  uint8_t shift = __builtin_ctz(g_bk->sda_pin) * 2;
  uint32_t cfgr = g_bk->sda_port->cfgr;
  cfgr = (cfgr & ~(3u << shift)) | (1u << shift); // mode = 01 (output)
  g_bk->sda_port->cfgr = cfgr;
}

void BK4819_SelectChip(uint8_t chip) {
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOF_PERIPH_CLOCK, TRUE);

  g_bk = &g_chips[chip ? 1 : 0];

  if (chip == 0) {
    g_bk->scn_port = BK1_SCN_PORT;
    g_bk->scn_pin = BK1_SCN_PIN;
    g_bk->sda_port = BK1_SDA_PORT;
    g_bk->sda_pin = BK1_SDA_PIN;
    g_bk->sck_port = BK1_SCK_PORT;
    g_bk->sck_pin = BK1_SCK_PIN;
  } else {
    g_bk->scn_port = BK2_SCN_PORT;
    g_bk->scn_pin = BK2_SCN_PIN;
    g_bk->sda_port = BK2_SDA_PORT;
    g_bk->sda_pin = BK2_SDA_PIN;
    g_bk->sck_port = BK2_SCK_PORT;
    g_bk->sck_pin = BK2_SCK_PIN;
  }

  g_bk->reg30_cached = false;

  gpio_set_output(g_bk->scn_port, g_bk->scn_pin);
  gpio_set_output(g_bk->sda_port, g_bk->sda_pin);
  gpio_set_output(g_bk->sck_port, g_bk->sck_pin);

  PIN_SET(g_bk->scn_port, g_bk->scn_pin);
  PIN_SET(g_bk->sck_port, g_bk->sck_pin);
  PIN_SET(g_bk->sda_port, g_bk->sda_pin);
}

static void write_u16(uint16_t data) {
  gpio_type *sda_port = g_bk->sda_port;
  uint16_t sda_pin = g_bk->sda_pin;
  gpio_type *sck_port = g_bk->sck_port;
  uint16_t sck_pin = g_bk->sck_pin;

  for (int i = 0; i < 16; i++) {
    if (data & 0x8000)
      PIN_SET(sda_port, sda_pin);
    else
      PIN_CLR(sda_port, sda_pin);
    bk_delay();
    PIN_CLR(sck_port, sck_pin);
    bk_delay();
    PIN_SET(sck_port, sck_pin);
    bk_delay();
    data <<= 1;
  }
}

static void write_u8(uint8_t data) {
  gpio_type *sda_port = g_bk->sda_port;
  uint16_t sda_pin = g_bk->sda_pin;
  gpio_type *sck_port = g_bk->sck_port;
  uint16_t sck_pin = g_bk->sck_pin;

  for (int i = 0; i < 8; i++) {
    if (data & 0x80)
      PIN_SET(sda_port, sda_pin);
    else
      PIN_CLR(sda_port, sda_pin);
    bk_delay();
    PIN_CLR(sck_port, sck_pin);
    bk_delay();
    PIN_SET(sck_port, sck_pin);
    bk_delay();
    data <<= 1;
  }
}

static uint16_t read_u16(void) {
  sda_input();
  gpio_type *sda_port = g_bk->sda_port;
  uint16_t sda_pin = g_bk->sda_pin;
  gpio_type *sck_port = g_bk->sck_port;
  uint16_t sck_pin = g_bk->sck_pin;

  uint16_t v = 0;
  for (int i = 0; i < 16; i++) {
    PIN_SET(sck_port, sck_pin);
    bk_delay();
    PIN_CLR(sck_port, sck_pin);
    bk_delay();
    v <<= 1;
    if (PIN_GET(sda_port, sda_pin))
      v |= 1;
  }
  sda_output();
  return v;
}

static inline void regcache_write(uint8_t reg, uint16_t data) {
  switch (reg) {
  case BK4819_REG_30:
    g_bk->reg30_cache = data;
    g_bk->reg30_cached = true;
    return;
  /* case BK4819_REG_43:
    g_bk->reg_cache_43 = data;
    return;
  case BK4819_REG_47:
    g_bk->reg_cache_47 = data;
    return;
  case BK4819_REG_7E:
    g_bk->reg_cache_7E = data;
    return;
  case 0x73:
    g_bk->reg_cache_73 = data;
    return; */
  default:
    return;
  }
}

static inline uint16_t regcache_read(uint8_t reg) {
  switch (reg) {
  case BK4819_REG_30:
    return g_bk->reg30_cached ? g_bk->reg30_cache : 0xFFFF;
  /* case BK4819_REG_43:
    return g_bk->reg_cache_43;
  case BK4819_REG_47:
    return g_bk->reg_cache_47;
  case BK4819_REG_7E:
    return g_bk->reg_cache_7E;
  case 0x73:
    return g_bk->reg_cache_73; */
  default:
    return 0xFFFF;
  }
}

void BK4819_WriteRegister(uint8_t reg, uint16_t data) {
  regcache_write(reg, data);

  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  PIN_SET(g_bk->scn_port, g_bk->scn_pin);
  PIN_SET(g_bk->sck_port, g_bk->sck_pin);
  bk_delay();
  PIN_CLR(g_bk->scn_port, g_bk->scn_pin);
  write_u8(reg);
  write_u16(data);
  PIN_SET(g_bk->scn_port, g_bk->scn_pin);
  bk_delay();
  PIN_SET(g_bk->sck_port, g_bk->sck_pin);
  PIN_SET(g_bk->sda_port, g_bk->sda_pin);

  __set_PRIMASK(primask);
}

uint16_t BK4819_ReadRegister(uint8_t reg) {
  uint16_t cached = regcache_read(reg);
  if (cached != 0xFFFF)
    return cached;

  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  PIN_SET(g_bk->scn_port, g_bk->scn_pin);
  PIN_SET(g_bk->sck_port, g_bk->sck_pin);
  bk_delay();
  PIN_CLR(g_bk->scn_port, g_bk->scn_pin);
  write_u8(reg | 0x80);
  uint16_t v = read_u16();
  PIN_SET(g_bk->scn_port, g_bk->scn_pin);
  bk_delay();
  PIN_SET(g_bk->sck_port, g_bk->sck_pin);
  PIN_SET(g_bk->sda_port, g_bk->sda_pin);

  __set_PRIMASK(primask);
  return v;
}

static const uint8_t sqTypeValues[4] = {0x88, 0xAA, 0xCC, 0xFF};
static const uint8_t DTMF_COEFFS[] = {111, 107, 103, 98, 80,  71,  58,  44,
                                      65,  55,  37,  23, 228, 203, 181, 159};

const Gain GAIN_TABLE[32] = {
    {0x3ff, 0},  // AUTO
    {0x3ff, 0},  //
    {0x3f7, 3},  //
    {0x3ef, 6},  //
    {0x3e7, 8},  //
    {0x3e6, 11}, //
    {0x3e5, 14}, //
    {0x3e4, 17}, //
    {0x3d3, 20}, //
    {0x3b3, 22}, //
    {0x3c3, 25}, //
    {0x3b2, 28}, //
    {0x3c2, 31}, //
    {0x3b1, 34}, //
    {0x3f0, 36}, //
    {0x3e8, 39}, //
    {0x390, 42}, //
    {0x3a0, 45}, //
    {0x368, 48}, //
    {0x360, 50}, //
    {0x348, 53}, //
    {0x2a0, 56}, //
    {0x301, 59}, //
    {0x20a, 62}, //
    {0x248, 64}, //
    {0x10a, 67}, //
    {0x201, 70}, //
    {0x109, 73}, //
    {0x200, 76}, //
    {0x1, 78},   //
    {0x100, 81}, //
    {0x0, 84},   //

};

static inline uint16_t scale_freq(const uint16_t freq) {
  return ((uint32_t)freq * 1353245u + (1u << 16)) >> 17;
}

void BK4819_Idle(void) { BK4819_WriteRegister(BK4819_REG_30, 0x0000); }

/* ── Filter band tracking ───────────────────────────────────── */

typedef enum {
  BAND_HF = 0,
  BAND_VHF,
  BAND_UHF,
  BAND_800,
  BAND_UNSET,
} BandState_t;

static BandState_t lastBand = BAND_UNSET;

static BandState_t getBand(uint32_t f) {
  if (f >= 800 * MHZ)
    return BAND_800;
  if (f >= 240 * MHZ)
    return BAND_UHF;
  if (f >= 30 * MHZ)
    return BAND_VHF;
  return BAND_HF;
}

void BK4819_Init(void) {
  g_bk->reg30_cached = false;
  lastBand = BAND_UNSET; // REG_33 сейчас будет обнулён, кэш должен совпадать

  PIN_SET(g_bk->scn_port, g_bk->scn_pin);
  PIN_SET(g_bk->sck_port, g_bk->sck_pin);
  PIN_SET(g_bk->sda_port, g_bk->sda_pin);

  BK4819_WriteRegister(BK4819_REG_00, 0x8000);
  BK4819_WriteRegister(BK4819_REG_00, 0x0000);
  BK4819_WriteRegister(BK4819_REG_37, 0x1D0F);

  g_bk->gpioOutState = 0;
  BK4819_WriteRegister(BK4819_REG_33, g_bk->gpioOutState);

  if (g_bk == &g_chips[0]) {
    BK4819_ToggleGpioOut(BK4819_GPIO2_VHF_RX, true);
  }

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

  delay_ms(10);
  BK4819_WriteRegister(BK4819_REG_3F, 0);

  BK4819_WriteRegister(BK4819_REG_19, 0x1041);
  BK4819_WriteRegister(BK4819_REG_7D, 0xE940 | 10);
  BK4819_WriteRegister(0x74, 0xAF1F);

  BK4819_DisableDTMF();

  BK4819_WriteRegister(0x40, (BK4819_ReadRegister(0x40) & ~(0x7FF)) |
                                 (50 * 10) | (1 << 12));
}

uint16_t BK4819_TuneToAndWaitRSSI(uint32_t freq) {
  BK4819_SetFrequency(freq);

  uint16_t reg = BK4819_ReadRegister(BK4819_REG_30);
  // BK4819_WriteRegister(BK4819_REG_30, 0x200);
  BK4819_WriteRegister(BK4819_REG_30, reg & ~BK4819_REG_30_ENABLE_VCO_CALIB);
  rt880_delay_us(300);
  BK4819_WriteRegister(BK4819_REG_30, reg);
  rt880_delay_us(2500);

  return BK4819_GetRSSI();
}

int8_t BK4819_GetAgcIndex() {
  int8_t idx = (BK4819_ReadRegister(BK4819_REG_7E) >> 12) & 7;
  if (idx > 3) {
    idx -= 8;
  }
  return idx;
}

uint8_t BK4819_GetAttenuation() {
  uint8_t reg = BK4819_REG_13;
  switch (BK4819_GetAgcIndex()) {
  case 0:
    reg = BK4819_REG_10;
    break;
  case 1:
    reg = BK4819_REG_11;
    break;
  case 2:
    reg = BK4819_REG_12;
    break;
  case 3:
    reg = BK4819_REG_13;
    break;
  case -1:
    reg = BK4819_REG_14;
    break;
  }
  static const uint8_t lna_peak[4] = {19, 16, 11, 0};
  static const uint8_t lna_gain[8] = {24, 19, 14, 9, 6, 4, 2, 0};
  static const uint8_t mixer_gain[4] = {8, 6, 3, 0};
  static const uint8_t pga_gain[8] = {33, 27, 21, 15, 9, 6, 3, 0};

  uint16_t v = BK4819_ReadRegister(reg);
  return lna_peak[(v >> 8) & 3] + lna_gain[(v >> 5) & 7] +
         mixer_gain[(v >> 3) & 3] + pga_gain[v & 7];
}

typedef struct {
  uint8_t lo;
  uint8_t low;
  uint8_t high;
} AgcConfig;

static const AgcConfig AGC_DEFAULT = {0, 56, 84};
static const AgcConfig AGC_FAST = {0, 32, 50};

void BK4819_SetAGC(bool fm, uint8_t gainIndex) {
  const bool enableAgc = gainIndex == AUTO_GAIN_INDEX;
  const AgcConfig *cfg = fm ? &AGC_DEFAULT : &AGC_FAST;
  uint16_t reg13 = enableAgc ? 0x03DF : GAIN_TABLE[gainIndex].regValue;
  uint16_t reg14 = fm ? 0x0210 : 0x0000;
  uint16_t reg49 =
      fm ? 0x2AB2 : (cfg->lo << 14) | (cfg->high << 7) | (cfg->low << 0);

  uint16_t reg7E = BK4819_ReadRegister(BK4819_REG_7E);
  reg7E &= ~((1 << 15) | (0b111 << 12)); // Clear AGC and index bits
  reg7E |= (!enableAgc << 15) |          // AGC fix mode
           (3u << 12) |                  // AGC fix index
           (5u << 3) |                   // Default DC
           (6u << 0);                    // Default value

  BK4819_WriteRegister(BK4819_REG_13, reg13);
  BK4819_WriteRegister(BK4819_REG_14, reg14);

  BK4819_WriteRegister(BK4819_REG_49, reg49);
  // BK4819_WriteRegister(BK4819_REG_7B, 0x8420);
  BK4819_WriteRegister(BK4819_REG_7E, reg7E);
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

uint32_t BK4819_GetFrequency(void) {
  return (BK4819_ReadRegister(BK4819_REG_39) << 16) |
         BK4819_ReadRegister(BK4819_REG_38);
}

void BK4819_SetFrequency(uint32_t freq) {
  uint16_t low = freq & 0xFFFF;
  uint16_t high = (freq >> 16) & 0xFFFF;

  if (low != g_bk->freqCacheLow) {
    BK4819_WriteRegister(BK4819_REG_38, low);
    g_bk->freqCacheLow = low;
  }

  if (high != g_bk->freqCacheHigh) {
    BK4819_WriteRegister(BK4819_REG_39, high);
    g_bk->freqCacheHigh = high;
  }
}

void BK4819_TuneTo(uint32_t freq, bool precise) {
  BK4819_SetFrequency(freq);

  uint16_t reg = BK4819_ReadRegister(BK4819_REG_30);
  if (precise) {
    BK4819_WriteRegister(BK4819_REG_30, 0x0200);
    BK4819_WriteRegister(BK4819_REG_30, reg);
  } else if (reg & BK4819_REG_30_ENABLE_VCO_CALIB) {
    BK4819_WriteRegister(BK4819_REG_30, reg & ~BK4819_REG_30_ENABLE_VCO_CALIB);
    BK4819_WriteRegister(BK4819_REG_30, reg);
  }
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

void BK4819_SetAF(BK4819_AF_Type_t af) {
  BK4819_WriteRegister(BK4819_REG_47, 0x6042 | (af << 8));
}

XtalMode BK4819_XtalGet(void) {
  return (XtalMode)((BK4819_ReadRegister(0x3C) >> 6) & 0b11);
}

void BK4819_XtalSet(XtalMode mode) {
  uint16_t ifset = 0x2AAB;
  uint16_t xtal = 20360;

  switch (mode) {
  case XTAL_0_13M:
    xtal = 20232;
    ifset = 0x3555;
    break;
  case XTAL_1_19_2M:
    xtal = 20296;
    ifset = 0x2E39;
    break;
  case XTAL_2_26M:
    // Use defaults
    break;
  case XTAL_3_38_4M:
    xtal = 20424;
    ifset = 0x271C;
    break;
  }

  BK4819_WriteRegister(0x3C, xtal);
  BK4819_WriteRegister(0x3D, ifset);
}

uint16_t BK4819_GetRegValue(RegisterSpec spec) {
  return (_ReadRegCached(spec.num) >> spec.offset) & spec.mask;
}

void BK4819_SetRegValue(RegisterSpec spec, uint16_t value) {
  uint16_t reg = BK4819_ReadRegister(spec.num);
  reg &= ~(spec.mask << spec.offset);
  BK4819_WriteRegister(spec.num, reg | (value << spec.offset));
}

uint8_t gLastModulation = 255;
void BK4819_SetModulation(ModulationType type) {
  /* if (type == MOD_BYP) {
    BK4819_EnterBypass();
  } else if (gLastModulation == MOD_BYP) {
    BK4819_ExitBypass();
  } */

  const bool isSsb = (type == MOD_LSB || type == MOD_USB);

  BK4819_SetAF(MOD_TYPE_REG47_VALUES[type]);
  BK4819_SetRegValue(RS_AFC_DIS, isSsb);

  if (type == MOD_WFM) {
    // Batch RF filter bandwidth registers: RF_FILT_BW=7, RF_FILT_BW_WEAK=7,
    // BW_MODE=3 Saves 2 SPI read-modify-write cycles vs 3x SetRegValue
    BK4819_WriteRegister(BK4819_REG_43,
                         (7u << 12) | (7u << 9) | (3u << 4) | (1u << 3));
    BK4819_XtalSet(XTAL_0_13M);
  } else {
    BK4819_XtalSet(XTAL_2_26M);
  }

  // sound boost
  if (isSsb) {
    BK4819_WriteRegister(0x75, 0xFC13);
  } else {
    BK4819_WriteRegister(0x75, 0xF50B); // default
  }

  if (isSsb) {
    BK4819_SetRegValue(RS_IF_F, 0);
  } else if (type == MOD_WFM) {
    BK4819_SetRegValue(RS_IF_F, 14223);
  } else {
    BK4819_SetRegValue(RS_IF_F, 10923);
  }

  uint16_t reg4A = 0x5430; // default

  if (isSsb || type == MOD_AM) {
    reg4A |= 0b1111111;
  } else {
    reg4A = (reg4A & ~0b1111111) | 46;
  }
  BK4819_WriteRegister(0x4A, reg4A);

  uint16_t r31 = BK4819_ReadRegister(0x31);
  if (type == MOD_AM) {
    BK4819_WriteRegister(0x31, r31 | 1);
    BK4819_WriteRegister(0x42, 0x6F5C);
    BK4819_WriteRegister(0x2A, 0x7434); // noise gate time constants
    BK4819_WriteRegister(0x2B, 0x0400);
    BK4819_WriteRegister(0x2F, 0x9990);
  } else {
    BK4819_WriteRegister(0x31, r31 & 0xFFFE);
    BK4819_WriteRegister(0x42, 0x6B5A);
    BK4819_WriteRegister(0x2A, 0x7400);
    BK4819_WriteRegister(0x2B, 0x0000);
    BK4819_WriteRegister(0x2F, 0x9890);
  }

  if (type == MOD_FM) {
    // Karina mod
    BK4819_WriteRegister(0x28, 1536);  // 0x0600 - noise gate для FM
    BK4819_WriteRegister(0x2C, 26210); // 0x6662 - emph/tx gain для FM
    // reg4A уже записан выше через (reg4A & ~0b111111) | 46,
    // читаем его снова чтобы применить FM-специфичную маску ~127U
  } else {
    BK4819_WriteRegister(0x28, 0x0B40); // восстановить дефолт
    BK4819_WriteRegister(0x2C, 0x1822); // восстановить дефолт
  }
  gLastModulation = type;
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

void BK4819_ToggleGpioOut(BK4819_GPIO_PIN_t pin, bool enable) {
  const uint16_t pin_bit = 0x40U >> pin;

  if (enable) {
    g_bk->gpioOutState |= pin_bit;
  } else {
    g_bk->gpioOutState &= ~pin_bit;
  }

  BK4819_WriteRegister(BK4819_REG_33, g_bk->gpioOutState);
}

uint16_t BK4819_GetFilter() {
  return g_chips[0].gpioOutState &
         (FILTER_HF | FILTER_VHF | FILTER_UHF | FILTER_800);
}

void BK4819_SelectFilterByFrequency(uint32_t f) {
  BandState_t band = getBand(f);
  if (band == lastBand) {
    return;
  }
  lastBand = band;
  // Определяем маску нужного фильтра
  uint16_t flt;
  if (band == BAND_HF)
    flt = FILTER_HF;
  else if (band == BAND_VHF)
    flt = FILTER_VHF;
  else if (band == BAND_UHF)
    flt = FILTER_UHF;
  else
    flt = FILTER_800;

  // Сбрасываем все фильтры и включаем только нужный
  g_chips[0].gpioOutState &=
      ~(FILTER_HF | FILTER_VHF | FILTER_UHF | FILTER_800);
  g_chips[0].gpioOutState |= flt;

  // Пишем в регистр чипа 0
  BK4819_SelectChip(0);
  BK4819_WriteRegister(BK4819_REG_33, g_chips[0].gpioOutState);

  // Восстанавливаем активный чип, если нужно
  if (g_bk != &g_chips[0]) {
    BK4819_SelectChip(1);
  }
}

void BK4819_ToggleFilter(Filter flt, bool on) {
  if (on) {
    g_chips[0].gpioOutState |= flt;
  } else {
    g_chips[0].gpioOutState &= ~flt;
  }

  BK4819_SelectChip(0);
  BK4819_WriteRegister(BK4819_REG_33, g_chips[0].gpioOutState);
  if (g_bk != &g_chips[0]) {
    BK4819_SelectChip(1);
  }
}

void BK4819_SelectB(bool on) {
  BK4819_SelectChip(1);
  BK4819_ToggleGpioOut(BK4819_GPIO2_VHF_RX, on);
  if (g_bk != &g_chips[1]) {
    BK4819_SelectChip(0);
  }
}

void BK4819_ToggleAFBit(bool enable) {
  uint16_t reg = BK4819_ReadRegister(BK4819_REG_47);
  reg &= ~(1 << 8);
  if (enable) {
    reg |= 1 << 8;
  }
  BK4819_WriteRegister(BK4819_REG_47, reg);
}

void BK4819_ToggleAFDAC(bool enable) {
  uint16_t reg = BK4819_ReadRegister(BK4819_REG_30);
  reg &= ~BK4819_REG_30_ENABLE_AF_DAC;
  if (enable) {
    reg |= BK4819_REG_30_ENABLE_AF_DAC;
  }
  BK4819_WriteRegister(BK4819_REG_30, reg);
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
