#ifndef TONE_H
#define TONE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Детектор субтонов и DTMF для активного BK-приёмника.
 * CTCSS/DCS читаются сканом REG_68/69/6A (неблокирующе, ~0.6 с),
 * DTMF — по прерыванию REG_02. Результат живёт до смены частоты/режима. */

void Tone_Reset(void);
/* enabled=false — активен SI/нет приёма; chip — 0/1, freq — единицы 10 Гц. */
void Tone_Update(bool enabled, uint8_t chip, uint32_t freq, bool squelchOpen);
/* "CT 88.5", "DCS 023", "DTMF 1234" или "" — для строки VFO. */
void Tone_GetStatus(char *buf, size_t n);

#endif
