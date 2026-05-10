#include "si473x.h"
#include "board.h"
#include "at32f423.h"

#define I2C_ADDR 0x22

#define PIN_SCL    GPIO_PINS_0
#define PORT_SCL   GPIOC
#define PIN_SDA    GPIO_PINS_1
#define PORT_SDA   GPIOC

#define PIN_SET(port, pin) ((port)->scr = (pin))
#define PIN_CLR(port, pin) ((port)->clr = (pin))
#define PIN_GET(port, pin) ((port)->idt & (pin))

SI47XX_MODE si4732mode = SI47XX_FM;
bool isSi4732On = false;

static void sda_input(void)
{
    gpio_init_type g;
    gpio_default_para_init(&g);
    g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    g.gpio_mode = GPIO_MODE_INPUT;
    g.gpio_pins = PIN_SDA;
    g.gpio_pull = GPIO_PULL_NONE;
    gpio_init(PORT_SDA, &g);
}

static void sda_output(void)
{
    gpio_init_type g;
    gpio_default_para_init(&g);
    g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    g.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
    g.gpio_mode = GPIO_MODE_OUTPUT;
    g.gpio_pins = PIN_SDA;
    g.gpio_pull = GPIO_PULL_UP;
    gpio_init(PORT_SDA, &g);
}

static void i2c_delay(void)
{
    __NOP(); __NOP(); __NOP(); __NOP();
    __NOP(); __NOP(); __NOP(); __NOP();
}

static void i2c_start(void)
{
    sda_output();
    PIN_SET(PORT_SDA, PIN_SDA);
    PIN_SET(PORT_SCL, PIN_SCL);
    i2c_delay();
    PIN_CLR(PORT_SDA, PIN_SDA);
    i2c_delay();
    PIN_CLR(PORT_SCL, PIN_SCL);
}

static void i2c_stop(void)
{
    sda_output();
    PIN_CLR(PORT_SDA, PIN_SDA);
    i2c_delay();
    PIN_SET(PORT_SCL, PIN_SCL);
    i2c_delay();
    PIN_SET(PORT_SDA, PIN_SDA);
    i2c_delay();
}

static bool i2c_write_byte(uint8_t data)
{
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
    bool ack = PIN_GET(PORT_SDA, PIN_SDA) == 0;
    PIN_CLR(PORT_SCL, PIN_SCL);
    return ack;
}

static uint8_t i2c_read_byte(bool ack)
{
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
    sda_output();
    if (ack)
        PIN_CLR(PORT_SDA, PIN_SDA);
    else
        PIN_SET(PORT_SDA, PIN_SDA);
    i2c_delay();
    PIN_SET(PORT_SCL, PIN_SCL);
    i2c_delay();
    PIN_CLR(PORT_SCL, PIN_SCL);
    return data;
}

static void wait_cts(void)
{
    uint8_t st;
    do {
        i2c_start();
        i2c_write_byte(I2C_ADDR << 1 | 1);
        st = i2c_read_byte(false);
        i2c_stop();
    } while (!(st & STATUS_CTS));
}

void SI47XX_WriteBuffer(const uint8_t *buf, uint8_t len)
{
    i2c_start();
    i2c_write_byte(I2C_ADDR << 1);
    for (uint8_t i = 0; i < len; i++)
        i2c_write_byte(buf[i]);
    i2c_stop();
}

void SI47XX_ReadBuffer(uint8_t *buf, uint8_t len)
{
    i2c_start();
    i2c_write_byte(I2C_ADDR << 1 | 1);
    for (uint8_t i = 0; i < len; i++)
        buf[i] = i2c_read_byte(i < len - 1);
    i2c_stop();
}

void SI47XX_SetProperty(uint16_t prop, uint16_t value)
{
    wait_cts();
    uint8_t cmd[6] = {CMD_SET_PROPERTY, 0, prop >> 8, prop & 0xFF, value >> 8, value & 0xFF};
    SI47XX_WriteBuffer(cmd, 6);
    delay_ms(8);
}

void SI47XX_Init(void)
{
    gpio_init_type g;
    crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);

    gpio_default_para_init(&g);
    g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    g.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
    g.gpio_mode = GPIO_MODE_OUTPUT;
    g.gpio_pins = PIN_SCL | PIN_SDA;
    g.gpio_pull = GPIO_PULL_UP;
    gpio_init(PORT_SCL, &g);
}

void SI47XX_PowerUp(void)
{
    uint8_t mode = (si4732mode == SI47XX_AM) ? FUNC_AM : FUNC_FM;
    uint8_t cmd[3] = {CMD_POWER_UP, 0x10 | mode, OUT_ANALOG};
    wait_cts();
    SI47XX_WriteBuffer(cmd, 3);
    delay_ms(500);
    isSi4732On = true;
    SI47XX_SetVolume(63);
}

void SI47XX_PowerDown(void)
{
    uint8_t cmd[1] = {CMD_POWER_DOWN};
    wait_cts();
    SI47XX_WriteBuffer(cmd, 1);
    delay_ms(10);
    isSi4732On = false;
}

void SI47XX_SetVolume(uint8_t volume)
{
    if (volume > 63) volume = 63;
    SI47XX_SetProperty(PROP_RX_VOLUME, volume);
}

void SI47XX_SetBandwidth(SI47XX_FilterBW bw, bool amplflt)
{
    uint16_t val = (amplflt ? 0x10 : 0) | (bw & 0x0F);
    SI47XX_SetProperty(0x0100, val);
}

void SI47XX_SetSsbBandwidth(SI47XX_SsbFilterBW bw)
{
    uint16_t val = (bw << 12) | (1 << 8) | 0x10;
    SI47XX_SetProperty(PROP_SSB_MODE, val);
}

void SI47XX_SetAutomaticGainControl(uint8_t agcdis, uint8_t agcidx)
{
    uint8_t cmd = (si4732mode == SI47XX_FM) ? 0x28 : CMD_AM_AGC_OVERRIDE;
    uint8_t buf[3] = {cmd, agcdis ? 1 : 0, agcidx};
    wait_cts();
    SI47XX_WriteBuffer(buf, 3);
}

void SI47XX_SetFreq(uint16_t freq)
{
    wait_cts();
    if (si4732mode == SI47XX_FM) {
        uint8_t cmd[3] = {CMD_FM_TUNE_FREQ, (freq >> 8) & 0xFF, freq & 0xFF};
        SI47XX_WriteBuffer(cmd, 3);
    } else {
        uint8_t cmd[5] = {CMD_AM_TUNE_FREQ, 0, (freq >> 8) & 0xFF, freq & 0xFF, 0};
        SI47XX_WriteBuffer(cmd, 5);
    }
}

void SI47XX_SwitchMode(SI47XX_MODE mode)
{
    si4732mode = mode;
}

void SI47XX_TuneTo(uint32_t f)
{
    if (si4732mode == SI47XX_FM)
        SI47XX_SetFreq(f / 1000);
    else
        SI47XX_SetFreq(f / 100);
}

uint16_t SI47XX_ReadRegister(uint8_t reg)
{
    (void)reg;
    uint8_t buf[8] = {0};
    uint8_t cmd[2] = {CMD_GET_REV, 0};
    wait_cts();
    SI47XX_WriteBuffer(cmd, 2);
    SI47XX_ReadBuffer(buf, 8);
    return (buf[2] << 8) | buf[3];
}
