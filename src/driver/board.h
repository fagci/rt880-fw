#ifndef RT880_BOARD_H
#define RT880_BOARD_H

#include "at32f423.h"

#define RT880_LED_RED_PIN       GPIO_PINS_4
#define RT880_LED_GREEN_PIN     GPIO_PINS_13
#define RT880_LED_BKLIGHT_PIN   GPIO_PINS_14
#define RT880_LED_PORT          GPIOA
#define RT880_LED_CRM_CLK       CRM_GPIOA_PERIPH_CLOCK

void rt880_led_init(void);
void rt880_led_on(void);
void rt880_led_off(void);
void rt880_led_toggle(void);

void rt880_delay_init(void);
void rt880_delay_ms(uint32_t ms);

#endif
