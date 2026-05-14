#include "si473x.h"
#include "../helper/log.h"
#include "../ui/graphics.h"
#include "at32f423.h"
#include "board.h"
#include "st7789.h"

/* ─── Pin mapping ──────────────────────────────────────────────── */
#define I2C_ADDR 0x11

#define PIN_SCL GPIO_PINS_0
#define PORT_SCL GPIOC
#define PIN_SDA GPIO_PINS_1
#define PORT_SDA GPIOC
#define PIN_RST GPIO_PINS_9 /* adjust to your board */
#define PORT_RST GPIOB

#define PIN_SET(port, pin) ((port)->scr = (pin))
#define PIN_CLR(port, pin) ((port)->clr = (pin))
#define PIN_GET(port, pin) ((port)->idt & (pin))

/* ─── State ────────────────────────────────────────────────────── */
RSQStatus rsqStatus;
static SsbMode currentSsbMode;

SI47XX_MODE si4732mode = SI47XX_FM;
uint16_t siCurrentFreq = 0;
bool isSi4732On = false;

/* ─── Helpers ──────────────────────────────────────────────────── */
static uint16_t fDiv(void) { return si4732mode == SI47XX_FM ? 1000 : 100; }

bool SI47XX_IsSSB(void) {
  return si4732mode == SI47XX_USB || si4732mode == SI47XX_LSB;
}

/* ─── Bit-bang I2C ─────────────────────────────────────────────── */
static void sda_input(void) {
  gpio_init_type g;
  gpio_default_para_init(&g);
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
  g.gpio_mode = GPIO_MODE_INPUT;
  g.gpio_pins = PIN_SDA;
  g.gpio_pull = GPIO_PULL_UP; /* pull-up needed to read SDA */
  gpio_init(PORT_SDA, &g);
}

static void sda_output(void) {
  gpio_init_type g;
  gpio_default_para_init(&g);
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
  g.gpio_mode = GPIO_MODE_OUTPUT;
  g.gpio_pins = PIN_SDA;
  g.gpio_pull = GPIO_PULL_UP;
  gpio_init(PORT_SDA, &g);
}

static void i2c_delay(void) {
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
}

static void i2c_start(void) {
  sda_output();
  PIN_SET(PORT_SDA, PIN_SDA);
  PIN_SET(PORT_SCL, PIN_SCL);
  i2c_delay();
  PIN_CLR(PORT_SDA, PIN_SDA);
  i2c_delay();
  PIN_CLR(PORT_SCL, PIN_SCL);
}

static void i2c_stop(void) {
  sda_output();
  PIN_CLR(PORT_SDA, PIN_SDA);
  i2c_delay();
  PIN_SET(PORT_SCL, PIN_SCL);
  i2c_delay();
  PIN_SET(PORT_SDA, PIN_SDA);
  i2c_delay();
}

static bool i2c_write_byte(uint8_t data) {
  sda_output();
  for (int i = 0; i < 8; i++) {
    if (data & 0x80)
      PIN_SET(PORT_SDA, PIN_SDA);
    else
      PIN_CLR(PORT_SDA, PIN_SDA);
    i2c_delay();
    PIN_SET(PORT_SCL, PIN_SCL);
    i2c_delay();
    PIN_CLR(PORT_SCL, PIN_SCL);
    data <<= 1;
  }
  sda_input();
  PIN_SET(PORT_SCL, PIN_SCL);
  i2c_delay();
  bool ack = (PIN_GET(PORT_SDA, PIN_SDA) == 0);
  PIN_CLR(PORT_SCL, PIN_SCL);
  i2c_delay();
  return ack;
}

static uint8_t i2c_read_byte(bool send_ack) {
  sda_input();
  uint8_t data = 0;
  for (int i = 0; i < 8; i++) {
    data <<= 1;
    PIN_SET(PORT_SCL, PIN_SCL);
    i2c_delay();
    if (PIN_GET(PORT_SDA, PIN_SDA))
      data |= 1;
    PIN_CLR(PORT_SCL, PIN_SCL);
    i2c_delay();
  }
  /* ACK/NACK */
  sda_output();
  if (send_ack)
    PIN_CLR(PORT_SDA, PIN_SDA); /* ACK  */
  else
    PIN_SET(PORT_SDA, PIN_SDA); /* NACK */
  i2c_delay();
  PIN_SET(PORT_SCL, PIN_SCL);
  i2c_delay();
  PIN_CLR(PORT_SCL, PIN_SCL);
  i2c_delay();
  return data;
}

/* ─── Buffer I/O ───────────────────────────────────────────────── */
void SI47XX_WriteBuffer(const uint8_t *buf, uint8_t len) {
  i2c_start();
  i2c_write_byte(I2C_ADDR << 1); /* W */
  for (uint8_t i = 0; i < len; i++)
    i2c_write_byte(buf[i]);
  i2c_stop();
}

void SI47XX_ReadBuffer(uint8_t *buf, uint8_t len) {
  i2c_start();
  i2c_write_byte((I2C_ADDR << 1) | 1); /* R */
  for (uint8_t i = 0; i < len; i++)
    buf[i] = i2c_read_byte(i < len - 1); /* ACK all except last */
  i2c_stop();
}

/* ─── CTS polling (with timeout) ──────────────────────────────── */
/* FIX #1: added timeout to prevent infinite loop on dead chip     */
/* FIX #2: NOT called before first POWER_UP (see SI47XX_PowerUp)  */
static bool waitToSend(void) {
  uint8_t st = 0;
  uint32_t timeout = 10000;
  do {
    i2c_delay();
    i2c_delay();
    SI47XX_ReadBuffer(&st, 1);
    if (--timeout == 0)
      return false; /* chip not responding */
  } while (!(st & STATUS_CTS));
  return true;
}

/* ─── GPIO init ────────────────────────────────────────────────── */
void SI47XX_Init(void) {
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);

  gpio_init_type g;
  gpio_default_para_init(&g);
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
  g.gpio_mode = GPIO_MODE_OUTPUT;
  g.gpio_pull = GPIO_PULL_UP;
  g.gpio_pins = PIN_SCL | PIN_SDA;
  gpio_init(PORT_SCL, &g); /* PORT_SCL == PORT_SDA == GPIOC */

  /* RST pin — push-pull, start LOW (chip in reset) */
  gpio_default_para_init(&g);
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_mode = GPIO_MODE_OUTPUT;
  g.gpio_pins = PIN_RST;
  g.gpio_pull = GPIO_PULL_NONE;
  gpio_init(PORT_RST, &g);
  PIN_CLR(PORT_RST, PIN_RST); /* hold in reset */
}

/* ─── Properties ───────────────────────────────────────────────── */
void sendProperty(uint16_t prop, uint16_t value) {
  waitToSend();
  uint8_t cmd[6] = {CMD_SET_PROPERTY,      0,
                    (uint8_t)(prop >> 8),  (uint8_t)(prop & 0xFF),
                    (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
  SI47XX_WriteBuffer(cmd, 6);
  delay_ms(8); /* Si47xx requires ≥10 ms between SET_PROPERTY commands */
}

uint16_t getProperty(uint16_t prop, bool *valid) {
  uint8_t response[4] = {0};
  uint8_t cmd[4] = {CMD_GET_PROPERTY, 0, (uint8_t)(prop >> 8),
                    (uint8_t)(prop & 0xFF)};
  waitToSend();
  SI47XX_WriteBuffer(cmd, 4);
  SI47XX_ReadBuffer(response, 4);
  if (valid)
    *valid = !(response[0] & STATUS_ERR);
  return (uint16_t)((response[2] << 8) | response[3]);
}

/* ─── Volume ───────────────────────────────────────────────────── */
void SI47XX_SetVolume(uint8_t volume) {
  if (volume > 63)
    volume = 63;
  sendProperty(PROP_RX_VOLUME, volume);
}

/* ─── AGC ──────────────────────────────────────────────────────── */
void SI47XX_SetAutomaticGainControl(uint8_t AGCDIS, uint8_t AGCIDX) {
  uint8_t cmd =
      (si4732mode == SI47XX_FM) ? CMD_FM_AGC_OVERRIDE : CMD_AM_AGC_OVERRIDE;
  SI47XX_AgcOverrride agc;
  agc.arg.DUMMY = 0;
  agc.arg.AGCDIS = AGCDIS;
  agc.arg.AGCIDX = AGCIDX;
  waitToSend();
  uint8_t buf[3] = {cmd, agc.raw[0], agc.raw[1]};
  SI47XX_WriteBuffer(buf, 3);
}

static void setAvcAmMaxGain(uint8_t gain) {
  if (gain < 12 || gain > 90)
    return;
  sendProperty(PROP_AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN,
               (uint16_t)(gain * 340));
}

/* ─── Bandwidth ────────────────────────────────────────────────── */
/* FIX #3: использовать правильное свойство для AM/FM раздельно     */
void SI47XX_SetBandwidth(SI47XX_FilterBW AMCHFLT, bool AMPLFLT) {
  SI47XX_BW_Config cfg = {0};
  cfg.param.AMCHFLT = AMCHFLT;
  cfg.param.AMPLFLT = AMPLFLT;
  sendProperty(PROP_AM_CHANNEL_FILTER, (cfg.raw[1] << 8) | cfg.raw[0]);
}

/* ─── SSB ──────────────────────────────────────────────────────── */
void SI47XX_SsbSetup(SI47XX_SsbFilterBW AUDIOBW, uint8_t SBCUTFLT,
                     uint8_t AVC_DIVIDER, uint8_t AVCEN, uint8_t SMUTESEL,
                     uint8_t DSP_AFCDIS) {
  currentSsbMode.param.SBCUTFLT = SBCUTFLT;
  currentSsbMode.param.AVC_DIVIDER = AVC_DIVIDER;
  currentSsbMode.param.AVCEN = AVCEN;
  currentSsbMode.param.SMUTESEL = SMUTESEL;
  currentSsbMode.param.DSP_AFCDIS = DSP_AFCDIS;
  currentSsbMode.param.AUDIOBW = AUDIOBW;
  /* FIX #4: структура заполняется побайтово, порядок raw[0]/raw[1] — LE */
  sendProperty(PROP_SSB_MODE, (uint16_t)((currentSsbMode.raw[1] << 8) |
                                         currentSsbMode.raw[0]));
}

void SI47XX_SetSsbBandwidth(SI47XX_SsbFilterBW bw) {
  SI47XX_SsbSetup(bw, 1, 0, 1, 0, 1);
}

void SI47XX_SetBFO(int16_t bfo) { sendProperty(PROP_SSB_BFO, (uint16_t)bfo); }

/* ─── RDS ──────────────────────────────────────────────────────── */
static void enableRDS(void) {
  if (si4732mode != SI47XX_FM)
    return;
  sendProperty(PROP_FM_RDS_INT_SOURCE, FLG_RDSRECV);
  sendProperty(PROP_FM_RDS_INT_FIFO_COUNT, 12);
  sendProperty(PROP_FM_RDS_CONFIG, (uint16_t)(((FLG_BLETHA_35 | FLG_BLETHB_35 |
                                                FLG_BLETHC_35 | FLG_BLETHD_35)
                                               << 8) |
                                              FLG_RDSEN));
}

void SI47XX_ReadRDS(uint8_t buf[13]) {
  uint8_t cmd[2] = {CMD_FM_RDS_STATUS, RDS_STATUS_ARG1_CLEAR_INT};
  waitToSend();
  SI47XX_WriteBuffer(cmd, 2);
  SI47XX_ReadBuffer(buf, 13);
}

/* ─── RSQ ──────────────────────────────────────────────────────── */
void RSQ_GET(void) {
  uint8_t cmd[2] = {CMD_FM_RSQ_STATUS, 0x01};
  if (si4732mode != SI47XX_FM)
    cmd[0] = CMD_AM_RSQ_STATUS;
  waitToSend();
  SI47XX_WriteBuffer(cmd, 2);
  SI47XX_ReadBuffer(rsqStatus.raw, si4732mode == SI47XX_FM ? 8 : 6);
}

/* ─── Power up/down ────────────────────────────────────────────── */
void SI47XX_PowerUp(void) {
  /* FIX #5: RST HIGH (деассертировать сброс) → фиксированная задержка
             → только потом POWER_UP, и уже после — waitToSend()        */
  PIN_SET(PORT_RST, PIN_RST);
  delay_ms(10); /* минимум 100 мкс после RST по даташиту, берём с запасом */

  uint8_t mode_arg = (si4732mode == SI47XX_AM) ? (FLG_XOSCEN | FUNC_AM)
                                               : (FLG_XOSCEN | FUNC_FM);
  uint8_t cmd[3] = {CMD_POWER_UP, mode_arg, OUT_ANALOG};
  SI47XX_WriteBuffer(cmd, 3); /* НЕ waitToSend перед первым POWER_UP */
  delay_ms(500);

  isSi4732On = true;
  SI47XX_SetVolume(63);

  if (si4732mode == SI47XX_FM) {
    enableRDS();
  } else if (si4732mode == SI47XX_AM) {
    SI47XX_SetAutomaticGainControl(1, 0);
    sendProperty(PROP_AM_SOFT_MUTE_MAX_ATTENUATION, 0);
    sendProperty(PROP_AM_AGC_RELEASE_RATE, 20);
    setAvcAmMaxGain(40);
  }

  SI47XX_SetFreq(siCurrentFreq);
}

void SI47XX_PatchPowerUp(void) {
  PIN_SET(PORT_RST, PIN_RST);
  delay_ms(10);

  /* 0b00110001 = XOSCEN | GPO2OEN | FUNC=QUERY (patch mode) */
  uint8_t cmd[3] = {CMD_POWER_UP, 0b00110001, OUT_ANALOG};
  SI47XX_WriteBuffer(cmd, 3);
  delay_ms(60);

  isSi4732On = true;

  // SI47XX_downloadPatch();

  SI47XX_SsbSetup(2, 1, 0, 1, 0, 1);
  setAvcAmMaxGain(42);
  SI47XX_SetVolume(63);
  SI47XX_SetFreq(siCurrentFreq);
  sendProperty(PROP_SSB_SOFT_MUTE_MAX_ATTENUATION, 0);
  sendProperty(PROP_AM_AUTOMATIC_VOLUME_CONTROL_MAX_GAIN, 0x7800);

  si4732mode = SI47XX_USB; /* TODO: установить режим ДО включения */
}

void SI47XX_PowerDown(void) {
  uint8_t cmd[1] = {CMD_POWER_DOWN};
  waitToSend();
  SI47XX_WriteBuffer(cmd, 1);
  delay_ms(10);
  PIN_CLR(PORT_RST, PIN_RST); /* держать RST LOW пока чип выключен */
  isSi4732On = false;
  siCurrentFreq = 0;
}

/* ─── Mode switch ──────────────────────────────────────────────── */
void SI47XX_SwitchMode(SI47XX_MODE mode) {
  if (si4732mode == mode)
    return;
  bool wasSSB = SI47XX_IsSSB();
  si4732mode = mode;
  if (mode == SI47XX_USB || mode == SI47XX_LSB) {
    if (!wasSSB) {
      SI47XX_PowerDown();
      SI47XX_PatchPowerUp();
    }
  } else {
    SI47XX_PowerDown();
    SI47XX_PowerUp();
  }
}

/* ─── Frequency ────────────────────────────────────────────────── */
void SI47XX_SetFreq(uint16_t freq) {
  if (siCurrentFreq == freq)
    return;

  uint8_t hb = (freq >> 8) & 0xFF;
  uint8_t lb = freq & 0xFF;
  bool isSW = (freq > 1800);

  uint8_t size = 5;
  uint8_t cmd[6] = {CMD_FM_TUNE_FREQ, 0x00, hb, lb, 0, 0};

  if (si4732mode == SI47XX_FM || si4732mode == SI47XX_AM) {
    cmd[1] = 0x01; /* FAST tune */
  } else {
    cmd[1] = (si4732mode == SI47XX_USB) ? 0b10000000 : 0b01000000;
  }

  if (si4732mode != SI47XX_FM) {
    cmd[0] = CMD_AM_TUNE_FREQ;
    size = 6;
  }

  if (si4732mode == SI47XX_AM && isSW) {
    cmd[5] = 1; /* ANTCAP для SW */
  }

  waitToSend();
  SI47XX_WriteBuffer(cmd, size);
  siCurrentFreq = freq;
}

void SI47XX_TuneTo(uint32_t f) {
  if (SI47XX_IsSSB()) {
    int64_t bfo = ((int64_t)(siCurrentFreq * fDiv()) - (int64_t)f) * 10;
    if (bfo > -16000 && bfo < 16000) {
      SI47XX_SetBFO((int16_t)bfo);
      f = (uint32_t)siCurrentFreq * fDiv();
    } else {
      SI47XX_SetBFO(0);
    }
  }
  f /= fDiv();
  if (si4732mode == SI47XX_FM)
    f -= f % 5; /* выравнивание на шаг 50 кГц */
  SI47XX_SetFreq((uint16_t)f);
}

uint32_t SI47XX_getFrequency(bool *valid) {
  uint8_t response[4] = {0};
  uint8_t cmd[1] = {(si4732mode == SI47XX_AM) ? CMD_AM_TUNE_STATUS
                                              : CMD_FM_TUNE_STATUS};
  waitToSend();
  SI47XX_WriteBuffer(cmd, 1);
  SI47XX_ReadBuffer(response, 4);
  if (valid)
    *valid = (response[1] & STATUS_VALID) != 0;
  return ((uint32_t)((response[2] << 8) | response[3])) * fDiv();
}

/* ─── Seek ─────────────────────────────────────────────────────── */
void SI47XX_Seek(bool up, bool wrap) {
  uint8_t seekOpt = (up ? FLG_SEEKUP : 0) | (wrap ? FLG_WRAP : 0);
  uint8_t cmd[6] = {CMD_FM_SEEK_START, seekOpt, 0, 0, 0, 0};

  if (si4732mode == SI47XX_AM) {
    cmd[0] = CMD_AM_SEEK_START;
    cmd[5] = (siCurrentFreq > 1800) ? 1 : 0;
  }

  waitToSend();
  SI47XX_WriteBuffer(cmd, si4732mode == SI47XX_FM ? 2 : 6);
}

/* ─── Seek limits/spacing/thresholds ──────────────────────────── */
void SI47XX_SetSeekFmLimits(uint32_t bottom, uint32_t top) {
  uint16_t d = fDiv();
  sendProperty(PROP_FM_SEEK_BAND_BOTTOM, (uint16_t)(bottom / d));
  sendProperty(PROP_FM_SEEK_BAND_TOP, (uint16_t)(top / d));
}

void SI47XX_SetSeekAmLimits(uint32_t bottom, uint32_t top) {
  uint16_t d = fDiv();
  sendProperty(PROP_AM_SEEK_BAND_BOTTOM, (uint16_t)(bottom / d));
  sendProperty(PROP_AM_SEEK_BAND_TOP, (uint16_t)(top / d));
}

void SI47XX_SetSeekFmSpacing(uint32_t spacing) {
  sendProperty(PROP_FM_SEEK_FREQ_SPACING, (uint16_t)(spacing / fDiv()));
}

void SI47XX_SetSeekAmSpacing(uint32_t spacing) {
  sendProperty(PROP_AM_SEEK_FREQ_SPACING, (uint16_t)(spacing / fDiv()));
}

void SI47XX_SetSeekFmRssiThreshold(uint16_t value) {
  sendProperty(PROP_FM_SEEK_TUNE_RSSI_THRESHOLD, value);
}

void SI47XX_SetSeekAmRssiThreshold(uint16_t value) {
  sendProperty(PROP_AM_SEEK_TUNE_RSSI_THRESHOLD, value);
}

/* ─── Tune status ──────────────────────────────────────────────── */
void SI47xx_GetStatus(uint8_t intack, uint8_t cancel) {
  SI47XX_TuneStatus status;
  uint8_t cmd =
      (si4732mode == SI47XX_AM) ? CMD_AM_TUNE_STATUS : CMD_FM_TUNE_STATUS;
  status.arg.INTACK = intack;
  status.arg.CANCEL = cancel;
  status.arg.RESERVED2 = 0;

  uint8_t cmdA[2] = {cmd, status.raw};
  uint8_t response[4] = {0};
  waitToSend();
  SI47XX_WriteBuffer(cmdA, 2);
  SI47XX_ReadBuffer(response, 4);
}

/* ─── Frontend AGC ─────────────────────────────────────────────── */
void SI47XX_SetAMFrontendAGC(uint8_t minGainIdx, uint8_t attnBackup) {
  sendProperty(PROP_AM_FRONTEND_AGC_CONTROL,
               (uint16_t)((minGainIdx << 8) | attnBackup));
}

bool SI47XX_IsAlive(void) {
  i2c_start();
  bool ack = i2c_write_byte(I2C_ADDR << 1); // 0x44, W
  i2c_stop();
  return ack; // true = чип ответил ACK
}

bool SI47XX_CheckCTS(void) {
  uint8_t st = 0;
  SI47XX_ReadBuffer(&st, 1);
  return (st & STATUS_CTS) != 0; // 0x80
}

bool SI47XX_CheckChipID(void) {
  // GET_REV только после POWER_UP!
  uint8_t cmd[1] = {CMD_GET_REV}; // 0x10
  uint8_t response[9] = {0};

  waitToSend();
  SI47XX_WriteBuffer(cmd, 1);
  waitToSend(); // подождать выполнения команды
  SI47XX_ReadBuffer(response, 9);

  Log("GET_REV: status=0x%02X PN=0x%02X FW=%u.%u CHIPREV=0x%02X", response[0],
      response[1], response[2], response[3], response[8]);
  Log_Render(0, LCD_HEIGHT - 8 * 6, 6);

  return (response[0] & STATUS_CTS) && (response[1] == 0x0D);
}

void SI47XX_I2C_Scan(void) {
  Log("I2C scan...");
  uint8_t found = 0;

  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    i2c_start();
    bool ack = i2c_write_byte(addr << 1); // W-бит
    i2c_stop();

    if (ack) {
      Log("  ACK at 0x%02X", addr);
      found++;
    }
  }

  if (found == 0)
    Log("  No devices found");
  else
    Log("  Total: %u device(s)", found);
  Log_Render(0, LCD_HEIGHT - 8 * 6, 6);
}
