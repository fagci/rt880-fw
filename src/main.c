#include "driver/board.h"
#include "driver/keyboard.h"
#include "driver/si473x.h"
#include "driver/st7789.h"
#include "modes/mode.h"
#include "modes/vfo.h"
#include "radio.h"
#include "ui/graphics.h"

#define RENDER_INTERVAL_MS 50 // ~20 fps максимум

int main(void) {
  board_init();
  UI_ClearScreen(C_BLACK);
  Mode_Switch(&MODE_VFO);

  uint32_t lastRender = 0;
  UI_ClearScreen(C_GRAY);

  Printf(0, 32, "Diagnose SI...");

  SI47XX_Init();

  SI47XX_I2C_Scan();

  Printf(0, 32 + 16 * 1, "ALIVE: %u", SI47XX_IsAlive());
  SI47XX_PowerUp();
  Printf(0, 32 + 16 * 2, "CTS: %u", SI47XX_CheckCTS());
  Printf(0, 32 + 16 * 3, "CHIP ID: %u", SI47XX_CheckChipID());
  SI47XX_SwitchMode(SI47XX_AM);
  SI47XX_TuneTo(1755000);

  gpio_pin_set(&PIN_AF_MUTE);

  for (;;) {
  }

  for (;;) {
    key_event_t evt;
    while (keyboard_get_event(&evt))
      Mode_Key(evt.key, evt.type);

    Mode_Update(); // вызывается каждую итерацию

    uint32_t now = millis();
    if (now - lastRender >= RENDER_INTERVAL_MS) {
      Mode_Render();
      lastRender = now;
    }
  }
}
