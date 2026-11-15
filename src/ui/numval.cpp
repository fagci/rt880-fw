#include "numval.h"
#include "../driver/board.h"
#include "../driver/st7789.h"
#include "../ui/graphics.h"
#include "../helper/unit_fmt.h"
#include "gfxfont.h"
#include "lvgl.h"
#include <string.h>

// Парсит buf до cursor → нативное значение
static uint32_t bufToValue(const NumVal_t *nv) {
  uint32_t intPart = 0, fracPart = 0;
  uint8_t localFrac = 0;
  bool inFrac = false;

  for (uint8_t i = 0; i < nv->cursor; i++) {
    char c = nv->buf[i];
    if (c == '.') {
      inFrac = true;
    } else if (c >= '0' && c <= '9') {
      if (inFrac) {
        fracPart = fracPart * 10 + (c - '0');
        localFrac++;
      } else {
        intPart = intPart * 10 + (c - '0');
      }
    }
  }

  uint32_t result = intPart * nv->multiplier;
  if (localFrac > 0) {
    static const uint32_t POW10[] = {1, 10, 100, 1000, 10000, 100000, 1000000};
    result += (fracPart * nv->multiplier) / POW10[localFrac];
  }
  return result;
}

// Вычисляет X с учётом выравнивания для строки длиной len символов
static uint16_t renderX(const NumVal_t *nv, uint8_t len) {
  switch (nv->pos) {
  case POS_C:
    return nv->x - (len * nv->charW) / 2;
  case POS_R:
    return nv->x - len * nv->charW;
  case POS_L:
  default:
    return nv->x;
  }
}

// ──────────────────────────────────────────────
// Управление режимом редактирования

static void startEdit(NumVal_t *nv) {
  nv->editing = true;
  nv->cursor = 0;
  nv->dotEntered = false;
  nv->fracDigits = 0;
  memset(nv->buf, 0, NUMVAL_BUF);
}

static void stopEdit(NumVal_t *nv, bool confirm) {
  if (confirm) {
    uint32_t v = bufToValue(nv);
    if (v >= nv->min && v <= nv->max) {
      nv->value = v;
      if (nv->onChange)
        nv->onChange(nv->value);
    }
  }
  nv->editing = false;
  nv->blinkState = 0;
}

// ──────────────────────────────────────────────
// Публичный API

void NumVal_Init(NumVal_t *nv, uint16_t x, uint16_t y, const GFXfont *font,
                 uint8_t charW, uint8_t charH, InputUnit unit, uint32_t value,
                 uint32_t min, uint32_t max, NumVal_CB onChange, TextPos pos,
                 const GFXfont *extraFont, uint8_t extraCharW, uint8_t extraCharH) {
  nv->x = x;
  nv->y = y;
  nv->font = font;
  nv->charW = charW;
  nv->charH = charH;
  nv->pos = pos;
  nv->unit = unit;
  nv->multiplier = unit_multiplier(unit);
  nv->value = value;
  nv->min = min;
  nv->max = max;
  nv->onChange = onChange;
  nv->editing = false;
  nv->blinkState = 0;
  nv->lastBlink = 0;
  nv->prevLen = 0;
  nv->prevExtra = 255;
  nv->dirty = true;
  nv->extraPrecision = (extraFont != NULL);
  nv->extraFont = extraFont;
  nv->extraCharW = extraCharW;
  nv->extraCharH = extraCharH;
  memset(nv->buf, 0, NUMVAL_BUF);
}

void NumVal_BindNative(NumVal_t *nv, void *lv_label) {
  nv->lvLabel = lv_label;
  nv->dirty = true;
}

void NumVal_Update(NumVal_t *nv) {
  if (!nv->editing)
    return;
  if (millis() - nv->lastBlink >= 500) {
    nv->blinkState = !nv->blinkState;
    nv->lastBlink = millis();

    nv->dirty = true;
  }
}

void NumVal_Render(NumVal_t *nv) {
  if (!nv->dirty)
    return;
  nv->dirty = false;

  char str[NUMVAL_BUF] = "";

  if (nv->editing) {
    if (nv->buf[0])
      strcpy(str, nv->buf);
  } else {
    fmt_value(str, NUMVAL_BUF, nv->value, nv->multiplier, nv->unit);
    (void)str;
  }

  /* Нативный путь: только обновляем текст лейбла. Выравнивание и
     затирание — внутри LVGL (фиксированная ширина + text align). */
  if (nv->lvLabel) {
    lv_label_set_text(static_cast<lv_obj_t *>(nv->lvLabel), str);
    return;
  }

  uint8_t len = strlen(str);

  /* Печатаем от фиксированного правого края (nv->x, POS_R): при вводе
     лейбл двигается, а LVGL сам затирает освободившееся место.
     Пустой буфер — пробелом: ужимание лейбла инвалидирует старый bbox. */
  PrintfEx(nv->x, nv->y, POS_R, C_WHITE, C_BLACK, nv->font, "%s",
           len ? str : " ");

  /* Курсор — вертикальная черта после последнего введённого символа.
     Позиция считается от того же правого края (исходный код рисовал
     его всегда у края — baseX + cursor*charW сокращалось до nv->x). */
  if (nv->editing) {
    uint16_t cx = (nv->pos == POS_R) ? nv->x - nv->cursor * nv->charW
                                     : nv->x + nv->cursor * nv->charW;
    FillRect(cx, nv->y - nv->charH + 1, 1, nv->charH,
             nv->blinkState ? C_YELLOW : BG());
  }

  nv->prevLen = len;

  /* ── Доп. точность: 2 маленьких знака справа ── */
  if (nv->extraPrecision && !nv->editing && nv->extraFont) {
    /* value в 10 Гц. fracPart = value % 100000 (в единицах 0.00001 МГц).
     * Первые 3 знака: fracPart / 100, доп. 2 знака: fracPart % 100 */
    uint8_t extra = (nv->value % 100000) % 100;
    PrintfEx(nv->x + 2, nv->y, POS_L, C_GRAY, BG(), nv->extraFont, "%02d",
             extra);
  }
}

bool NumVal_Key(NumVal_t *nv, KEY_Code_t key, Key_State_t state) {
  // Длинный EXIT — отмена редактирования
  if (state == KEY_LONG_PRESSED && key == KEY_EXIT && nv->editing) {
    stopEdit(nv, false);

    nv->dirty = true;
    return true;
  }

  if (state != KEY_RELEASED)
    return false;

  // Не в режиме редактирования — цифра открывает редактор
  if (!nv->editing) {
    if (key >= KEY_0 && key <= KEY_9) {
      startEdit(nv);
    } else {
      return false;
    }
  }

  switch (key) {
  case KEY_0:
  case KEY_1:
  case KEY_2:
  case KEY_3:
  case KEY_4:
  case KEY_5:
  case KEY_6:
  case KEY_7:
  case KEY_8:
  case KEY_9: {
    if (nv->cursor >= NUMVAL_BUF - 1)
      break;
    if (nv->dotEntered && nv->fracDigits >= NUMVAL_MAX_FRAC)
      break;

    char digit = '0' + (key - KEY_0);

    // Авто-вставка точки: если ещё не вводили дробь и
    // новая цифра заставит целую часть превысить max/multiplier
    if (!nv->dotEntered && nv->multiplier > 1) {
      // Считаем текущую целую часть из buf
      uint32_t intPart = 0;
      for (uint8_t i = 0; i < nv->cursor; i++) {
        if (nv->buf[i] >= '0' && nv->buf[i] <= '9')
          intPart = intPart * 10 + (nv->buf[i] - '0');
      }
      uint32_t newInt = intPart * 10 + (digit - '0');
      uint32_t maxInt = nv->max / nv->multiplier;
      // Если новая целая часть превысила максимальную — вставляем точку
      if (newInt > maxInt && nv->cursor < NUMVAL_BUF - 2) {
        nv->buf[nv->cursor++] = '.';
        nv->buf[nv->cursor] = '\0';
        nv->dotEntered = true;
      }
    }

    // Теперь вставляем саму цифру (с учётом ограничения fracDigits)
    if (nv->dotEntered && nv->fracDigits >= NUMVAL_MAX_FRAC)
      break;

    nv->buf[nv->cursor++] = digit;
    nv->buf[nv->cursor] = '\0';
    if (nv->dotEntered)
      nv->fracDigits++;


    nv->dirty = true;
    return true;
  }

  case KEY_STAR:
    // Вставить точку вручную (если единицы допускают дробь)
    if (nv->multiplier > 1 && !nv->dotEntered && nv->cursor > 0 &&
        nv->cursor < NUMVAL_BUF - 1) {
      nv->buf[nv->cursor++] = '.';
      nv->buf[nv->cursor] = '\0';
      nv->dotEntered = true;
  
      nv->dirty = true;
    }
    return true;

  case KEY_EXIT:
    if (nv->cursor == 0) {
      stopEdit(nv, false);
  
      nv->dirty = true;
      return true;
    }
    // Backspace
    if (nv->buf[nv->cursor - 1] == '.') {
      nv->dotEntered = false;
    } else if (nv->dotEntered && nv->fracDigits > 0) {
      nv->fracDigits--;
    }
    nv->buf[--nv->cursor] = '\0';

    nv->dirty = true;
    return true;

  case KEY_MENU:
  case KEY_HASH:
  case KEY_PTT:
    if (nv->editing) {
      stopEdit(nv, true);
  
      nv->dirty = true;
      return true;
    }
    break;

  default:
    break;
  }

  return false;
}

bool NumVal_IsEditing(const NumVal_t *nv) { return nv->editing; }

void NumVal_Invalidate(NumVal_t *nv) { nv->dirty = true; }

uint32_t NumVal_GetValue(NumVal_t *nv) { return nv->value; }

void NumVal_SetValue(NumVal_t *nv, uint32_t val) {
  nv->value = val;
  nv->dirty = true;
  nv->prevExtra = 255;
}
