#include "board.h"
#include "at32f423.h"
#include "at32f423_flash.h"
#include "gpio.h"
#include "keyboard.h"

volatile uint32_t tick_ms = 0;

// HEXT=8MHz → PLL → SCLK=150MHz
// 150 = (8/1) * 150/4/2
static void clock_init(void) {
  flash_psr_set(FLASH_WAIT_CYCLE_4);

  CRM->ctrl_bit.hexten = TRUE;
  uint32_t t = 50000;
  while (!CRM->ctrl_bit.hextstbl && --t)
    ;
  if (!t) {
    flash_psr_set(FLASH_WAIT_CYCLE_0);
    return;
  }

  CRM->pllcfg_bit.pllrcs = 1;
  CRM->pllcfg_bit.pllms = 1;
  CRM->pllcfg_bit.pllns = 150;
  CRM->pllcfg_bit.pllfr = 2;

  CRM->ctrl_bit.pllen = TRUE;
  t = 50000;
  while (!CRM->ctrl_bit.pllstbl && --t)
    ;
  if (!t) {
    flash_psr_set(FLASH_WAIT_CYCLE_0);
    return;
  }

  CRM->cfg_bit.ahbdiv = 0;
  CRM->cfg_bit.apb1div = 4;
  CRM->cfg_bit.apb2div = 0;
  CRM->cfg_bit.sclksel = CRM_SCLK_PLL;
  t = 50000;
  while (CRM->cfg_bit.sclksts != CRM_SCLK_PLL && --t)
    ;

  system_core_clock_update();
}

static void dwt_init(void) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void SysTick_Handler(void) {
  tick_ms++;
  if ((tick_ms % KEY_SCAN_PERIOD_MS) == 0)
    keyboard_scan_tick();
}

// ── Задержки (DWT, не зависят от SysTick, работают из ISR) ──────────────────

void delay_us(uint32_t us) {
  uint32_t start = DWT->CYCCNT;
  uint32_t cycles = us * (SystemCoreClock / 1000000U);
  while ((DWT->CYCCNT - start) < cycles)
    ;
}

void delay_ms(uint32_t ms) {
  while (ms--)
    delay_us(1000);
}

uint32_t millis(void) { return tick_ms; }

// ── Периферия ────────────────────────────────────────────────────────────────

void audio_path_set(uint8_t source) {
  if (source)
    gpio_pin_set(&PIN_ANC_POWER);
  else
    gpio_pin_clr(&PIN_ANC_POWER);
}

void board_init(void) {
  SCB->VTOR = 0x08002800; // таблица векторов не на 0x08000000

  clock_init();
  dwt_init();
  SysTick_Config(SystemCoreClock / 1000); // 1 мс тик

  gpio_pin_init_output(&PIN_LED_RED);
  gpio_pin_init_output(&PIN_LED_GREEN);
  gpio_pin_init_output(&PIN_LCD_BACKLIGHT);

  gpio_pin_init_output(&PIN_AF_MUTE);
  gpio_pin_init_output(&PIN_ANC_POWER);
  gpio_pin_init_output(&PIN_RX_TS);
  gpio_pin_init_output(&PIN_FM_POWER);
  gpio_pin_clr(&PIN_AF_MUTE);
  gpio_pin_clr(&PIN_ANC_POWER);
  gpio_pin_clr(&PIN_RX_TS);
  gpio_pin_clr(&PIN_FM_POWER);

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

  keyboard_init();
}
