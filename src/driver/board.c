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

// board.h — добавить пин как аналоговый (без pull):
// PC2 = ADC1_IN12 на AT32F423 (проверь datasheet пин-маппинг)

void rt880_adc_init(void) {
  // Тактирование
  crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE); // ← добавить
  crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);

  // PC2 в аналоговый режим (keyin)
  gpio_init_type g;
  gpio_default_para_init(&g);
  g.gpio_mode = GPIO_MODE_ANALOG;
  g.gpio_pins = GPIO_PINS_2;
  gpio_init(GPIOC, &g);

  // PA7 в аналоговый режим (батарея)  ← добавить
  gpio_default_para_init(&g);
  g.gpio_mode = GPIO_MODE_ANALOG;
  g.gpio_pins = GPIO_PINS_7;
  gpio_init(GPIOA, &g);

  // ADC: одиночное преобразование, без прерываний
  adc_base_config_type adc_cfg;
  adc_base_default_para_init(&adc_cfg);
  adc_cfg.sequence_mode = FALSE;
  adc_cfg.repeat_mode = FALSE;
  adc_cfg.data_align = ADC_RIGHT_ALIGNMENT;
  adc_cfg.ordinary_channel_length = 1;
  adc_base_config(ADC1, &adc_cfg);
  adc_enable(ADC1, TRUE);

  // Калибровка
  adc_calibration_init(ADC1);
  while (adc_calibration_init_status_get(ADC1))
    ;
  adc_calibration_start(ADC1);
  while (adc_calibration_status_get(ADC1))
    ;
}

uint16_t rt880_adc_read_battery(void) {
  // PA7 = ADC1_IN7 на AT32F423
  adc_ordinary_channel_set(ADC1, ADC_CHANNEL_7, 1, ADC_SAMPLETIME_47_5);
  adc_ordinary_software_trigger_enable(ADC1, TRUE);
  while (!adc_flag_get(ADC1, ADC_OCCE_FLAG))
    ;
  return adc_ordinary_conversion_data_get(ADC1);
}

// Возвращает напряжение батареи в сотых вольта (например 372 = 3.72 В)
uint16_t battery_voltage_cv(void) {
  uint32_t raw = rt880_adc_read_battery();
  // Vbat_cV = raw * Vref_cV * (R1+R2) / (ADC_MAX * R2)
  // = raw * 330 * 347 / (4095 * 47)
  return (uint16_t)((raw * 330UL * 347UL) / (4095UL * 47UL));
}

uint16_t rt880_adc_read_keyin(void) {
  // Канал 12 = PC2 (уточни по datasheet AT32F423)
  adc_ordinary_channel_set(ADC1, ADC_CHANNEL_12, 1, ADC_SAMPLETIME_47_5);
  adc_ordinary_software_trigger_enable(ADC1, TRUE);
  while (!adc_flag_get(ADC1, ADC_OCCE_FLAG))
    ;
  return adc_ordinary_conversion_data_get(ADC1);
}

void board_init(void) {
  SCB->VTOR = 0x08002800; // таблица векторов не на 0x08000000

  clock_init();
  dwt_init();
  SysTick_Config(SystemCoreClock / 1000); // 1 мс тик

  gpio_pin_init_output(&PIN_LED_RED);
  gpio_pin_init_output(&PIN_LED_GREEN);
  gpio_pin_init_output(&PIN_LCD_BACKLIGHT);
  gpio_pin_init_output(&PIN_KEYBOARD_BACKLIGHT);

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

  rt880_adc_init();

  keyboard_init();

  st7789_init();
  st7789_backlight_on();
  gpio_pin_set(&PIN_KEYBOARD_BACKLIGHT);
}
