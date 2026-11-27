#ifndef TONE_H
#define TONE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Детектор субтонов и DTMF для активного BK-приёмника.
 * CTCSS/DCS читаются сканом REG_68/69/6A (неблокирующе, ~0.6 с),
 * DTMF — по прерыванию REG_02. Результат живёт до смены частоты/режима. */

/* Компактный код субтона для Loot.code/CH.tone: 0xFF — нет,
 * 0..49 — индекс CTCSS_OPTIONS, 0x80|idx — индекс DCS_OPTIONS. */
#define TONE_CODE_NONE 0xFFu
#define TONE_CODE_DCS 0x80u

void Tone_Reset(void);
/* enabled=false — активен SI/нет приёма; chip — 0/1, freq — единицы 10 Гц. */
void Tone_Update(bool enabled, uint8_t chip, uint32_t freq, bool squelchOpen);
/* "CT 88.5", "DCS 023", "DTMF 1234" или "" — для строки VFO. */
void Tone_GetStatus(char *buf, size_t n);
/* Последний найденный CTCSS/DCS (код как выше); DTMF не учитывается. */
uint8_t Tone_GetCode(void);
/* "CT 88.5", "DCS 023" или "" — для списков/формы канала. */
void Tone_CodeString(uint8_t code, char *buf, size_t n);

#endif
