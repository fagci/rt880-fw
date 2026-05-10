#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "driver/st7789.h"
#include "helper/measurements.h"
#include "ui/graphics.h"
#include "ui/spectrum.h"

static const char *KEY_NAMES[] = {
    [KEY_NONE] = "NONE", //
    [KEY_1] = "1",       //
    [KEY_2] = "2",       //
    [KEY_3] = "3",       //
    [KEY_4] = "4",       //
    [KEY_5] = "5",       //
    [KEY_6] = "6",       //
    [KEY_7] = "7",       //
    [KEY_8] = "8",       //
    [KEY_9] = "9",       //
    [KEY_0] = "0",       //
    [KEY_MENU] = "MENU", //
    [KEY_UP] = "UP",     //
    [KEY_DOWN] = "DOWN", //
    [KEY_EXIT] = "EXIT", //
    [KEY_STAR] = "STAR", //
    [KEY_HASH] = "HASH", //

    [KEY_PTT] = "PTT",     /* PB6  */
    [KEY_SIDE1] = "SIDE1", /* PC0  */
    [KEY_SIDE2] = "SIDE2", /* PC1  */
    [KEY_ALARM] = "ALARM", /* PC2  */
};

int main(void) {
  board_init();

  st7789_init();
  st7789_backlight_on();
  gpio_pin_set(&PIN_KEYBOARD_BACKLIGHT);

  BK4819_SelectChip(0);
  BK4819_Init();
  BK4819_RX_TurnOn();
  BK4819_SetAFC(0);
  // BK4819_SetModulation(MOD_FM);
  BK4819_SetAGC(true, 1);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_TuneTo(17230000, true);

  gpio_pin_set(&PIN_AF_MUTE);

  for (;;) {
    key_event_t evt;
    while (keyboard_get_event(&evt)) {
      UI_ClearScreen(C_BLACK);
      PrintfEx(0, 18 * 1, POS_L, C_WHITE, C_BLACK, "Code %u", evt.code);
      PrintfEx(0, 18 * 2, POS_L, C_WHITE, C_BLACK, "Name %s",
               KEY_NAMES[evt.key]);
    }

    uint16_t adc = rt880_adc_read_keyin();
    PrintfEx(0, 18 * 3, POS_L, C_WHITE, C_BLACK, "ADC: %u   ", adc);

    delay_ms(250);
  }
}
