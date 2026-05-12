#pragma once
#include "../driver/keyboard.h"

typedef struct Mode_t {
  void (*enter)(struct Mode_t *self);
  void (*exit)(struct Mode_t *self);
  void (*update)(struct Mode_t *self);
  void (*render)(struct Mode_t *self);
  bool (*key)(struct Mode_t *self, KEY_Code_t key, Key_State_t state);
  void *ctx; // данные конкретного режима
} Mode_t;

void Mode_Init(void);
void Mode_Switch(Mode_t *mode);
void Mode_Update(void);
void Mode_Render(void);
void Mode_Key(KEY_Code_t key, Key_State_t state);

Mode_t *Mode_Current(void);
