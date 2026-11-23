#include "at32f423.h"
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "helper/bootprof.h"
#include "helper/channels.h"
#include "helper/console.h"
#include "helper/store.h"
#include "modes/mode.h"
#include "modes/modes.h"
#include "ui/graphics.h"
#include "ui/lv_port.h"
#include "lvgl.h"

/* Статические конструкторы: .init_array заполняют crtbegin и компилятор.
 * __libc_init_array звать нельзя — в этом линке _init/_fini без crtn. */
typedef void (*InitFn)(void);
extern "C" InitFn __init_array_start[];
extern "C" InitFn __init_array_end[];

extern "C" void RunInitArray(void) {
  for (InitFn *p = __init_array_start; p < __init_array_end; ++p)
    (*p)();
}

int main(void) {
  Prof_Init();
  board_init();

  /* Внешняя W25Q32: состояние и список находок переживают выключение. */
  Store_Init();
  Store_ApplyUi();
  CH_Init();

  gpio_pin_clr(&PIN_LED_RED);   // past board_init
  gpio_pin_set(&PIN_LED_GREEN);

  lv_port_init();
  Prof_Mark(PROF_LV);

  gpio_pin_clr(&PIN_LED_GREEN); // past lv_port_init
  gpio_pin_set(&PIN_LED_RED);

  Mode_Switch(MODE_VFO);
  Prof_Mark(PROF_SWITCH);

  gpio_pin_clr(&PIN_LED_RED);   // past Mode_Switch
  led_rx_set(false);            // зелёный — теперь индикатор открытого шумодава

  uint32_t lastRender = 0;
  bool firstLoop = true;

  for (;;) {
    Console_Poll();
    Store_Poll();
    CH_Poll();

    key_event_t evt;
    while (keyboard_get_event(&evt))
      Mode_Key(evt.key, evt.type);

    Mode_Update();
    if (firstLoop)
      Prof_Mark(PROF_UPDATE);

    uint32_t now = millis();
    if (firstLoop || now - lastRender >= Mode_RenderInterval()) {
      Mode_Render();
      lastRender = now;
      if (firstLoop)
        Prof_Mark(PROF_DRAW);
    }

    /* Рендер LVGL: сам решает, есть ли что перерисовывать.
       Возвращает время до следующего таймера — можно спать. */
    uint32_t idle = lv_timer_handler();
    if (firstLoop) {
      Prof_Mark(PROF_READY);
      Prof_Print();
      firstLoop = false;
    }
    if (idle > 1)
      __WFI();
  }
}
