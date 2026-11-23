#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/* Карта пинов RT-880 (AT32F423RCT7, LQFP64).
 *
 * Номера в комментариях — физические ноги U502, подписи — как в схеме
 * RT880 V2. Здесь только дескрипторы; режим (вход/выход/аналог) задаётся
 * в board.cpp и драйверах. */

#include "at32f423.h"
#include "driver/gpio.h"

/* ── Индикация и подсветка ──────────────────────────────────────────── */
/* PA4, нога 20: BACKLCD-EN — подсветка LCD. */
static const gpio_pin_t PIN_LCD_BACKLIGHT =
    GPIO_PIN(GPIOA, GPIO_PINS_4, CRM_GPIOA_PERIPH_CLOCK);
/* PA9, нога 42: подсветка клавиатуры. */
static const gpio_pin_t PIN_KEYBOARD_BACKLIGHT =
    GPIO_PIN(GPIOA, GPIO_PINS_9, CRM_GPIOA_PERIPH_CLOCK);
/* PA13, нога 46: PDA/RX-CLGLED — зелёный светодиод приёма. */
static const gpio_pin_t PIN_LED_GREEN =
    GPIO_PIN(GPIOA, GPIO_PINS_13, CRM_GPIOA_PERIPH_CLOCK);
/* PA14, нога 49: PCK/TX-RLED — красный светодиод. */
static const gpio_pin_t PIN_LED_RED =
    GPIO_PIN(GPIOA, GPIO_PINS_14, CRM_GPIOA_PERIPH_CLOCK);

/* ── Питание и коммутация трактов ───────────────────────────────────── */
/* PF8, нога 31: AF MUTE. */
static const gpio_pin_t PIN_AF_MUTE =
    GPIO_PIN(GPIOF, GPIO_PINS_8, CRM_GPIOF_PERIPH_CLOCK);
/* PC5, нога 25: FM-POWER-EN (SI4732). */
static const gpio_pin_t PIN_FM_POWER =
    GPIO_PIN(GPIOC, GPIO_PINS_5, CRM_GPIOC_PERIPH_CLOCK);
/* PC4, нога 24: RXTS-SW — выбор источника звука U501. */
static const gpio_pin_t PIN_RX_TS =
    GPIO_PIN(GPIOC, GPIO_PINS_4, CRM_GPIOC_PERIPH_CLOCK);
/* PC3, нога 11: ANC POWER-EN — выбор аудиотракта BK1/BK2. */
static const gpio_pin_t PIN_ANC_POWER =
    GPIO_PIN(GPIOC, GPIO_PINS_3, CRM_GPIOC_PERIPH_CLOCK);
/* PF0, нога 5: UV/HF ANT-SW -> Q119 -> катушка K200.
 * HIGH — ВЧ-тракт (HF/U8, второй разъём), LOW — V/U (800M-ANT). */
static const gpio_pin_t PIN_ANT_SWITCH =
    GPIO_PIN(GPIOF, GPIO_PINS_0, CRM_GPIOF_PERIPH_CLOCK);

/* ── Клавиатура ─────────────────────────────────────────────────────── */
/* Столбцы KEYOUT1..4: PB10 (29), PB2 (28), PB1 (27), PB0 (26). */
static const gpio_pin_t PIN_KEY_O1 =
    GPIO_PIN(GPIOB, GPIO_PINS_10, CRM_GPIOB_PERIPH_CLOCK);
static const gpio_pin_t PIN_KEY_O2 =
    GPIO_PIN(GPIOB, GPIO_PINS_2, CRM_GPIOB_PERIPH_CLOCK);
static const gpio_pin_t PIN_KEY_O3 =
    GPIO_PIN(GPIOB, GPIO_PINS_1, CRM_GPIOB_PERIPH_CLOCK);
static const gpio_pin_t PIN_KEY_O4 =
    GPIO_PIN(GPIOB, GPIO_PINS_0, CRM_GPIOB_PERIPH_CLOCK);
/* Строки KEYIN1..4: PC10 (51), PC11 (52), PC12 (53), PD2 (54). */
static const gpio_pin_t PIN_KEY_M1 =
    GPIO_PIN(GPIOC, GPIO_PINS_10, CRM_GPIOC_PERIPH_CLOCK);
static const gpio_pin_t PIN_KEY_M2 =
    GPIO_PIN(GPIOC, GPIO_PINS_11, CRM_GPIOC_PERIPH_CLOCK);
static const gpio_pin_t PIN_KEY_M3 =
    GPIO_PIN(GPIOC, GPIO_PINS_12, CRM_GPIOC_PERIPH_CLOCK);
static const gpio_pin_t PIN_KEY_M4 =
    GPIO_PIN(GPIOD, GPIO_PINS_2, CRM_GPIOD_PERIPH_CLOCK);
/* PB6, нога 58: PTTKEY. */
static const gpio_pin_t PIN_KEY_PTT =
    GPIO_PIN(GPIOB, GPIO_PINS_6, CRM_GPIOB_PERIPH_CLOCK);

/* ── UART на USB-разъёме ────────────────────────────────────────────── */
/* PA11/PA12, ноги 44/45: USART6 AF8 (TX/RX), как в стоке. Это не USB:
 * кабель CPS/прошивки — обычный USB-UART, линии идут на D+/D-. */
static const gpio_pin_t PIN_UART_TX =
    GPIO_PIN(GPIOA, GPIO_PINS_11, CRM_GPIOA_PERIPH_CLOCK);
static const gpio_pin_t PIN_UART_RX =
    GPIO_PIN(GPIOA, GPIO_PINS_12, CRM_GPIOA_PERIPH_CLOCK);

/* ── Внешняя SPI-флеш W25Q32 (U509) ─────────────────────────────────── */
/* PB12, нога 33: W25Q-CS (программный CS). */
static const gpio_pin_t PIN_FLASH_CS =
    GPIO_PIN(GPIOB, GPIO_PINS_12, CRM_GPIOB_PERIPH_CLOCK);
/* PB13/14/15, ноги 34/35/36: W25Q-SCK/DO/DI — SPI2, AF5. */
static const gpio_pin_t PIN_FLASH_SCK =
    GPIO_PIN(GPIOB, GPIO_PINS_13, CRM_GPIOB_PERIPH_CLOCK);
static const gpio_pin_t PIN_FLASH_DO =
    GPIO_PIN(GPIOB, GPIO_PINS_14, CRM_GPIOB_PERIPH_CLOCK);
static const gpio_pin_t PIN_FLASH_DI =
    GPIO_PIN(GPIOB, GPIO_PINS_15, CRM_GPIOB_PERIPH_CLOCK);

/* ── Аналоговые входы ───────────────────────────────────────────────── */
/* PC2, нога 10: KEYIN-AD — резистивная лестница SIDE1/SIDE2/ALARM. */
static const gpio_pin_t PIN_KEY_ADC =
    GPIO_PIN(GPIOC, GPIO_PINS_2, CRM_GPIOC_PERIPH_CLOCK);
/* PA7, нога 23: LOWBATTY-DET — делитель батареи, ADC1_IN7. */
static const gpio_pin_t PIN_BAT =
    GPIO_PIN(GPIOA, GPIO_PINS_7, CRM_GPIOA_PERIPH_CLOCK);
/* ── I2C SI4732 (делится с клавишами) ───────────────────────────────── */
/* PC0/PC1, ноги 8/9: CODE2/CODE1-ADIN — I2C1 AF7, SCL/SDA SI4732. */
static const gpio_pin_t PIN_SI_SCL =
    GPIO_PIN(GPIOC, GPIO_PINS_0, CRM_GPIOC_PERIPH_CLOCK);
static const gpio_pin_t PIN_SI_SDA =
    GPIO_PIN(GPIOC, GPIO_PINS_1, CRM_GPIOC_PERIPH_CLOCK);

#endif /* BOARD_PINS_H */
