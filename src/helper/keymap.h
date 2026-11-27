#ifndef RT880_KEYMAP_H
#define RT880_KEYMAP_H

#include "../driver/keyboard.h"
#include <stdbool.h>
#include <stdint.h>

/* Назначения кнопок VFO. Как и в k1-fw, действие хранится отдельно для
 * короткого и длинного нажатия; по умолчанию заполнены клавиши приложений
 * RT-880, а long 1/2/3 освобождены (multiwatch теперь в меню). */
typedef enum {
  KA_NONE = 0, /* клавиша поглощается, действий нет */
  KA_BAND,
  KA_FC,
  KA_ANALYSER,
  KA_BC_SCANNER,
  KA_CHANNELS,
  KA_BANDS,
  KA_SCANNER,
  KA_SETTINGS,
  KA_LOOTLIST,
  KA_SAVE_LOOT,
  KA_CLEAR_LOOT,

  KA_NEXT_VFO,
  KA_VFO_MODE,
  KA_GROUP_NEXT,
  KA_FILTER_NEXT,
  KA_RADIO_NEXT,
  KA_BAND_UP,
  KA_BAND_DOWN,

  KA_STEP_UP,
  KA_STEP_DOWN,
  KA_BW_UP,
  KA_BW_DOWN,
  KA_MOD_UP,
  KA_MOD_DOWN,
  KA_SQL_UP,
  KA_SQL_DOWN,
  KA_GAIN_UP,
  KA_GAIN_DOWN,
  KA_AFC_UP,
  KA_AFC_DOWN,
  KA_SQUELCH_FORCE,

  KA_THEME,
  KA_ANTENNA,
  KA_KEYLOCK,
  KA_LEVEL_VFO,
  KA_ALWAYS_RSSI,
  KA_BRIGHT_UP,
  KA_BRIGHT_DOWN,
  KA_MULTIWATCH,

  KA_COUNT
} KeyAction;

/* bound = 0: клавиша не назначена, работает штатный обработчик режима.
 * bound = 1 и action = KA_NONE: клавиша выключена. */
typedef struct __attribute__((packed)) {
  uint8_t action;
  uint8_t bound;
} KeyBinding;

/* [key][0] — короткое нажатие, [key][1] — длинное. */
extern KeyBinding gKeymap[KEY_COUNT][2];

struct MenuItem;

void Keymap_LoadDefaults(void);
const char *Keymap_ActionName(KeyAction a);
/* Список действий для редактора (в flash; [0] — Default). */
const MenuItem *Keymap_ActionItems(void);
uint16_t Keymap_ActionItemCount(void);
/* true — клавиша назначена и обработана (включая выключенную KA_NONE). */
bool Keymap_Handle(KEY_Code_t key, Key_State_t state);

#endif /* RT880_KEYMAP_H */
