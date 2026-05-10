#include "gpio.h"

void gpio_pin_init_output(const gpio_pin_t *p) {
  gpio_init_type g;
  crm_periph_clock_enable(p->clk, TRUE);
  gpio_default_para_init(&g);
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_mode = GPIO_MODE_OUTPUT;
  g.gpio_pins = p->pin;
  g.gpio_pull = GPIO_PULL_NONE;
  gpio_init(p->port, &g);
}

void gpio_pin_init_input(const gpio_pin_t *p, gpio_pull_type pull) {
  gpio_init_type g;
  crm_periph_clock_enable(p->clk, TRUE);
  gpio_default_para_init(&g);
  g.gpio_mode = GPIO_MODE_INPUT;
  g.gpio_pins = p->pin;
  g.gpio_pull = pull;
  gpio_init(p->port, &g);
}
