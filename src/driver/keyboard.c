#include "keyboard.h"
#include "board.h"
#include "driver/gpio.h"

/* ── Таблица матрицы ────────────────────────────────────────────────── */
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

/* ── Таблица прямых кнопок ──────────────────────────────────────────── */
/* Индексы 16..19 — продолжение единого пространства кодов */
#define DIRECT_BASE 16
#define DIRECT_COUNT 1
#define ADC_BASE DIRECT_BASE + DIRECT_COUNT
#define ADC_COUNT 3

typedef struct {
  const gpio_pin_t *pin;
  key_id_t key;
} direct_key_def_t;

static const direct_key_def_t DIRECT[DIRECT_COUNT] = {
    {&PIN_KEY_PTT, KEY_PTT},     /* PB6, idx=16 */
    {&PIN_KEY_SIDE1, KEY_SIDE1}, /* PC0, idx=17 */
    {&PIN_KEY_SIDE2, KEY_SIDE2}, /* PC1, idx=18 */
    {&PIN_KEY_ALARM, KEY_ALARM}, /* PC2, idx=19 */
};

/* ── Общее состояние (матрица + прямые как единый массив) ───────────── */
#define TOTAL_KEYS (16 + DIRECT_COUNT)

static uint8_t debounce_cnt[TOTAL_KEYS];
static uint16_t hold_ticks[TOTAL_KEYS];
static bool long_sent[TOTAL_KEYS];
static bool key_debounced[TOTAL_KEYS];

static uint16_t key_raw_matrix;
static bool key_raw_direct[DIRECT_COUNT];

static uint8_t col_idx;

/* ── Очередь событий ───────────────────────────────────────────────── */
static key_event_t evt_queue[KEY_QUEUE_SIZE];
static uint8_t q_head, q_tail;

static void queue_push(key_id_t key, key_evt_type_t type, uint8_t code) {
  uint8_t next = (q_tail + 1) & (KEY_QUEUE_SIZE - 1);
  if (next == q_head)
    return;
  evt_queue[q_tail].key = key;
  evt_queue[q_tail].type = type;
  evt_queue[q_tail].code = code;
  q_tail = next;
}

/* ── Общий обработчик дебаунса и событий ───────────────────────────── */
static void process_key(uint8_t idx, key_id_t key_id, bool pressed) {
  bool was = key_debounced[idx];

  if (pressed == was) {
    debounce_cnt[idx] = 0;
  } else {
    if (++debounce_cnt[idx] >= KEY_DEBOUNCE_TICKS) {
      debounce_cnt[idx] = 0;
      key_debounced[idx] = pressed;
      if (pressed) {
        hold_ticks[idx] = 0;
        long_sent[idx] = false;
        queue_push(key_id, KEY_EVT_PRESS, idx);
      } else {
        hold_ticks[idx] = 0;
        queue_push(key_id, KEY_EVT_RELEASE, idx);
      }
    }
  }

  if (key_debounced[idx] && !long_sent[idx]) {
    if (++hold_ticks[idx] >= KEY_LONG_PRESS_TICKS) {
      long_sent[idx] = true;
      queue_push(key_id, KEY_EVT_LONG_PRESS, idx);
    }
  }
}

/* ── Инициализация ──────────────────────────────────────────────────── */
void keyboard_init(void) {
  /* матрица */
  for (int i = 0; i < 4; i++) {
    gpio_pin_init_input(ROW[i], GPIO_PULL_UP);
    gpio_pin_init_output(COL[i]);
    gpio_pin_set(COL[i]);
  }
  gpio_pin_clr(COL[0]);
  col_idx = 0;

  /* прямые кнопки — все входы с pull-up */
  for (int i = 0; i < DIRECT_COUNT; i++)
    gpio_pin_init_input(DIRECT[i].pin, GPIO_PULL_UP);
}

/* ── Сканирование (вызывать каждые KEY_SCAN_PERIOD_MS мс) ───────────── */
void keyboard_scan_tick(void) {
  /* 1. Считываем строки текущей колонки матрицы */
  for (int row = 0; row < 4; row++) {
    int bit = col_idx * 4 + row;
    if (!gpio_pin_read(ROW[row]))
      key_raw_matrix |= (1U << bit);
    else
      key_raw_matrix &= ~(1U << bit);
  }

  /* 2. Переключаем колонку */
  gpio_pin_set(COL[col_idx]);
  col_idx = (col_idx + 1) & 3;
  gpio_pin_clr(COL[col_idx]);

  /* 3. Ждём полного оборота матрицы */
  if (col_idx != 0)
    return;

  /* 4. Дебаунс матрицы */
  for (int i = 0; i < 16; i++)
    process_key(i, KEY_MAP[i], (key_raw_matrix >> i) & 1);

  uint16_t adc = rt880_adc_read_keyin();

  typedef struct {
    uint16_t lo;
    uint16_t hi;
    key_id_t key;
  } adc_key_t;
  static const adc_key_t ADC_KEYS[] = {
      {1300, 2000, KEY_SIDE2},
      {2000, 2850, KEY_SIDE1},
      {2850, 3100, KEY_ALARM},
  };
  for (int i = 0; i < ADC_COUNT; i++) {
    key_raw_direct[i] = ADC_KEYS[i].lo <= adc && adc <= ADC_KEYS[i].hi;
    process_key(ADC_BASE + i, ADC_KEYS[i].key, key_raw_direct[i]);
  }

  /* 5. Прямые кнопки — читаем и обрабатываем тут же */
  for (int i = 0; i < DIRECT_COUNT; i++) {
    key_raw_direct[i] = !gpio_pin_read(DIRECT[i].pin); /* LOW = нажата */
    process_key(DIRECT_BASE + i, DIRECT[i].key, key_raw_direct[i]);
  }
}

/* ── Получить событие ───────────────────────────────────────────────── */
bool keyboard_get_event(key_event_t *evt) {
  if (q_head == q_tail)
    return false;
  *evt = evt_queue[q_head];
  q_head = (q_head + 1) & (KEY_QUEUE_SIZE - 1);
  return true;
}
