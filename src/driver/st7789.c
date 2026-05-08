#include "st7789.h"
#include "board.h"
#include "at32f423.h"

/* === Pin Definitions === */
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

/* === Low-level GPIO helpers === */
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

#define PIN_SET(port, pin)   ((port)->scr = (pin))
#define PIN_CLR(port, pin)   ((port)->clr = (pin))

static void spi_write_byte(uint8_t data)
{
    for (int i = 0; i < 8; i++)
    {
        PIN_CLR(PORT_SCK, PIN_SCK);
        if (data & 0x80)
            PIN_SET(PORT_SDA, PIN_SDA);
        else
            PIN_CLR(PORT_SDA, PIN_SDA);
        __NOP();
        PIN_SET(PORT_SCK, PIN_SCK);
        __NOP();
        data <<= 1;
    }
}

static void send_cmd(uint8_t cmd)
{
    PIN_CLR(PORT_DCX, PIN_DCX);
    PIN_CLR(PORT_CS, PIN_CS);
    spi_write_byte(cmd);
    PIN_SET(PORT_CS, PIN_CS);
}

static void send_data(uint8_t data)
{
    PIN_SET(PORT_DCX, PIN_DCX);
    PIN_CLR(PORT_CS, PIN_CS);
    spi_write_byte(data);
    PIN_SET(PORT_CS, PIN_CS);
}

static void send_data16(uint16_t data)
{
    send_data(data >> 8);
    send_data(data & 0xFF);
}

/* === ST7789 Commands === */
#define SWRESET   0x01
#define SLPOUT    0x11
#define NORON     0x13
#define INVOFF    0x20
#define DISPON    0x29
#define CASET     0x2A
#define RASET     0x2B
#define RAMWR     0x2C
#define COLMOD    0x3A
#define MADCTL    0x36

/* === Public API === */
void st7789_init(void)
{
    gpio_pin_init(PORT_DCX, PIN_DCX);
    gpio_pin_init(PORT_RESET, PIN_RESET);
    gpio_pin_init(PORT_SDA, PIN_SDA);
    gpio_pin_init(PORT_SCK, PIN_SCK);
    gpio_pin_init(PORT_CS, PIN_CS);
    gpio_pin_init(PORT_BL, PIN_BL);

    st7789_reset();

    send_cmd(SLPOUT);
    rt880_delay_ms(120);

    send_cmd(COLMOD);
    send_data(0x55); /* 16-bit color */

    send_cmd(MADCTL);
    send_data(0x00);

    send_cmd(INVOFF);

    send_cmd(NORON);

    send_cmd(DISPON);
    rt880_delay_ms(50);

    /* backlight on */
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
    send_cmd(CASET);
    send_data16(0);
    send_data16(ST7789_WIDTH - 1);

    send_cmd(RASET);
    send_data16(0);
    send_data16(ST7789_HEIGHT - 1);

    send_cmd(RAMWR);
    PIN_SET(PORT_DCX, PIN_DCX);
    PIN_CLR(PORT_CS, PIN_CS);

    uint32_t pixels = (uint32_t)ST7789_WIDTH * ST7789_HEIGHT;
    for (uint32_t i = 0; i < pixels; i++)
    {
        spi_write_byte(color >> 8);
        spi_write_byte(color & 0xFF);
    }

    PIN_SET(PORT_CS, PIN_CS);
}
