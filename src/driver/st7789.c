#include "st7789.h"
#include "board.h"
#include "at32f423.h"

#define PIN_DCX    GPIO_PINS_13
#define PORT_DCX   GPIOC
#define PIN_RESET  GPIO_PINS_14
#define PORT_RESET GPIOC
#define PIN_SDA    GPIO_PINS_5
#define PORT_SDA   GPIOB
#define PIN_SCK    GPIO_PINS_3
#define PORT_SCK   GPIOB
#define PIN_CS     GPIO_PINS_15
#define PORT_CS    GPIOA
#define PIN_BL     GPIO_PINS_4
#define PORT_BL    GPIOA

#define PIN_SET(port, pin)   ((port)->scr = (pin))
#define PIN_CLR(port, pin)   ((port)->clr = (pin))

static void gpio_pin_init(gpio_type *port, uint16_t pin)
{
    gpio_init_type g;
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    gpio_default_para_init(&g);
    g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    g.gpio_out_type  = GPIO_OUTPUT_PUSH_PULL;
    g.gpio_mode = GPIO_MODE_OUTPUT;
    g.gpio_pins = pin;
    g.gpio_pull = GPIO_PULL_NONE;
    gpio_init(port, &g);
}

static void delay_cycle(void)
{
    __NOP(); __NOP(); __NOP(); __NOP();
    __NOP(); __NOP(); __NOP(); __NOP();
}

static void spi_write_byte(uint8_t data)
{
    for (int i = 0; i < 8; i++)
    {
        PIN_CLR(PORT_SCK, PIN_SCK);
        if (data & 0x80) PIN_SET(PORT_SDA, PIN_SDA);
        else             PIN_CLR(PORT_SDA, PIN_SDA);
        delay_cycle();
        PIN_SET(PORT_SCK, PIN_SCK);
        delay_cycle();
        data <<= 1;
    }
}

void st7789_cs_low(void)
{
    PIN_CLR(PORT_CS, PIN_CS);
}

void st7789_cs_high(void)
{
    PIN_SET(PORT_CS, PIN_CS);
}

void st7789_write_cmd(uint8_t cmd)
{
    PIN_CLR(PORT_DCX, PIN_DCX);
    spi_write_byte(cmd);
}

void st7789_write_data(uint8_t data)
{
    PIN_SET(PORT_DCX, PIN_DCX);
    spi_write_byte(data);
}

void st7789_write_data16(uint16_t data)
{
    st7789_write_data(data >> 8);
    st7789_write_data(data & 0xFF);
}

void st7789_set_addr_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
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

void st7789_init(void)
{
    gpio_pin_init(PORT_DCX, PIN_DCX);
    gpio_pin_init(PORT_RESET, PIN_RESET);
    gpio_pin_init(PORT_SDA, PIN_SDA);
    gpio_pin_init(PORT_SCK, PIN_SCK);
    gpio_pin_init(PORT_CS, PIN_CS);
    gpio_pin_init(PORT_BL, PIN_BL);

    st7789_cs_high();
    PIN_SET(PORT_DCX, PIN_DCX);
    PIN_SET(PORT_RESET, PIN_RESET);
    PIN_CLR(PORT_BL, PIN_BL);

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

    PIN_SET(PORT_BL, PIN_BL);
}

void st7789_reset(void)
{
    PIN_CLR(PORT_RESET, PIN_RESET);
    rt880_delay_ms(10);
    PIN_SET(PORT_RESET, PIN_RESET);
    rt880_delay_ms(120);
}

void st7789_flush(uint16_t color)
{
    st7789_cs_low();
    st7789_write_cmd(0x2A);
    st7789_write_data16(0); st7789_write_data16(ST7789_WIDTH - 1);
    st7789_write_cmd(0x2B);
    st7789_write_data16(0); st7789_write_data16(ST7789_HEIGHT - 1);
    st7789_write_cmd(0x2C);
    PIN_SET(PORT_DCX, PIN_DCX);
    uint32_t n = (uint32_t)ST7789_WIDTH * ST7789_HEIGHT;
    for (uint32_t i = 0; i < n; i++) {
        spi_write_byte(color >> 8);
        spi_write_byte(color & 0xFF);
    }
    st7789_cs_high();
}
