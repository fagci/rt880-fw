#include "board.h"

static uint32_t fac_ms;

static void gpio_output_init(gpio_type *port, uint16_t pin,
                             crm_periph_clock_type clk) {
  gpio_init_type g;
  crm_periph_clock_enable(clk, TRUE);
  gpio_default_para_init(&g);
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_mode = GPIO_MODE_OUTPUT;
  g.gpio_pins = pin;
  g.gpio_pull = GPIO_PULL_NONE;
  gpio_init(port, &g);
}

void rt880_led_init(void) {
  gpio_output_init(RT880_LED_PORT, RT880_LED_RED_PIN, RT880_LED_CRM_CLK);
  gpio_output_init(RT880_LED_PORT, RT880_LED_GREEN_PIN, RT880_LED_CRM_CLK);
  gpio_output_init(RT880_LED_PORT, RT880_LED_BKLIGHT_PIN, RT880_LED_CRM_CLK);
}

void rt880_led_on(void) {
  RT880_LED_PORT->scr =
      RT880_LED_RED_PIN | RT880_LED_GREEN_PIN | RT880_LED_BKLIGHT_PIN;
}

void rt880_led_off(void) {
  RT880_LED_PORT->clr =
      RT880_LED_RED_PIN | RT880_LED_GREEN_PIN | RT880_LED_BKLIGHT_PIN;
}

void rt880_ant_sw(bool hf) {
  if (hf) {
    ANT_SW_PORT->scr = ANT_SW_PIN;
  } else {
    ANT_SW_PORT->clr = ANT_SW_PIN;
  }
}

void rt880_led_toggle(void) {
  RT880_LED_PORT->togr =
      RT880_LED_RED_PIN | RT880_LED_GREEN_PIN | RT880_LED_BKLIGHT_PIN;
}

void rt880_delay_init(void) {
  systick_clock_source_config(SYSTICK_CLOCK_SOURCE_AHBCLK_NODIV);
  fac_ms = system_core_clock / 1000U;
}

void rt880_delay_ms(uint32_t ms) {
  uint32_t temp;
  SysTick->LOAD = (uint32_t)(ms * fac_ms);
  SysTick->VAL = 0x00;
  SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
  do {
    temp = SysTick->CTRL;
  } while ((temp & 0x01) && !(temp & (1 << 16)));
  SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
  SysTick->VAL = 0x00;
}

void rt880_delay_us(uint32_t us) {
  uint32_t temp;
  SysTick->LOAD = us * (fac_ms / 1000);
  SysTick->VAL = 0x00;
  SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
  do {
    temp = SysTick->CTRL;
  } while ((temp & 0x01) && !(temp & (1 << 16)));
  SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
  SysTick->VAL = 0x00;
}

void rt880_audio_init(void) {
  gpio_output_init(AF_MUTE_PORT, AF_MUTE_PIN, CRM_GPIOF_PERIPH_CLOCK);
  gpio_output_init(ANC_PWR_PORT, ANC_PWR_PIN, CRM_GPIOC_PERIPH_CLOCK);
  gpio_output_init(AUDIO_PWR_PORT, AUDIO_PWR_PIN, CRM_GPIOC_PERIPH_CLOCK);
  gpio_output_init(FM_PWR_PORT, FM_PWR_PIN, CRM_GPIOC_PERIPH_CLOCK);

  AF_MUTE_PORT->clr = AF_MUTE_PIN;
  ANC_PWR_PORT->clr = ANC_PWR_PIN;
  AUDIO_PWR_PORT->clr = AUDIO_PWR_PIN;
  FM_PWR_PORT->clr = FM_PWR_PIN;
}

void rt880_audio_path_set(uint8_t source) {
  if (source == 0) {
    ANC_PWR_PORT->clr = ANC_PWR_PIN;
  } else {
    ANC_PWR_PORT->scr = ANC_PWR_PIN;
  }
}
