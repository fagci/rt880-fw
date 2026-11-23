#ifndef RT880_STORE_H
#define RT880_STORE_H

#include <stdbool.h>

#include "channels.h"

/* Энергонезависимое состояние в конце внешней W25Q32.
 *
 * Стоковые данные (каналы, голосовые подсказки) занимают начало флеш,
 * поэтому под настройки отведён хвост 0x3E0000..0x3FFFFF: 32 сектора,
 * запись по кольцу. Каждая запись защищена CRC; при обрыве питания
 * загружается предыдущая целая. Слоты 28..31 отданы кольцу каналов,
 * состояние пишется только в 0..27. */

void Store_Init(void);
bool Store_HasState(void);

/* Пометить состояние изменённым: запись уйдёт после паузы без изменений. */
void Store_MarkDirty(void);
void Store_Poll(void);

/* Применить сохранённое: UI (тема, антенна) — до первого Mode_Switch,
 * настройки VFO/диапазонов — из Radio_Init после дефолтов. */
bool Store_ApplyUi(void);
bool Store_ApplyRadio(void);

/* Диагностика для UART-консоли. */
void Store_DebugPrint(void);
/* Стереть сохранённое состояние и каналы (начало флеш не трогается). */
bool Store_Wipe(void);

/* ── Кольцо каналов (слоты 28..31) ──────────────────────────────────
 * Одна запись — один сектор: заголовок 20 байт, данные с offset 256,
 * count * sizeof(CH). Чтение и запись потоковые: массива каналов в RAM
 * нет. Заголовок пишется последним, поэтому обрыв питания оставляет
 * предыдущий целый снимок. */
bool Store_ChannelsLoad(uint16_t *count, uint16_t *scanlist);
bool Store_ChannelsRead(uint16_t idx, CH *ch);
bool Store_ChannelsWriteBegin(void);
bool Store_ChannelsWritePut(const CH *ch);
bool Store_ChannelsWriteCommit(uint16_t count, uint16_t scanlist);
void Store_ChannelsWriteAbort(void);

#endif /* RT880_STORE_H */
