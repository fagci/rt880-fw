#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "driver/st7789.h"
#include "helper/measurements.h"
#include "ui/graphics.h"
#include "ui/spectrum.h"

int main(void) {
  board_init();

  st7789_init();
  st7789_backlight_on();

  BK4819_SelectChip(0);
  BK4819_Init();
  BK4819_RX_TurnOn();
  BK4819_SetAFC(0);
  // BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, 1);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_TuneTo(17230000, true);

  gpio_pin_set(&PIN_AF_MUTE);

  UI_ClearScreen(C_BLACK);

  for (;;) {
    key_event_t evt;
    while (keyboard_get_event(&evt)) {
      PrintfEx(0, 18 * 1, POS_L, C_WHITE, C_BLACK, "Key %u", evt.key);
    }
    PrintfEx(0, 18 * 2, POS_L, C_WHITE, C_BLACK, "Lalala");

    delay_ms(250);
  }
}
