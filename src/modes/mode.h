#pragma once
#include "../driver/keyboard.h"

class Mode {
public:
  virtual void enter() {}
  virtual void exit() {}
  virtual void update() {}
  virtual void render() {}
  virtual bool key(KEY_Code_t key, Key_State_t state) { (void)key; (void)state; return false; }
  virtual ~Mode() = default;
};

void Mode_Init(void);
void Mode_Switch(Mode &mode);
void Mode_Update(void);
void Mode_Render(void);
void Mode_Key(KEY_Code_t key, Key_State_t state);

Mode *Mode_Current(void);
