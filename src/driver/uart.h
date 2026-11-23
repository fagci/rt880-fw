#ifndef RT880_UART_H
#define RT880_UART_H

#include <stddef.h>
#include <stdint.h>

/* USART6 на PA11/PA12 (ноги 44/45, разъём USB): в стоке — кабель CPS.
 * Передача опрашивается, приём идёт в кольцо из прерывания. */
#define UART_BAUD_DEFAULT 115200u

#ifdef __cplusplus
extern "C" {
#endif

void uart_init(uint32_t baud);

void uart_putc(char c);
void uart_write(const void *data, size_t len);
void uart_puts(const char *s);
void uart_flush(void);
void uart_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* -1, если буфер пуст. */
int uart_getc(void);
size_t uart_read(void *dst, size_t cap);
size_t uart_available(void);
/* Сколько байт потеряно из-за переполнения кольца. */
uint32_t uart_rx_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* RT880_UART_H */
