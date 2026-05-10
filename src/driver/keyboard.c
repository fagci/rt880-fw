#include "keyboard.h"
#include "board.h"
#include "driver/gpio.h"

/* ── Таблица колонок и строк ────────────────────────────────────────── */
static const gpio_pin_t *const COL[4] = {
    &PIN_KEY_O1,
    &PIN_KEY_O2,
    &PIN_KEY_O3,
    &PIN_KEY_O4,
};

static const gpio_pin_t *const ROW[4] = {
    &PIN_KEY_M1,
    &PIN_KEY_M2,
    &PIN_KEY_M3,
    &PIN_KEY_M4,
};

/* ── Таблица маппинга бит → key_id_t ───────────────────────────────── */
/* Бит = col*4 + row, порядок: col0/row0..col3/row3                      */
/* PLACEHOLDER — скорректируй по схеме или экспериментально               */
static const key_id_t KEY_MAP[16] = {
    KEY_1,    // 0
    KEY_4,    // 1
    KEY_7,    // 2
    KEY_STAR, // 3
    KEY_2,    // 4
    KEY_5,    // 5
    KEY_8,    // 6
    KEY_0,    // 7
    KEY_3,    // 8
    KEY_6,    // 9
    KEY_9,    // 10
    KEY_HASH, // 11
    KEY_MENU, // 12
    KEY_UP,   // 13
    KEY_DOWN, // 14
    KEY_EXIT, // 15
};

/* ── Внутреннее состояние ──────────────────────────────────────────── */
static uint16_t key_raw; /* текущее сырое состояние (1 = нажата)   */
static uint16_t key_debounced; /* состояние после дебаунса               */
static uint8_t debounce_cnt[16];
static uint16_t hold_ticks[16]; /* счётчик удержания для long press        */
static bool long_sent[16]; /* уже отправили long press?               */

static uint8_t col_idx; /* текущая активная колонка scan           */

/* ── Очередь событий ───────────────────────────────────────────────── */
static key_event_t evt_queue[KEY_QUEUE_SIZE];
static uint8_t q_head, q_tail;

static void queue_push(key_id_t key, key_evt_type_t type, uint8_t code) {
  uint8_t next = (q_tail + 1) & (KEY_QUEUE_SIZE - 1);
  if (next == q_head)
    return; /* очередь полна — теряем событие */
  evt_queue[q_tail].key = key;
  evt_queue[q_tail].type = type;
  evt_queue[q_tail].code = code;
  q_tail = next;
}

/* ── Инициализация ──────────────────────────────────────────────────── */
void keyboard_init(void) {
  for (int i = 0; i < 4; i++) {
    gpio_pin_init_input(ROW[i], GPIO_PULL_UP);
    gpio_pin_init_output(COL[i]);
    gpio_pin_set(COL[i]); /* HIGH = неактивна */
  }

  /* Активируем первую колонку */
  gpio_pin_clr(COL[0]);
  col_idx = 0;
}

/* ── Один шаг сканирования ─────────────────────────────────────────── */
/*  Вызывать каждые KEY_SCAN_PERIOD_MS миллисекунд                        */
void keyboard_scan_tick(void) {
  /* 1. Считываем 4 строки текущей активной колонки */
  for (int row = 0; row < 4; row++) {
    int bit = col_idx * 4 + row;
    if (!gpio_pin_read(ROW[row])) { /* LOW = нажата */
      key_raw |= (1U << bit);
    } else {
      key_raw &= ~(1U << bit);
    }
  }

  /* 2. Переключаем колонку */
  gpio_pin_set(COL[col_idx]);
  col_idx = (col_idx + 1) & 3;
  gpio_pin_clr(COL[col_idx]);

  /* 3. После полного цикла (каждые 4 тика) — дебаунс и события */
  if (col_idx != 0)
    return; /* ждём полного оборота */

  for (int i = 0; i < 16; i++) {
    bool pressed = (key_raw >> i) & 1;
    bool was = (key_debounced >> i) & 1;

    if (pressed == was) {
      debounce_cnt[i] = 0;
    } else {
      if (++debounce_cnt[i] >= KEY_DEBOUNCE_TICKS) {
        debounce_cnt[i] = 0;
        if (pressed) {
          key_debounced |= (1U << i);
          hold_ticks[i] = 0;
          long_sent[i] = false;
          queue_push(KEY_MAP[i], KEY_EVT_PRESS, i);
        } else {
          key_debounced &= ~(1U << i);
          hold_ticks[i] = 0;
          queue_push(KEY_MAP[i], KEY_EVT_RELEASE, i);
        }
      }
    }

    /* Long press */
    if ((key_debounced >> i) & 1) {
      if (!long_sent[i]) {
        hold_ticks[i]++;
        if (hold_ticks[i] >= KEY_LONG_PRESS_TICKS) {
          long_sent[i] = true;
          queue_push(KEY_MAP[i], KEY_EVT_LONG_PRESS, i);
        }
      }
    }
  }
}

/* ── Получить событие из очереди ──────────────────────────────────── */
bool keyboard_get_event(key_event_t *evt) {
  if (q_head == q_tail)
    return false;
  *evt = evt_queue[q_head];
  q_head = (q_head + 1) & (KEY_QUEUE_SIZE - 1);
  return true;
}
