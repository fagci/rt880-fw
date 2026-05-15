#pragma once
#include "../driver/keyboard.h"
#include "../ui/graphics.h"
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

  // выравнивание текста
  TextPos pos;

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
  uint8_t prevExtra; /* предыдущие 2 знака для сравнения */

  bool dirty;

  /* Доп. точность: 2 маленьких знака справа */
  bool extraPrecision;
  GFXfont *extraFont;
  uint8_t extraCharW, extraCharH;
} NumVal_t;

// multiplier и fracDigits выводятся из unit
// pos — выравнивание: POS_L, POS_C, POS_R
// extraFont/extraCharW/extraCharH — NULL/0 если доп. знаки не нужны
void NumVal_Init(NumVal_t *nv, uint16_t x, uint16_t y, GFXfont *font,
                 uint8_t charW, uint8_t charH, InputUnit unit, uint32_t value,
                 uint32_t min, uint32_t max, NumVal_CB onChange, TextPos pos,
                 GFXfont *extraFont, uint8_t extraCharW, uint8_t extraCharH);

// вызывать периодически — управляет морганием курсора
void NumVal_Update(NumVal_t *nv);

// рисует только свою область
void NumVal_Render(NumVal_t *nv);

// возвращает true если событие поглощено
bool NumVal_Key(NumVal_t *nv, KEY_Code_t key, Key_State_t state);

bool NumVal_IsEditing(const NumVal_t *nv);

void NumVal_Invalidate(NumVal_t *nv);

uint32_t NumVal_GetValue(NumVal_t *nv);

void NumVal_SetValue(NumVal_t *nv, uint32_t val);
