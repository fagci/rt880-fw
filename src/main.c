#include "driver/board.h"
#include "driver/keyboard.h"
#include "modes/mode.h"
#include "modes/vfo.h"

#define RENDER_INTERVAL_MS 16

int main(void) {
  board_init();
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
