#ifndef RT880_BOARD_H
#define RT880_BOARD_H

#include "at32f423.h"

#define RT880_LED_PIN          GPIO_PINS_13
#define RT880_LED_GPIO         GPIOA
#define RT880_LED_CRM_CLK      CRM_GPIOA_PERIPH_CLOCK

#define RT880_LED2_PIN         GPIO_PINS_4
#define RT880_LED2_GPIO        GPIOA
#define RT880_LED2_CRM_CLK     CRM_GPIOA_PERIPH_CLOCK

#define RT880_PTT_PIN          GPIO_PINS_0
#define RT880_PTT_GPIO         GPIOA
#define RT880_PTT_CRM_CLK      CRM_GPIOA_PERIPH_CLOCK

void rt880_led_init(void);
void rt880_led_on(void);
void rt880_led_off(void);
void rt880_led_toggle(void);

void rt880_delay_init(void);
void rt880_delay_ms(uint32_t ms);

#endif
