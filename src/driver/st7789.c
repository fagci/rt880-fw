#include "st7789.h"
#include "at32f423.h"
#include "board.h"

#define PIN_DCX GPIO_PINS_13
#define PORT_DCX GPIOC
#define PIN_RESET GPIO_PINS_14
#define PORT_RESET GPIOC
#define PIN_CS GPIO_PINS_15
#define PORT_CS GPIOA
#define PIN_BL GPIO_PINS_4
#define PORT_BL GPIOA

#define PIN_SET(port, pin) ((port)->scr = (pin))
#define PIN_CLR(port, pin) ((port)->clr = (pin))

/*
 * DMA channel selection for SPI1_TX:
 * AT32F423 uses DMAMUX, so SPI1_TX can be routed to a chosen DMA channel.
 * Here DMA1 channel 1 is used as an example.
 */
#define ST7789_DMA_CH DMA1_CHANNEL1
#define ST7789_DMA_CLK CRM_DMA1_PERIPH_CLOCK
#define ST7789_DMA_MUX_CH DMAMUX_CHANNEL1
#define ST7789_DMA_REQ_SPI1_TX DMAMUX_DMAREQ_ID_SPI1_TX
#define ST7789_DMA_TC_FLAG DMA1_FDT1_FLAG
#define ST7789_DMA_GL_FLAG DMA1_GL1_FLAG

#define ST7789_DMA_PIXELS_PER_CHUNK 1024

static uint8_t st7789_dma_buf[ST7789_DMA_PIXELS_PER_CHUNK * 2];

static void gpio_pin_init(gpio_type *port, uint16_t pin) {
  gpio_init_type g;

  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);

  gpio_default_para_init(&g);
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_mode = GPIO_MODE_OUTPUT;
  g.gpio_pins = pin;
  g.gpio_pull = GPIO_PULL_NONE;
  gpio_init(port, &g);
}

static void spi_hw_init(void) {
  gpio_init_type g;
  spi_init_type s;

  crm_periph_clock_enable(CRM_SPI1_PERIPH_CLOCK, TRUE);

  gpio_default_para_init(&g);
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_mode = GPIO_MODE_MUX;
  g.gpio_pins = GPIO_PINS_3 | GPIO_PINS_5; /* PB3=SCK, PB5=MOSI */
  g.gpio_pull = GPIO_PULL_NONE;
  gpio_init(GPIOB, &g);

  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE3, GPIO_MUX_5);
  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE5, GPIO_MUX_5);

  spi_default_para_init(&s);
  s.transmission_mode = SPI_TRANSMIT_FULL_DUPLEX;
  s.master_slave_mode = SPI_MODE_MASTER;
  s.mclk_freq_division = SPI_MCLK_DIV_2;
  s.first_bit_transmission = SPI_FIRST_BIT_MSB;
  s.frame_bit_num = SPI_FRAME_8BIT;
  s.clock_polarity = SPI_CLOCK_POLARITY_LOW;
  s.clock_phase = SPI_CLOCK_PHASE_1EDGE;
  s.cs_mode_selection = SPI_CS_SOFTWARE_MODE;
  spi_init(SPI1, &s);

  spi_enable(SPI1, TRUE);
}

static void spi_dma_init(void) {
  dma_init_type d;

  crm_periph_clock_enable(ST7789_DMA_CLK, TRUE);

  dma_reset(ST7789_DMA_CH);
  dma_default_para_init(&d);

  d.peripheral_base_addr = (uint32_t)&SPI1->dt;
  d.memory_base_addr = 0;
  d.direction = DMA_DIR_MEMORY_TO_PERIPHERAL;
  d.buffer_size = 0;
  d.peripheral_inc_enable = FALSE;
  d.memory_inc_enable = TRUE;
  d.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
  d.memory_data_width = DMA_MEMORY_DATA_WIDTH_BYTE;
  d.loop_mode_enable = FALSE;
  d.priority = DMA_PRIORITY_HIGH;

  dma_init(ST7789_DMA_CH, &d);

  dmamux_enable(DMA1, TRUE);
  dmamux_init(DMA1MUX_CHANNEL1, DMAMUX_DMAREQ_ID_SPI1_TX);
  spi_i2s_dma_transmitter_enable(SPI1, TRUE);
}

static void spi_write(uint8_t data) {
  while (spi_i2s_flag_get(SPI1, SPI_I2S_TDBE_FLAG) == RESET)
    ;

  spi_i2s_data_transmit(SPI1, data);

  while (spi_i2s_flag_get(SPI1, SPI_I2S_RDBF_FLAG) == RESET)
    ;

  (void)spi_i2s_data_receive(SPI1);
}

static void spi1_tx_dma(const uint8_t *buf, uint16_t len) {
  while (spi_i2s_flag_get(SPI1, SPI_I2S_BF_FLAG) != RESET)
    ;

  dma_channel_enable(ST7789_DMA_CH, FALSE);
  dma_flag_clear(ST7789_DMA_GL_FLAG);

  ST7789_DMA_CH->maddr = (uint32_t)buf;
  ST7789_DMA_CH->dtcnt = len;

  dma_channel_enable(ST7789_DMA_CH, TRUE);

  while (dma_flag_get(ST7789_DMA_TC_FLAG) == RESET)
    ;

  dma_flag_clear(ST7789_DMA_TC_FLAG);
  dma_channel_enable(ST7789_DMA_CH, FALSE);

  while (spi_i2s_flag_get(SPI1, SPI_I2S_BF_FLAG) != RESET)
    ;
}

void st7789_backlight_off() { PIN_CLR(PORT_BL, PIN_BL); }

void st7789_backlight_on() { PIN_SET(PORT_BL, PIN_BL); }

void st7789_cs_low(void) { PIN_CLR(PORT_CS, PIN_CS); }

void st7789_cs_high(void) { PIN_SET(PORT_CS, PIN_CS); }

void st7789_write_cmd(uint8_t cmd) {
  PIN_CLR(PORT_DCX, PIN_DCX);
  spi_write(cmd);
}

void st7789_write_data(uint8_t data) {
  PIN_SET(PORT_DCX, PIN_DCX);
  spi_write(data);
}

void st7789_write_data16(uint16_t data) {
  st7789_write_data((uint8_t)(data >> 8));
  st7789_write_data((uint8_t)(data & 0xFF));
}

void st7789_set_addr_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  st7789_cs_low();

  st7789_write_cmd(0x2A);
  st7789_write_data16(x);
  st7789_write_data16(x + w - 1);

  st7789_write_cmd(0x2B);
  st7789_write_data16(y);
  st7789_write_data16(y + h - 1);

  st7789_write_cmd(0x2C);

  st7789_cs_high();
}

void st7789_start_pixels(void) {}

void st7789_end_pixels(void) {}

void st7789_reset(void) {
  PIN_CLR(PORT_RESET, PIN_RESET);
  rt880_delay_ms(10);
  PIN_SET(PORT_RESET, PIN_RESET);
  rt880_delay_ms(120);
}

void st7789_init(void) {
  gpio_pin_init(PORT_DCX, PIN_DCX);
  gpio_pin_init(PORT_RESET, PIN_RESET);
  gpio_pin_init(PORT_CS, PIN_CS);
  gpio_pin_init(PORT_BL, PIN_BL);

  spi_hw_init();
  spi_dma_init();

  st7789_cs_high();
  PIN_SET(PORT_DCX, PIN_DCX);
  PIN_SET(PORT_RESET, PIN_RESET);
  st7789_backlight_off();

  st7789_reset();

  st7789_cs_low();
  st7789_write_cmd(0x01);
  st7789_cs_high();
  rt880_delay_ms(150);

  st7789_cs_low();
  st7789_write_cmd(0x11);
  st7789_cs_high();
  rt880_delay_ms(120);

  st7789_cs_low();
  st7789_write_cmd(0x3A);
  st7789_write_data(0x55);
  st7789_cs_high();

  st7789_cs_low();
  st7789_write_cmd(0x36);
  st7789_write_data(0x00);
  st7789_cs_high();

  st7789_cs_low();
  st7789_write_cmd(0x13);
  st7789_cs_high();

  st7789_cs_low();
  st7789_write_cmd(0x29);
  st7789_cs_high();
  rt880_delay_ms(50);
}

void st7789_fill_rect_dma(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          uint16_t color) {
  if (x >= ST7789_WIDTH || y >= ST7789_HEIGHT || w == 0 || h == 0)
    return;
  if (x + w > ST7789_WIDTH)
    w = ST7789_WIDTH - x;
  if (y + h > ST7789_HEIGHT)
    h = ST7789_HEIGHT - y;

  uint32_t left = (uint32_t)w * h;

  for (uint32_t i = 0; i < ST7789_DMA_PIXELS_PER_CHUNK; i++) {
    st7789_dma_buf[i * 2 + 0] = (uint8_t)(color >> 8);
    st7789_dma_buf[i * 2 + 1] = (uint8_t)(color & 0xFF);
  }

  st7789_cs_low();

  st7789_write_cmd(0x2A);
  st7789_write_data16(x);
  st7789_write_data16(x + w - 1);

  st7789_write_cmd(0x2B);
  st7789_write_data16(y);
  st7789_write_data16(y + h - 1);

  st7789_write_cmd(0x2C);
  PIN_SET(PORT_DCX, PIN_DCX);

  while (left) {
    uint16_t px = (left > ST7789_DMA_PIXELS_PER_CHUNK)
                      ? ST7789_DMA_PIXELS_PER_CHUNK
                      : (uint16_t)left;
    spi1_tx_dma(st7789_dma_buf, px * 2);
    left -= px;
  }

  st7789_cs_high();
}

void st7789_flush(uint16_t color) {
  st7789_fill_rect_dma(0, 0, ST7789_WIDTH, ST7789_HEIGHT, color);
}

void st7789_set_addr_window_raw(uint16_t x, uint16_t y, uint16_t w,
                                uint16_t h) {
  st7789_write_cmd(0x2A);
  st7789_write_data16(x);
  st7789_write_data16(x + w - 1);

  st7789_write_cmd(0x2B);
  st7789_write_data16(y);
  st7789_write_data16(y + h - 1);

  st7789_write_cmd(0x2C);
  PIN_SET(PORT_DCX, PIN_DCX);
}

void st7789_write_pixels_dma(const uint16_t *pixels, uint32_t count) {
  while (count) {
    uint32_t chunk = count;
    if (chunk > ST7789_DMA_PIXELS_PER_CHUNK)
      chunk = ST7789_DMA_PIXELS_PER_CHUNK;

    for (uint32_t i = 0; i < chunk; i++) {
      st7789_dma_buf[i * 2 + 0] = (uint8_t)(pixels[i] >> 8);
      st7789_dma_buf[i * 2 + 1] = (uint8_t)(pixels[i] & 0xFF);
    }

    spi1_tx_dma(st7789_dma_buf, chunk * 2);
    pixels += chunk;
    count -= chunk;
  }
}

void st7789_set_vscroll_area(uint16_t tfa, uint16_t vsa, uint16_t bfa) {
  st7789_cs_low();
  st7789_write_cmd(0x33); // VSCRDEF
  st7789_write_data16(tfa);
  st7789_write_data16(vsa);
  st7789_write_data16(bfa);
  st7789_cs_high();
}

void st7789_set_vscroll_start(uint16_t vsp) {
  st7789_cs_low();
  st7789_write_cmd(0x37); // VSCRSADD
  st7789_write_data16(vsp);
  st7789_cs_high();
}
