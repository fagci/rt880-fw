/* Хост-заглушка SDK AT32 — только то, что нужно, чтобы собрать UI на ПК. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { volatile uint32_t scr, clr, togr, idt, cfgr; } gpio_type;
extern gpio_type g_fake_gpio[8];
#define GPIOA (&g_fake_gpio[0])
#define GPIOB (&g_fake_gpio[1])
#define GPIOC (&g_fake_gpio[2])
#define GPIOD (&g_fake_gpio[3])
#define GPIOF (&g_fake_gpio[5])
#define GPIO_PINS_0 1
#define GPIO_PINS_1 2
#define GPIO_PINS_2 4
#define GPIO_PINS_3 8
#define GPIO_PINS_4 16
#define GPIO_PINS_5 32
#define GPIO_PINS_6 64
#define GPIO_PINS_7 128
#define GPIO_PINS_8 256
#define GPIO_PINS_9 512
#define GPIO_PINS_10 1024
#define GPIO_PINS_11 2048
#define GPIO_PINS_12 4096
#define GPIO_PINS_13 8192
#define GPIO_PINS_14 16384
#define GPIO_PINS_15 32768
typedef enum { GPIO_PULL_NONE, GPIO_PULL_UP, GPIO_PULL_DOWN } gpio_pull_type;
typedef enum { CRM_GPIOA_PERIPH_CLOCK, CRM_GPIOB_PERIPH_CLOCK, CRM_GPIOC_PERIPH_CLOCK,
  CRM_GPIOD_PERIPH_CLOCK, CRM_GPIOF_PERIPH_CLOCK } crm_periph_clock_type;
extern uint32_t SystemCoreClock;
#ifdef __cplusplus
}
#endif
