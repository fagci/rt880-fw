#include "driver/board.h"
#include "driver/keyboard.h"
#include "driver/st7789.h"
#include "modes/mode.h"
#include "modes/vfo.h"
#include "radio.h"
#include "ui/graphics.h"

int main(void) {
  board_init();

  UI_ClearScreen(C_BLACK);

  Mode_Switch(&MODE_VFO);

  for (;;) {
    key_event_t evt;
    while (keyboard_get_event(&evt))
      Mode_Key(evt.key, evt.type);

    Mode_Update();
    Mode_Render();
  }
}
