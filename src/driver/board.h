#ifndef BOARD_H
#define BOARD_H

#include "at32f423.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>

extern volatile uint32_t tick_ms;
extern uint8_t g_clockStage; /* стадия clock_init (диагностика) */
/* Зелёный светодиод = «шумодав открыт / слушаем». Одна точка управления. */
void led_rx_set(bool on);
/* Подсветка клавиатуры (PA9): true — включена. */
void keyboard_backlight_set(bool on);

#ifdef __cplusplus
extern "C" {
#endif

void SysTick_Handler(void); /* определён в board.cpp, вектор из startup_at32f423.s */
void lv_tick_inc(uint32_t tick_period); /* LVGL, из lv_hal_tick.c */

void delay_us(uint32_t us);
void delay_ms(uint32_t ms);
uint32_t millis(void);
/* Короткий тон на PA5 (ключевой бипер), блокирующий. */
void board_beep(uint16_t freq_hz, uint16_t ms);

void audio_path_set(uint8_t source); /* 0=BK1, 1=BK2 */
/* PF0/K200: true — ВЧ-тракт (HF/U8), false — V/U. */
void ant_sw(bool hf);
bool ant_is_main(void);      /* текущее положение реле: PF0 == HIGH */
/* Время последнего движения K200 (millis), 0 — ещё не переключали. */
uint32_t ant_last_switch_ms(void);
/* Внешние TX-гейты: PC15 HF-TXPOWER-EN, PB11 TX29/50M-SW. */
void board_tx_gates(bool hf, bool tx29);

void board_init(void);

uint16_t rt880_adc_read_keyin(void);
uint16_t rt880_adc_read_battery(void);
uint16_t battery_voltage_cv(void);

#ifdef __cplusplus
}
#endif

#endif
