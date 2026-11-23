#include "w25q32.h"

#include "at32f423.h"
#include "at32f423_spi.h"
#include "board.h"
#include "board_pins.h"
#include "gpio.h"


#define W25Q_CMD_READ 0x03u
#define W25Q_CMD_READ_STATUS 0x05u
#define W25Q_CMD_WRITE_ENABLE 0x06u
#define W25Q_CMD_PAGE_PROGRAM 0x02u
#define W25Q_CMD_SECTOR_ERASE 0x20u
#define W25Q_CMD_JEDEC_ID 0x9Fu

#define W25Q_BUSY_TIMEOUT_MS 2000u

static bool s_inited;
static bool s_present;
static uint32_t s_id;

static inline void cs_low(void) { gpio_pin_clr(&PIN_FLASH_CS); }
static inline void cs_high(void) { gpio_pin_set(&PIN_FLASH_CS); }

static uint8_t spi_xfer(uint8_t b) {
  while (spi_i2s_flag_get(SPI2, SPI_I2S_TDBE_FLAG) == RESET)
    ;
  spi_i2s_data_transmit(SPI2, b);
  while (spi_i2s_flag_get(SPI2, SPI_I2S_RDBF_FLAG) == RESET)
    ;
  return (uint8_t)spi_i2s_data_receive(SPI2);
}

static void send_addr(uint32_t addr) {
  spi_xfer((uint8_t)(addr >> 16));
  spi_xfer((uint8_t)(addr >> 8));
  spi_xfer((uint8_t)addr);
}

static uint8_t read_status(void) {
  cs_low();
  spi_xfer(W25Q_CMD_READ_STATUS);
  uint8_t st = spi_xfer(0xFF);
  cs_high();
  return st;
}

static bool wait_ready(void) {
  /* BUSY выставляется не мгновенно после CS↑: без паузы первый опрос
   * читает 0, и следующая команда (страница) уходит во время стирания. */
  delay_us(100);
  uint32_t start = millis();
  while (read_status() & 0x01u) {
    if (millis() - start > W25Q_BUSY_TIMEOUT_MS)
      return false;
  }
  return true;
}

static void write_enable(void) {
  cs_low();
  spi_xfer(W25Q_CMD_WRITE_ENABLE);
  cs_high();
  /* WEL защёлкивается по CS↑; даём паузу, иначе следующая команда
   * иногда уходит до установки WEL и игнорируется. */
  delay_us(10);
}

static uint32_t read_jedec_id(void) {
  cs_low();
  spi_xfer(W25Q_CMD_JEDEC_ID);
  uint32_t id = (uint32_t)spi_xfer(0xFF) << 16;
  id |= (uint32_t)spi_xfer(0xFF) << 8;
  id |= spi_xfer(0xFF);
  cs_high();
  return id;
}

bool W25Q_Init(void) {
  if (s_inited)
    return s_present;
  s_inited = true;

  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_SPI2_PERIPH_CLOCK, TRUE);

  gpio_pin_init_output(&PIN_FLASH_CS);
  gpio_pin_set(&PIN_FLASH_CS);

  gpio_init_type g;
  gpio_default_para_init(&g);
  g.gpio_mode = GPIO_MODE_MUX;
  g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_pull = GPIO_PULL_NONE;
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_pins =
      (uint16_t)(PIN_FLASH_SCK.pin | PIN_FLASH_DO.pin | PIN_FLASH_DI.pin);
  gpio_init(GPIOB, &g);
  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE13, GPIO_MUX_5);
  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE14, GPIO_MUX_5);
  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE15, GPIO_MUX_5);

  spi_init_type s;
  spi_default_para_init(&s);
  s.transmission_mode = SPI_TRANSMIT_FULL_DUPLEX;
  s.master_slave_mode = SPI_MODE_MASTER;
  s.mclk_freq_division = SPI_MCLK_DIV_8; /* APB1 75 МГц / 8 = 9.4 МГц */
  s.first_bit_transmission = SPI_FIRST_BIT_MSB;
  s.frame_bit_num = SPI_FRAME_8BIT;
  s.clock_polarity = SPI_CLOCK_POLARITY_LOW;
  s.clock_phase = SPI_CLOCK_PHASE_1EDGE;
  s.cs_mode_selection = SPI_CS_SOFTWARE_MODE;
  spi_init(SPI2, &s);
  spi_enable(SPI2, TRUE);

  s_id = read_jedec_id();
  s_present = (s_id != 0 && s_id != 0xFFFFFFu);
  return s_present;
}

bool W25Q_Present(void) { return s_present; }

uint32_t W25Q_JedecId(void) { return s_id; }


void W25Q_Read(uint32_t addr, void *dst, uint32_t len) {
  if (!s_present)
    return;
  uint8_t *d = (uint8_t *)dst;

  cs_low();
  spi_xfer(W25Q_CMD_READ);
  send_addr(addr);
  while (len--)
    *d++ = spi_xfer(0xFF);
  cs_high();
}

bool W25Q_SectorErase(uint32_t addr) {
  if (!s_present || (addr % W25Q_SECTOR_SIZE))
    return false;

  write_enable();
  cs_low();
  spi_xfer(W25Q_CMD_SECTOR_ERASE);
  send_addr(addr);
  cs_high();
  return wait_ready();
}

bool W25Q_PageProgram(uint32_t addr, const void *src, uint32_t len) {
  if (!s_present || !len || len > W25Q_PAGE_SIZE ||
      (addr % W25Q_PAGE_SIZE) + len > W25Q_PAGE_SIZE)
    return false;
  const uint8_t *p = (const uint8_t *)src;

  write_enable();
  cs_low();
  spi_xfer(W25Q_CMD_PAGE_PROGRAM);
  send_addr(addr);
  for (uint32_t i = 0; i < len; ++i)
    spi_xfer(p[i]);
  cs_high();
  return wait_ready();
}
