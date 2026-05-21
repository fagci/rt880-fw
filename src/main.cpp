#include "at32f423.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "modes/mode.h"
#include "modes/vfo.h"
#include "ui/graphics.h"

#define RENDER_INTERVAL_MS 50

int main(void) {
  board_init();

  // Verify we get here - turn off red
  gpio_pin_clr(&PIN_LED_RED);
  gpio_pin_set(&PIN_LED_GREEN); // We got past board_init

  UI_ClearScreen(C_BLACK);
  gpio_pin_clr(&PIN_LED_GREEN); // We got past UI_ClearScreen
  gpio_pin_set(&PIN_LED_RED);   // Mark before Mode_Switch

  Mode_Switch(MODE_VFO);

  gpio_pin_clr(&PIN_LED_RED); // Success - in main loop
  gpio_pin_set(&PIN_LED_GREEN);

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
  }
}
