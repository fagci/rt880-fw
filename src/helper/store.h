#ifndef RT880_STORE_H
#define RT880_STORE_H

#include <stdbool.h>

#include "bands.h"
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

/* Пока сканирующий режим временно гоняет VFO по частотам, в запись должно
 * попадать состояние на входе, а не промежуточная частота скана. Pin
 * запоминает текущие VFO/currentVfo/фильтр, Unpin — снова пишет живые. */
void Store_PinRadio(void);
void Store_UnpinRadio(void);

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
 * count * sizeof(CH). Версия 1 (8 байт, без имени) и версия 2 (16 байт)
 * читаются; запись всегда v2. Массива каналов в RAM нет, чтение и запись
 * потоковые. Заголовок пишется последним, поэтому обрыв питания оставляет
 * предыдущий целый снимок. */
bool Store_ChannelsLoad(uint16_t *count, uint16_t *scanlist);
bool Store_ChannelsRead(uint16_t idx, CH *ch);
bool Store_ChannelsWriteBegin(void);
bool Store_ChannelsWritePut(const CH *ch);
bool Store_ChannelsWriteCommit(uint16_t count, uint16_t scanlist);
void Store_ChannelsWriteAbort(void);

/* ── Кольцо диапазонов (слоты 24..27) ──────────────────────────────
 * Снимок пользовательского плана диапазонов, как у каналов: заголовок
 * 20 байт, данные с offset 256, count * sizeof(BandRec). Заголовок
 * пишется последним. */
bool Store_BandsLoad(uint16_t *count);
bool Store_BandsRead(uint16_t idx, BandRec *out);
bool Store_BandsWriteBegin(void);
bool Store_BandsWritePut(const BandRec *rec);
bool Store_BandsWriteCommit(uint16_t count);
void Store_BandsWriteAbort(void);

#endif /* RT880_STORE_H */
