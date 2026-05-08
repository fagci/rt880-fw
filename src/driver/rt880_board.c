#include "rt880_board.h"

static uint32_t fac_ms;

static void gpio_output_init(gpio_type *port, uint16_t pin, crm_periph_clock_type clk)
{
    gpio_init_type gpio_init_struct;
    crm_periph_clock_enable(clk, TRUE);
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init_struct.gpio_out_type  = GPIO_OUTPUT_PUSH_PULL;
    gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_init_struct.gpio_pins = pin;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init(port, &gpio_init_struct);
}

void rt880_led_init(void)
{
    gpio_output_init(RT880_LED_PORT, RT880_LED_RED_PIN, RT880_LED_CRM_CLK);
    gpio_output_init(RT880_LED_PORT, RT880_LED_GREEN_PIN, RT880_LED_CRM_CLK);
    gpio_output_init(RT880_LED_PORT, RT880_LED_BKLIGHT_PIN, RT880_LED_CRM_CLK);
}

void rt880_led_on(void)
{
    RT880_LED_PORT->scr = RT880_LED_RED_PIN | RT880_LED_GREEN_PIN | RT880_LED_BKLIGHT_PIN;
}

void rt880_led_off(void)
{
    RT880_LED_PORT->clr = RT880_LED_RED_PIN | RT880_LED_GREEN_PIN | RT880_LED_BKLIGHT_PIN;
}

void rt880_led_toggle(void)
{
    RT880_LED_PORT->togr = RT880_LED_RED_PIN | RT880_LED_GREEN_PIN | RT880_LED_BKLIGHT_PIN;
}

void rt880_delay_init(void)
{
    systick_clock_source_config(SYSTICK_CLOCK_SOURCE_AHBCLK_NODIV);
    fac_ms = system_core_clock / 1000U;
}

void rt880_delay_ms(uint32_t ms)
{
    uint32_t temp;
    SysTick->LOAD = (uint32_t)(ms * fac_ms);
    SysTick->VAL = 0x00;
    SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
    do
    {
        temp = SysTick->CTRL;
    } while ((temp & 0x01) && !(temp & (1 << 16)));
    SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
    SysTick->VAL = 0x00;
}
