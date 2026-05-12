#include "mode.h"
#include <stdlib.h>

// mode_manager.c

static Mode_t *current = NULL;
static Mode_t *next = NULL; // отложенное переключение

void Mode_Switch(Mode_t *mode) {
  next = mode; // не переключаем прямо здесь — можем быть внутри update/key
}

void Mode_Update(void) {
  // переключение в безопасном месте
  if (next) {
    if (current && current->exit)
      current->exit(current);
    current = next;
    next = NULL;
    if (current->enter)
      current->enter(current);
  }
  if (current && current->update)
    current->update(current);
}

void Mode_Render(void) {
  if (current && current->render)
    current->render(current);
}

void Mode_Key(KEY_Code_t key, Key_State_t state) {
  if (current && current->key)
    current->key(current, key, state);
}
