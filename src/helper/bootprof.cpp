#include "bootprof.h"

#include "../driver/board.h"
#include "../driver/uart.h"
#include "at32f423.h"

static uint32_t s_cyc[PROF_STAGES];
static bool s_ready;

void Prof_Init(void) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void Prof_Mark(uint8_t stage) {
  if (stage < PROF_STAGES)
    s_cyc[stage] = DWT->CYCCNT;
  if (stage == PROF_READY)
    s_ready = true;
}

static uint32_t since_clock(uint8_t stage) {
  uint32_t mhz = SystemCoreClock / 1000000u;
  if (!mhz)
    return 0;
  return (s_cyc[stage] - s_cyc[PROF_CLOCK]) / mhz;
}

void Prof_Print(void) {
  if (!s_ready)
    return;

  /* До переключения на PLL ядро идёт от HICK 8 МГц; хвост clock_init после
   * переключения считается по 8 МГц — это доли микросекунды. */
  uint32_t pre = s_cyc[PROF_CLOCK] / 8u;
  uint32_t ready = pre + since_clock(PROF_READY);

  uart_printf("READY %lu\r\n", (unsigned long)ready);
  uart_printf("PROF pre=%lu hext=%lu uart=%lu gpio=%lu adc=%lu lcd=%lu "
              "clear=%lu board=%lu lv=%lu sw=%lu upd=%lu draw=%lu "
              "ready=%lu mhz=%lu clk=%u\r\n",
              (unsigned long)pre,
              (unsigned long)(s_cyc[PROF_HEXT] / 8u),
              (unsigned long)(pre + since_clock(PROF_UART)),
              (unsigned long)(pre + since_clock(PROF_GPIO)),
              (unsigned long)(pre + since_clock(PROF_ADC)),
              (unsigned long)(pre + since_clock(PROF_LCD)),
              (unsigned long)(pre + since_clock(PROF_CLEAR)),
              (unsigned long)(pre + since_clock(PROF_BOARD)),
              (unsigned long)(pre + since_clock(PROF_LV)),
              (unsigned long)(pre + since_clock(PROF_SWITCH)),
              (unsigned long)(pre + since_clock(PROF_UPDATE)),
              (unsigned long)(pre + since_clock(PROF_DRAW)),
              (unsigned long)ready,
              (unsigned long)(SystemCoreClock / 1000000u),
              (unsigned)g_clockStage);
}
