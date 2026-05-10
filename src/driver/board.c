#include "board.h"
#include "at32f423.h"
#include "at32f423_flash.h"
#include "gpio.h"
#include "keyboard.h"

volatile uint32_t tick_ms = 0;

// HEXT = 8 MHz → PLL → SCLK = 150 MHz
// SCLK = (HEXT / pllms) * pllns / pllfr / 2
//  150 = (8 / 1) * 150 / 4 / 2  ✓

void system_clock_config(void) {
  flash_psr_set(FLASH_WAIT_CYCLE_4);

  CRM->ctrl_bit.hexten = TRUE;
  uint32_t timeout = 50000;
  while (CRM->ctrl_bit.hextstbl == 0 && --timeout)
    ;

  if (timeout == 0) {
    flash_psr_set(FLASH_WAIT_CYCLE_0);
    return;
  }

  CRM->pllcfg_bit.pllrcs = 1;
  CRM->pllcfg_bit.pllms = 1;
  CRM->pllcfg_bit.pllns = 150;
  CRM->pllcfg_bit.pllfr = 2;

  CRM->ctrl_bit.pllen = TRUE;
  timeout = 50000;
  while (CRM->ctrl_bit.pllstbl == 0 && --timeout)
    ;

  if (timeout == 0) {
    flash_psr_set(FLASH_WAIT_CYCLE_0);
    return;
  }

  CRM->cfg_bit.ahbdiv = 0;
  CRM->cfg_bit.apb1div = 4;
  CRM->cfg_bit.apb2div = 0;

  CRM->cfg_bit.sclksel = CRM_SCLK_PLL;
  timeout = 50000;
  while (CRM->cfg_bit.sclksts != CRM_SCLK_PLL && --timeout)
    ;

  system_core_clock_update();
}

void SysTick_Handler(void) {
  tick_ms++;
  if ((tick_ms % KEY_SCAN_PERIOD_MS) == 0)
    keyboard_scan_tick();
}

void rt880_tick_init(void) { SysTick_Config(SystemCoreClock / 1000); }

uint32_t rt880_tick_ms(void) { return tick_ms; }

void rt880_dwt_init(void) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t rt880_dwt_ms(void) { return DWT->CYCCNT / (SystemCoreClock / 1000); }

void rt880_led_init(void) {
  gpio_pin_init_output(&PIN_LED_RED);
  gpio_pin_init_output(&PIN_LED_GREEN);
  gpio_pin_init_output(&PIN_LCD_BACKLIGHT);
}

// DWT-based: работает из любого контекста, не зависит от SysTick
void rt880_delay_us(uint32_t us) {
  uint32_t start = DWT->CYCCNT;
  uint32_t cycles = us * (SystemCoreClock / 1000000U);
  while ((DWT->CYCCNT - start) < cycles)
    ;
}

// Разбито на 1 мс куски — нет переполнения uint32_t при любом ms
void rt880_delay_ms(uint32_t ms) {
  while (ms--)
    rt880_delay_us(1000);
}

void rt880_audio_init(void) {
  gpio_pin_init_output(&PIN_AF_MUTE);
  gpio_pin_init_output(&PIN_ANC_POWER);
  gpio_pin_init_output(&PIN_RX_TS);
  gpio_pin_init_output(&PIN_FM_POWER);

  gpio_pin_clr(&PIN_AF_MUTE);
  gpio_pin_clr(&PIN_ANC_POWER);
  gpio_pin_clr(&PIN_RX_TS);
  gpio_pin_clr(&PIN_FM_POWER);
}

void rt880_keyboard_init(void) {
  gpio_pin_init_input(&PIN_KEY_M1, true);
  gpio_pin_init_input(&PIN_KEY_M2, true);
  gpio_pin_init_input(&PIN_KEY_M3, true);
  gpio_pin_init_input(&PIN_KEY_M4, true);

  gpio_pin_init_output(&PIN_KEY_O1);
  gpio_pin_init_output(&PIN_KEY_O2);
  gpio_pin_init_output(&PIN_KEY_O3);
  gpio_pin_init_output(&PIN_KEY_O4);

  gpio_pin_set(&PIN_KEY_O1);
  gpio_pin_set(&PIN_KEY_O2);
  gpio_pin_set(&PIN_KEY_O3);
  gpio_pin_set(&PIN_KEY_O4);
}

void rt880_audio_path_set(uint8_t source) {
  if (source == 0)
    gpio_pin_clr(&PIN_ANC_POWER);
  else
    gpio_pin_set(&PIN_ANC_POWER);
}

void rt880_init(void) {
  SCB->VTOR = FLASH_BASE | 0x2800;
  system_clock_config();
  rt880_dwt_init();  // DWT должен быть до первого delay
  rt880_tick_init(); // SysTick: 1 мс прерывание
  rt880_led_init();
  rt880_audio_init();
  keyboard_init();
}
