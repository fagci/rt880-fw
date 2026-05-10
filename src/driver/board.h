#ifndef RT880_BOARD_H
#define RT880_BOARD_H

#include "at32f423.h"
#include <stdbool.h>

#define RT880_LED_RED_PIN GPIO_PINS_4
#define RT880_LED_GREEN_PIN GPIO_PINS_13
#define RT880_LED_BKLIGHT_PIN GPIO_PINS_14
#define RT880_LED_PORT GPIOA
#define RT880_LED_CRM_CLK CRM_GPIOA_PERIPH_CLOCK

#define ANT_SW_PIN GPIO_PINS_0
#define ANT_SW_PORT GPIOF
#define AF_MUTE_PIN GPIO_PINS_8
#define AF_MUTE_PORT GPIOF
#define ANC_PWR_PIN GPIO_PINS_3
#define ANC_PWR_PORT GPIOC
#define AUDIO_PWR_PIN GPIO_PINS_4
#define AUDIO_PWR_PORT GPIOC
#define FM_PWR_PIN GPIO_PINS_5
#define FM_PWR_PORT GPIOC

void rt880_led_init(void);
void rt880_led_on(void);
void rt880_led_off(void);
void rt880_led_toggle(void);

void rt880_delay_init(void);
void rt880_delay_ms(uint32_t ms);
void rt880_delay_us(uint32_t us);

void rt880_audio_init(void);
void rt880_audio_path_set(uint8_t source); /* 0=BK1, 1=BK2 */

void rt880_ant_sw(bool hf);
#endif
