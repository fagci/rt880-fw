#include "at32f423.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "modes/mode.h"
#include "modes/vfo.h"
#include "ui/graphics.h"
#include "ui/lv_port.h"
#include "lvgl.h"

#define RENDER_INTERVAL_MS 50

int main(void) {
  board_init();

  gpio_pin_clr(&PIN_LED_RED);   // past board_init
  gpio_pin_set(&PIN_LED_GREEN);

  lv_port_init();

  gpio_pin_clr(&PIN_LED_GREEN); // past lv_port_init
  gpio_pin_set(&PIN_LED_RED);

  Mode_Switch(MODE_VFO);

  gpio_pin_clr(&PIN_LED_RED);   // past Mode_Switch
  led_rx_set(false);            // зелёный — теперь индикатор открытого шумодава

  uint32_t lastRender = 0;

  for (;;) {
    key_event_t evt;
    while (keyboard_get_event(&evt))
      Mode_Key(evt.key, evt.type);

    Mode_Update();

    uint32_t now = millis();
    if (now - lastRender >= RENDER_INTERVAL_MS) {
      Mode_Render();
      lastRender = now;
    }

    /* Рендер LVGL: сам решает, есть ли что перерисовывать.
       Возвращает время до следующего таймера — можно спать. */
    uint32_t idle = lv_timer_handler();
    if (idle > 1)
      __WFI();
  }
}
