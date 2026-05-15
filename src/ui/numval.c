#include "numval.h"
#include "../driver/board.h"
#include "../driver/st7789.h"
#include "../ui/graphics.h"
#include "gfxfont.h"
#include <stdio.h>
#include <string.h>

static const uint32_t POW10[] = {1, 10, 100, 1000, 10000, 100000, 1000000};

// ──────────────────────────────────────────────
// Вспомогательное

static uint32_t unitMultiplier(InputUnit unit) {
    switch (unit) {
    case UNIT_MHZ:
        return 100000;
    case UNIT_KHZ:
        return 100;
    case UNIT_VOLTS:
        return 100;
    default:
        return 1;
    }
}

// Количество дробных знаков из multiplier (log10)
static uint8_t fracWidth(uint32_t mul) {
    uint8_t n = 0;
    while (mul >= 10) {
        mul /= 10;
        n++;
    }
    return n;
}

// Форматирует nv->value → строку для отображения
// Для UNIT_MHZ показываем не более 3 знаков после точки
static void valueToStr(const NumVal_t *nv, char *out) {
    uint32_t v   = nv->value;
    uint32_t mul = nv->multiplier;

    if (mul <= 1) {
        sprintf(out, "%lu", (unsigned long)v);
        return;
    }

    uint32_t intPart  = v / mul;
    uint32_t fracPart = v % mul;
    uint8_t  fw       = fracWidth(mul);

    // Для МГц ограничиваем до 3 знаков после точки
    if (nv->unit == UNIT_MHZ && fw > 3) {
        uint8_t drop = fw - 3;
        uint32_t divisor = 1;
        for (uint8_t i = 0; i < drop; i++) divisor *= 10;
        fracPart /= divisor;
        fw = 3;
    }

    // убираем trailing zeros
    while (fw > 0 && fracPart % 10 == 0) {
        fracPart /= 10;
        fw--;
    }

    if (fw > 0) {
        sprintf(out, "%lu.%0*lu", (unsigned long)intPart, fw,
                (unsigned long)fracPart);
    } else {
        sprintf(out, "%lu", (unsigned long)intPart);
    }
}

// Парсит buf до cursor → нативное значение
static uint32_t bufToValue(const NumVal_t *nv) {
    uint32_t intPart = 0, fracPart = 0;
    uint8_t  localFrac = 0;
    bool     inFrac    = false;

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
    if (localFrac > 0)
        result += (fracPart * nv->multiplier) / POW10[localFrac];
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
    nv->editing    = true;
    nv->cursor     = 0;
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
    nv->editing    = false;
    nv->blinkState = 0;
}

// ──────────────────────────────────────────────
// Публичный API

void NumVal_Init(NumVal_t *nv, uint16_t x, uint16_t y, GFXfont *font,
                 uint8_t charW, uint8_t charH, InputUnit unit, uint32_t value,
                 uint32_t min, uint32_t max, NumVal_CB onChange, TextPos pos) {
    nv->x          = x;
    nv->y          = y;
    nv->font       = font;
    nv->charW      = charW;
    nv->charH      = charH;
    nv->pos        = pos;
    nv->unit       = unit;
    nv->multiplier = unitMultiplier(unit);
    nv->value      = value;
    nv->min        = min;
    nv->max        = max;
    nv->onChange   = onChange;
    nv->editing    = false;
    nv->blinkState = 0;
    nv->lastBlink  = 0;
    nv->prevLen    = 0;
    nv->dirty      = true;
    memset(nv->buf, 0, NUMVAL_BUF);
}

void NumVal_Update(NumVal_t *nv) {
    if (!nv->editing)
        return;
    if (millis() - nv->lastBlink >= 500) {
        nv->blinkState = !nv->blinkState;
        nv->lastBlink  = millis();
        gRedrawScreen  = true;
        nv->dirty      = true;
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
        valueToStr(nv, str);
    }

    uint8_t len      = strlen(str);
    uint8_t clearLen = len > nv->prevLen ? len : nv->prevLen;

    // Затираем предыдущую область с учётом выравнивания
    uint16_t clearX = renderX(nv, clearLen);
    FillRect(clearX, nv->y - nv->charH + 1,
             clearLen * nv->charW + 1, nv->charH, BG());

    if (len > 0) {
        uint16_t drawX = renderX(nv, len);
        PrintfEx(drawX, nv->y, POS_L, C_WHITE, C_BLACK, nv->font, "%s", str);
    }

    // Курсор — вертикальная черта после последнего введённого символа
    if (nv->editing && nv->blinkState) {
        uint16_t baseX = renderX(nv, nv->cursor);
        uint16_t cx    = baseX + nv->cursor * nv->charW;
        FillRect(cx, nv->y - nv->charH + 1, 1, nv->charH, C_YELLOW);
    }

    nv->prevLen = len;
}

bool NumVal_Key(NumVal_t *nv, KEY_Code_t key, Key_State_t state) {
    // Длинный EXIT — отмена редактирования
    if (state == KEY_LONG_PRESSED && key == KEY_EXIT && nv->editing) {
        stopEdit(nv, false);
        gRedrawScreen = true;
        nv->dirty     = true;
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
            uint32_t newInt    = intPart * 10 + (digit - '0');
            uint32_t maxInt    = nv->max / nv->multiplier;
            // Если новая целая часть превысила максимальную — вставляем точку
            if (newInt > maxInt && nv->cursor < NUMVAL_BUF - 2) {
                nv->buf[nv->cursor++] = '.';
                nv->buf[nv->cursor]   = '\0';
                nv->dotEntered        = true;
            }
        }

        // Теперь вставляем саму цифру (с учётом ограничения fracDigits)
        if (nv->dotEntered && nv->fracDigits >= NUMVAL_MAX_FRAC)
            break;

        nv->buf[nv->cursor++] = digit;
        nv->buf[nv->cursor]   = '\0';
        if (nv->dotEntered)
            nv->fracDigits++;

        gRedrawScreen = true;
        nv->dirty     = true;
        return true;
    }

    case KEY_STAR:
        // Вставить точку вручную (если единицы допускают дробь)
        if (nv->multiplier > 1 && !nv->dotEntered && nv->cursor > 0 &&
            nv->cursor < NUMVAL_BUF - 1) {
            nv->buf[nv->cursor++] = '.';
            nv->buf[nv->cursor]   = '\0';
            nv->dotEntered        = true;
            gRedrawScreen         = true;
            nv->dirty             = true;
        }
        return true;

    case KEY_EXIT:
        if (nv->cursor == 0) {
            stopEdit(nv, false);
            gRedrawScreen = true;
            nv->dirty     = true;
            return true;
        }
        // Backspace
        if (nv->buf[nv->cursor - 1] == '.') {
            nv->dotEntered = false;
        } else if (nv->dotEntered && nv->fracDigits > 0) {
            nv->fracDigits--;
        }
        nv->buf[--nv->cursor] = '\0';
        gRedrawScreen         = true;
        nv->dirty             = true;
        return true;

    case KEY_MENU:
    case KEY_HASH:
    case KEY_PTT:
        if (nv->editing) {
            stopEdit(nv, true);
            gRedrawScreen = true;
            nv->dirty     = true;
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
}
