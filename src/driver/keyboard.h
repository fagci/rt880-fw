#ifndef RT880_KEYBOARD_H
#define RT880_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

/* ── Настройки ──────────────────────────────────────────────────────── */
#define KEY_SCAN_PERIOD_MS 5 /* вызов keyboard_scan_tick() каждые N мс   */
#define KEY_DEBOUNCE_TICKS 4 /* 4 × 5 мс = 20 мс стабильного состояния  */
#define KEY_LONG_PRESS_TICKS 60 /* 60 × 5 мс = 300 мс → long press          */

#define KEY_QUEUE_SIZE 8

/* ── Коды клавиш ──────────────────────────────────────────────────────────
 * Матрица 4×4: O1..O4 — колонки (выходы, LOW = активна)
 *               M1..M4 — строки  (входы с pull-up, LOW = нажата)
 *
 * Маппинг — PLACEHOLDER, уточни по схеме или экспериментально:
 *
 *         O1      O2      O3      O4
 * M1   MENU     UP     DOWN   EXIT
 * M2     1       2       3    STAR
 * M3     4       5       6    HASH
 * M4     7       8       9      0
 *
 * Биты KeyPressed: col*4 + row  (0..15)
 * ──────────────────────────────────────────────────────────────────────── */
typedef enum {
  KEY_NONE = 0,
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
  KEY_COUNT
} key_id_t;

typedef enum {
  KEY_EVT_PRESS,
  KEY_EVT_LONG_PRESS,
  KEY_EVT_RELEASE,
} key_evt_type_t;

typedef struct {
  uint8_t code;
  key_id_t key;
  key_evt_type_t type;
} key_event_t;

/* ── Публичный API ───────────────────────────────────────────────────── */
void keyboard_init(void);

/* Вызывать из SysTick (1 мс) или таймера каждые KEY_SCAN_PERIOD_MS */
void keyboard_scan_tick(void);

/* Получить следующее событие из очереди; возвращает false если очередь пуста */
bool keyboard_get_event(key_event_t *evt);

#endif /* RT880_KEYBOARD_H */
