#include "mode.h"
#include "../driver/st7789.h"
#include "../ui/spectrum.h"

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
    /* Аппаратный скролл сбрасывается при каждой смене режима — вместе с
     * ним сбрасываем и программное состояние водопада (wfScrollInited).
     * Иначе второй вход в режим с водопадом пропускает инициализацию
     * области скролла, а WF_Render продолжает слать смещение — и едет
     * весь экран. */
    WF_Reset();
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
