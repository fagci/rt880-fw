#ifndef RT880_BOOTPROF_H
#define RT880_BOOTPROF_H

#include <stdint.h>

enum {
  PROF_HEXT = 0,
  PROF_CLOCK,
  PROF_UART,
  PROF_GPIO,
  PROF_ADC,
  PROF_LCD,
  PROF_CLEAR,
  PROF_BOARD,
  PROF_LV,
  PROF_SWITCH,
  PROF_UPDATE,
  PROF_DRAW,
  PROF_READY,
  PROF_STAGES
};

/* Включает DWT CYCCNT в начале main (до clock_init, пока 8 МГц). */
void Prof_Init(void);
void Prof_Mark(uint8_t stage);
/* Печатает READY/PROF по UART; повторный вызов безопасен. */
void Prof_Print(void);

#endif /* RT880_BOOTPROF_H */
