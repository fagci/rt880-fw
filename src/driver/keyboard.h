
#ifndef RT880_KEYBOARD_H
#define RT880_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

/* ── Настройки ──────────────────────────────────────────────────────── */
#define KEY_SCAN_PERIOD_MS 5
#define KEY_DEBOUNCE_TICKS 4    /* 4 × 5 мс = 20 мс  */
#define KEY_LONG_PRESS_TICKS 60 /* 60 × 5 мс = 300 мс */
#define KEY_QUEUE_SIZE 8

typedef enum {
  KEY_NONE = 0,
  /* матрица 4×4 */
  KEY_1,
  KEY_2,
  KEY_3,
  KEY_4,
  KEY_5,
  KEY_6,
  KEY_7,
  KEY_8,
  KEY_9,
  KEY_0,
  KEY_MENU,
  KEY_UP,
  KEY_DOWN,
  KEY_EXIT,
  KEY_STAR,
  KEY_HASH,
  /* прямые GPIO */
  KEY_PTT,   /* PB6  */
  KEY_SIDE1, /* PC0  */
  KEY_SIDE2, /* PC1  */
  KEY_ALARM, /* PC2  */
  KEY_COUNT
} key_id_t;

typedef enum {
  KEY_EVT_PRESS,
  KEY_EVT_LONG_PRESS,
  KEY_EVT_RELEASE,

  KEY_PRESSED = KEY_EVT_PRESS,
  KEY_RELEASED = KEY_EVT_RELEASE,
  KEY_LONG_PRESSED = KEY_EVT_LONG_PRESS,
  KEY_LONG_PRESSED_CONT = 4,
} key_evt_type_t;

typedef struct {
  uint8_t code; /* сырой индекс для отладки */
  key_id_t key;
  key_evt_type_t type;
} key_event_t;

typedef key_id_t KEY_Code_t;
typedef key_evt_type_t Key_State_t;

void keyboard_init(void);
void keyboard_scan_tick(void);
bool keyboard_get_event(key_event_t *evt);

#endif /* RT880_KEYBOARD_H */
