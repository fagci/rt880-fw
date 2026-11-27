#include "board.h"
#include "../helper/bootprof.h"
#include "../ui/graphics.h"
#include "at32f423.h"
#include "at32f423_flash.h"
#include "gpio.h"
#include "keyboard.h"
#include "st7789.h"
#include "uart.h"
#include "boot_layout.h"

volatile uint32_t tick_ms = 0;

// SCLK=150 МГц: (8 МГц * 150 / 1) / 4 / 2. Источник PLL — HEXT; если кварц
// не завёлся — HICK (тоже 8 МГц в этой конфигурации).
// Раньше переключение выполнялось «в лоб» без auto step mode (обязателен
// для >108 МГц): sclksts не становился PLL, цикл ожидания тихо истекал и
// ядро оставалось на 8 МГц (SPI 4 Мбит/с, LVGL в ~19 раз медленнее).
// g_clockStage — для диагностики: 1 HEXT ok, 2 PLL от HICK, 3 PLL залочен,
// 4 SCLK=PLL; 0x8x — отказ на соответствующей стадии.
uint8_t g_clockStage;

static bool wait_flag(uint32_t flag, uint32_t n) {
  while (crm_flag_get(flag) == RESET)
    if (!--n)
      return false;
  return true;
}

static void clock_init(void) {
  flash_psr_set(FLASH_WAIT_CYCLE_4);

  crm_pll_clock_source_type src = CRM_PLL_SOURCE_HEXT;
  crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, TRUE);
  if (wait_flag(CRM_HEXT_STABLE_FLAG, 200000)) {
    g_clockStage = 1;
  } else {
    crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, FALSE);
    src = CRM_PLL_SOURCE_HICK;
    g_clockStage = 2;
  }
  Prof_Mark(PROF_HEXT);

  /* 135, а не 150 МГц. Помеха ходит за тактом SPI дисплея/APB1:
   * на 144 (SPI 48) она была на 432, на 120 (SPI 30) — на 450. Нужен
   * такт, у которого гармоники обходят 440–470: SPI = APB1 = 135/4 =
   * 33.75 МГц, 13-я = 438.75, 14-я = 472.5 — полоса ровно между ними.
   * Ядро 3×135 = 405, 4×135 = 540. */
  crm_pll_config(src, 135, 1, CRM_PLL_FR_4);
  crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
  if (!wait_flag(CRM_PLL_STABLE_FLAG, 200000)) {
    g_clockStage |= 0x80;
    flash_psr_set(FLASH_WAIT_CYCLE_0);
    return;
  }
  g_clockStage = 3;

  crm_ahb_div_set(CRM_AHB_DIV_1);
  crm_apb1_div_set(CRM_APB1_DIV_4);
  crm_apb2_div_set(CRM_APB2_DIV_1);

  crm_auto_step_mode_enable(TRUE);
  crm_sysclk_switch(CRM_SCLK_PLL);
  uint32_t t = 200000;
  while (crm_sysclk_switch_status_get() != CRM_SCLK_PLL && --t)
    ;
  crm_auto_step_mode_enable(FALSE);

  if (!t) {
    g_clockStage |= 0x80;
    flash_psr_set(FLASH_WAIT_CYCLE_0);
    system_core_clock_update();
    return;
  }
  g_clockStage = 4;

  system_core_clock_update();
}

static void dwt_init(void) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  /* CYCCNT не сбрасываем: Prof_Init включает его в начале main. */
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void SysTick_Handler(void) {
  tick_ms++;
  lv_tick_inc(1);
  if ((tick_ms % KEY_SCAN_PERIOD_MS) == 0)
    keyboard_scan_tick();
}

// ── Задержки (DWT, не зависят от SysTick, работают из ISR) ──────────────────

/* Зелёный LED показывает открытый шумодав (приём). Полярность (set = горит)
 * — как в прежних тест-точках/«сердцебиении» в main; при инверсии поменять
 * здесь. */
void led_rx_set(bool on) { gpio_pin_write(&PIN_LED_GREEN, on); }

void keyboard_backlight_set(bool on) {
  gpio_pin_write(&PIN_KEYBOARD_BACKLIGHT, on);
}

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

static bool s_antMain;
static uint32_t s_antSwitchMs;

/* PF0/K200: HIGH — основной вход на ВЧ-тракт, LOW — на V/U. */
void ant_sw(bool hf) {
  s_antMain = hf;
  s_antSwitchMs = millis();
  gpio_pin_write(&PIN_ANT_SWITCH, hf);
}

bool ant_is_main(void) { return s_antMain; }

uint32_t ant_last_switch_ms(void) { return s_antSwitchMs; }

/* PC15 HF-TXPOWER-EN и PB11 TX29/50M-SW. Оба активны высоким уровнем. */
void board_tx_gates(bool hf, bool tx29) {
  gpio_pin_write(&PIN_HF_TX_POWER, hf);
  gpio_pin_write(&PIN_TX29_50M, tx29);
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

  // Тактирование ADC: при 150 МГц ядра hclk/16 ≈ 9.4 МГц (раньше было 4 МГц
  // на 8 МГц) — резистивная лестница клавиш требует длинного семплирования
  adc_common_config_type adc_com;
  adc_common_default_para_init(&adc_com);
  adc_com.div = ADC_HCLK_DIV_16;
  adc_common_config(&adc_com);

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
  for (uint32_t _t = 50000; adc_calibration_init_status_get(ADC1) && --_t; );
  adc_calibration_start(ADC1);
  for (uint32_t _t = 50000; adc_calibration_status_get(ADC1) && --_t; );
}

uint16_t rt880_adc_read_battery(void) {
  // PA7 = ADC1_IN7 на AT32F423
  adc_ordinary_channel_set(ADC1, ADC_CHANNEL_7, 1, ADC_SAMPLETIME_47_5);
  adc_ordinary_software_trigger_enable(ADC1, TRUE);
  for (uint32_t _t = 50000; !adc_flag_get(ADC1, ADC_OCCE_FLAG) && --_t; );
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
  for (uint32_t _t = 50000; !adc_flag_get(ADC1, ADC_OCCE_FLAG) && --_t; );
  return adc_ordinary_conversion_data_get(ADC1);
}

void board_init(void) {
  SCB->VTOR = APP_BASE; // таблица векторов не на 0x08000000

  /* Если загрузчик всё же ушёл в приложение, PB6 остался залоченным выходом
   * в ноль после software-входа. Снимается только сбросом. */
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  if (GPIOB->wpr & GPIO_PINS_6)
    NVIC_SystemReset();

  /* Загрузчик/stage-2 мог оставить PRIMASK; без этого не работают
   * SysTick (LVGL) и прерывание консоли USART6. */
  __enable_irq();

  clock_init();
  Prof_Mark(PROF_CLOCK);
  dwt_init();
  SysTick_Config(SystemCoreClock / 1000); // 1 мс тик

  uart_init(UART_BAUD_DEFAULT);
  Prof_Mark(PROF_UART);

  gpio_pin_init_output(&PIN_LED_RED);
  gpio_pin_init_output(&PIN_LED_GREEN);
  gpio_pin_init_output(&PIN_KEYBOARD_BACKLIGHT);

  gpio_pin_init_output(&PIN_AF_MUTE);
  gpio_pin_init_output(&PIN_ANC_POWER);
  gpio_pin_init_output(&PIN_RX_TS);
  gpio_pin_init_output(&PIN_FM_POWER);
  gpio_pin_init_output(&PIN_ANT_SWITCH);
  gpio_pin_init_output(&PIN_HF_TX_POWER);
  gpio_pin_init_output(&PIN_TX29_50M);
  gpio_pin_clr(&PIN_AF_MUTE);
  gpio_pin_clr(&PIN_ANC_POWER);
  gpio_pin_clr(&PIN_RX_TS);
  gpio_pin_clr(&PIN_FM_POWER);
  gpio_pin_clr(&PIN_HF_TX_POWER);
  gpio_pin_clr(&PIN_TX29_50M);
  ant_sw(false); /* по умолчанию V/U-тракт, как в стоке */

  gpio_pin_init_input(&PIN_KEY_M1, GPIO_PULL_UP);
  gpio_pin_init_input(&PIN_KEY_M2, GPIO_PULL_UP);
  gpio_pin_init_input(&PIN_KEY_M3, GPIO_PULL_UP);
  gpio_pin_init_input(&PIN_KEY_M4, GPIO_PULL_UP);
  gpio_pin_init_output(&PIN_KEY_O1);
  gpio_pin_init_output(&PIN_KEY_O2);
  gpio_pin_init_output(&PIN_KEY_O3);
  gpio_pin_init_output(&PIN_KEY_O4);
  gpio_pin_set(&PIN_KEY_O1);
  gpio_pin_set(&PIN_KEY_O2);
  gpio_pin_set(&PIN_KEY_O3);
  gpio_pin_set(&PIN_KEY_O4);

  gpio_pin_set(&PIN_LED_RED);

  // Test point 1: after GPIO init
  gpio_pin_clr(&PIN_LED_GREEN);
  gpio_pin_set(&PIN_LED_GREEN);

  Prof_Mark(PROF_GPIO);
  rt880_adc_init();
  Prof_Mark(PROF_ADC);

  // Test point 2: after ADC
  gpio_pin_clr(&PIN_LED_GREEN);
  gpio_pin_set(&PIN_LED_GREEN);

  st7789_init();
  Prof_Mark(PROF_LCD);

  // Test point 3: after LCD init
  gpio_pin_clr(&PIN_LED_GREEN);
  gpio_pin_set(&PIN_LED_GREEN);

  UI_ClearScreen(C_BLACK);
  Prof_Mark(PROF_CLEAR);

  st7789_backlight_on();
  keyboard_backlight_set(true);

  keyboard_init();
  Prof_Mark(PROF_BOARD);
}
