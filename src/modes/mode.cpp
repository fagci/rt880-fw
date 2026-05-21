#include "mode.h"
#include "../driver/st7789.h"

static Mode *current = nullptr;
static Mode *next = nullptr;

void Mode_Switch(Mode &mode) {
  next = &mode;
}

void Mode_Update() {
  if (next) {
    if (current)
      current->exit();
    current = next;
    next = nullptr;
    if (current)
      current->enter();
    st7789_scroll_reset();
  }
  if (current)
    current->update();
}

void Mode_Render() {
  if (current)
    current->render();
}

void Mode_Key(KEY_Code_t key, Key_State_t state) {
  if (current)
    current->key(key, state);
}

Mode *Mode_Current() {
  return current;
}
