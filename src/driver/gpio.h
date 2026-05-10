#ifndef RT880_GPIO_H
#define RT880_GPIO_H

#include "at32f423.h"
#include <stdbool.h>

/* ── Дескриптор пина ─────────────────────────────────────────────────── */
typedef struct {
  gpio_type *port;
  uint16_t pin;              /* GPIO_PINS_x */
  crm_periph_clock_type clk; /* CRM_GPIOx_PERIPH_CLOCK */
} gpio_pin_t;

/* Удобный инициализатор:
 *   static const gpio_pin_t MY_PIN = GPIO_PIN(GPIOB, GPIO_PINS_5,
 * CRM_GPIOB_PERIPH_CLOCK);
 */
#define GPIO_PIN(p, n, c)                                                      \
  { .port = (p), .pin = (n), .clk = (c) }

/* ── Инициализация ───────────────────────────────────────────────────── */
void gpio_pin_init_output(const gpio_pin_t *p);
void gpio_pin_init_input(const gpio_pin_t *p, gpio_pull_type pull);

/* ── Управление выходом ──────────────────────────────────────────────── */
static inline void gpio_pin_set(const gpio_pin_t *p) { p->port->scr = p->pin; }
static inline void gpio_pin_clr(const gpio_pin_t *p) { p->port->clr = p->pin; }
static inline void gpio_pin_toggle(const gpio_pin_t *p) {
  p->port->togr = p->pin;
}
static inline void gpio_pin_write(const gpio_pin_t *p, bool v) {
  v ? gpio_pin_set(p) : gpio_pin_clr(p);
}

/* ── Чтение входа ────────────────────────────────────────────────────── */
static inline bool gpio_pin_read(const gpio_pin_t *p) {
  return (p->port->idt & p->pin) != 0;
}

#endif /* RT880_GPIO_H */
