#include "uart.h"

#include "at32f423.h"
#include "at32f423_usart.h"
#include "board_pins.h"
#include "gpio.h"

#include <stdarg.h>
#include <stdio.h>

#define UART_RX_SIZE 256u
#define UART_RX_MASK (UART_RX_SIZE - 1u)

static volatile uint8_t s_rx[UART_RX_SIZE];
static volatile uint16_t s_rxHead;
static volatile uint16_t s_rxTail;
static volatile uint32_t s_rxDropped;
static bool s_ready;

void uart_init(uint32_t baud) {
  s_ready = false;

  /* В стоке на этих ногах USART6 (AF8), OTG не используется: его тактирование
   * и прерывания могут удерживать PA11/PA12. */
  nvic_irq_disable(USART6_IRQn);
  nvic_irq_disable(OTGFS1_IRQn);
  nvic_irq_disable(OTGFS1_WKUP_IRQn);
  crm_periph_clock_enable(CRM_OTGFS1_PERIPH_CLOCK, FALSE);

  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_USART6_PERIPH_CLOCK, TRUE);
  usart_reset(USART6);

  usart_init(USART6, baud, USART_DATA_8BITS, USART_STOP_1_BIT);
  usart_transmitter_enable(USART6, TRUE);
  usart_receiver_enable(USART6, TRUE);
  usart_enable(USART6, TRUE);

  gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE11, GPIO_MUX_8);
  gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE12, GPIO_MUX_8);

  gpio_init_type g;
  gpio_default_para_init(&g);
  g.gpio_mode = GPIO_MODE_MUX;
  g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_pull = GPIO_PULL_UP;
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_pins = (uint16_t)(PIN_UART_TX.pin | PIN_UART_RX.pin);
  gpio_init(GPIOA, &g);

  s_rxHead = 0;
  s_rxTail = 0;
  s_rxDropped = 0;
  s_ready = true;

  usart_interrupt_enable(USART6, USART_RDBF_INT, TRUE);
  nvic_irq_enable(USART6_IRQn, 3, 0);
}

void uart_putc(char c) {
  if (!s_ready)
    return;
  while (!(USART6->sts & USART_TDBE_FLAG))
    ;
  USART6->dt = (uint8_t)c;
}

void uart_write(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  while (len--)
    uart_putc((char)*p++);
}

void uart_puts(const char *s) {
  while (*s)
    uart_putc(*s++);
}

void uart_flush(void) {
  if (!s_ready)
    return;
  while (!(USART6->sts & USART_TDC_FLAG))
    ;
}

void uart_printf(const char *fmt, ...) {
  char buf[256];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (n > 0)
    uart_write(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1u);
}

int uart_getc(void) {
  uint16_t tail = s_rxTail;
  if (tail == s_rxHead)
    return -1;

  int c = s_rx[tail];
  s_rxTail = (uint16_t)((tail + 1u) & UART_RX_MASK);
  return c;
}

size_t uart_read(void *dst, size_t cap) {
  uint8_t *p = (uint8_t *)dst;
  size_t n = 0;

  while (n < cap) {
    int c = uart_getc();
    if (c < 0)
      break;
    p[n++] = (uint8_t)c;
  }
  return n;
}

size_t uart_available(void) {
  return (size_t)((uint16_t)(s_rxHead - s_rxTail) & UART_RX_MASK);
}

uint32_t uart_rx_dropped(void) { return s_rxDropped; }

extern "C" void USART6_IRQHandler(void) {
  uint32_t sts = USART6->sts;

  /* Чтение STS, затем DT снимает флаги ошибок и RDBF. */
  if (sts & (USART_RDBF_FLAG | USART_ROERR_FLAG | USART_FERR_FLAG |
             USART_NERR_FLAG | USART_PERR_FLAG)) {
    uint8_t b = (uint8_t)USART6->dt;
    if (sts & USART_RDBF_FLAG) {
      uint16_t next = (uint16_t)((s_rxHead + 1u) & UART_RX_MASK);
      if (next != s_rxTail) {
        s_rx[s_rxHead] = b;
        s_rxHead = next;
      } else {
        s_rxDropped++;
      }
    }
  }
}
