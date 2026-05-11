#pragma once
#include "../driver/keyboard.h"
#include "finput.h" // InputUnit
#include "gfxfont.h"
#include <stdbool.h>
#include <stdint.h>

#define NUMVAL_BUF 13
#define NUMVAL_MAX_FRAC 6

typedef void (*NumVal_CB)(uint32_t value);

typedef struct {
  // положение и шрифт
  uint16_t x, y;
  GFXfont *font;
  uint8_t charW, charH; // пиксельный размер одного символа

  // диапазон и единицы (используются при валидации и форматировании)
  uint32_t value;
  uint32_t min, max;
  InputUnit unit;
  uint32_t multiplier; // нативных единиц на единицу отображения

  // коллбек при подтверждении
  NumVal_CB onChange;

  // состояние редактирования
  bool editing;
  char buf[NUMVAL_BUF];
  uint8_t cursor;
  bool dotEntered;
  uint8_t fracDigits;
  uint8_t blinkState;
  uint32_t lastBlink;

  // для корректного затирания предыдущей отрисовки
  uint8_t prevLen;

  bool dirty;
} NumVal_t;

// multiplier и fracDigits выводятся из unit
void NumVal_Init(NumVal_t *nv, uint16_t x, uint16_t y, GFXfont *font,
                 uint8_t charW, uint8_t charH, InputUnit unit, uint32_t value,
                 uint32_t min, uint32_t max, NumVal_CB onChange);

// вызывать периодически — управляет морганием курсора
void NumVal_Update(NumVal_t *nv);

// рисует только свою область
void NumVal_Render(NumVal_t *nv);

// возвращает true если событие поглощено
bool NumVal_Key(NumVal_t *nv, KEY_Code_t key, Key_State_t state);

bool NumVal_IsEditing(const NumVal_t *nv);

void NumVal_Invalidate(NumVal_t *nv);
