#ifndef RT880_BOARD_H
#define RT880_BOARD_H

#include "at32f423.h"
#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>

extern volatile uint32_t tick_ms;

static const gpio_pin_t PIN_LED_GREEN =
    GPIO_PIN(GPIOA, GPIO_PINS_13, CRM_GPIOA_PERIPH_CLOCK);

static const gpio_pin_t PIN_LED_RED =
    GPIO_PIN(GPIOA, GPIO_PINS_14, CRM_GPIOA_PERIPH_CLOCK);

static const gpio_pin_t PIN_LCD_BACKLIGHT =
    GPIO_PIN(GPIOA, GPIO_PINS_4, CRM_GPIOA_PERIPH_CLOCK);

static const gpio_pin_t PIN_AF_MUTE =
    GPIO_PIN(GPIOF, GPIO_PINS_8, CRM_GPIOF_PERIPH_CLOCK);

static const gpio_pin_t PIN_FM_POWER =
    GPIO_PIN(GPIOC, GPIO_PINS_5, CRM_GPIOC_PERIPH_CLOCK);

static const gpio_pin_t PIN_RX_TS =
    GPIO_PIN(GPIOC, GPIO_PINS_4, CRM_GPIOC_PERIPH_CLOCK);

static const gpio_pin_t PIN_ANC_POWER =
    GPIO_PIN(GPIOC, GPIO_PINS_3, CRM_GPIOC_PERIPH_CLOCK);

static const gpio_pin_t PIN_ANT_SWITCH =
    GPIO_PIN(GPIOF, GPIO_PINS_0, CRM_GPIOF_PERIPH_CLOCK);

static const gpio_pin_t PIN_KEY_M1 =
    GPIO_PIN(GPIOC, GPIO_PINS_10, CRM_GPIOC_PERIPH_CLOCK);

static const gpio_pin_t PIN_KEY_M2 =
    GPIO_PIN(GPIOC, GPIO_PINS_11, CRM_GPIOC_PERIPH_CLOCK);

static const gpio_pin_t PIN_KEY_M3 =
    GPIO_PIN(GPIOC, GPIO_PINS_12, CRM_GPIOC_PERIPH_CLOCK);

static const gpio_pin_t PIN_KEY_M4 =
    GPIO_PIN(GPIOD, GPIO_PINS_2, CRM_GPIOD_PERIPH_CLOCK);

static const gpio_pin_t PIN_KEY_O1 =
    GPIO_PIN(GPIOB, GPIO_PINS_10, CRM_GPIOB_PERIPH_CLOCK);

static const gpio_pin_t PIN_KEY_O2 =
    GPIO_PIN(GPIOB, GPIO_PINS_2, CRM_GPIOB_PERIPH_CLOCK);

static const gpio_pin_t PIN_KEY_O3 =
    GPIO_PIN(GPIOB, GPIO_PINS_1, CRM_GPIOB_PERIPH_CLOCK);

static const gpio_pin_t PIN_KEY_O4 =
    GPIO_PIN(GPIOB, GPIO_PINS_0, CRM_GPIOB_PERIPH_CLOCK);

void rt880_tick_init(void);
uint32_t rt880_tick_ms(void);

void rt880_dwt_init(void);
uint32_t rt880_dwt_ms(void);

void rt880_delay_ms(uint32_t ms);
void rt880_delay_us(uint32_t us);

void rt880_audio_init(void);
void rt880_audio_path_set(uint8_t source); /* 0=BK1, 1=BK2 */

void rt880_ant_sw(bool hf);

void rt880_init();
#endif
