#include "driver/board.h"
#include "driver/keyboard.h"
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
