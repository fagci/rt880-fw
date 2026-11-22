#pragma once
#include "../driver/keyboard.h"

class Mode {
public:
  virtual void enter() {}
  virtual void exit() {}
  virtual void update() {}
  virtual void render() {}
  /* Период вызова render() в главном цикле: анализатору нужен 60 Гц,
   * остальным режимам 20 Гц достаточно. */
  virtual uint16_t renderIntervalMs() { return 50; }
  virtual bool key(KEY_Code_t key, Key_State_t state) { (void)key; (void)state; return false; }
  virtual ~Mode() = default;
};

void Mode_Init(void);
void Mode_Switch(Mode &mode);
void Mode_Update(void);
void Mode_Render(void);
uint16_t Mode_RenderInterval(void);
void Mode_Key(KEY_Code_t key, Key_State_t state);
bool Mode_KeyLocked(void);

Mode *Mode_Current(void);
