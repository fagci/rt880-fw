#ifndef CHANNELS_H
#define CHANNELS_H

#include <stdbool.h>
#include <stdint.h>

/* Сохранённые каналы и сканлисты.
 *
 * Список живёт не в RAM, а в своём кольце W25Q32 (см. store.h): RAM здесь
 * в обрез, поэтому чтение и перезапись идут потоком по индексу. В RAM —
 * только счётчик, текущая маска сканлистов и флаг отложенной записи.
 *
 * Канал: частота (ед. 10 Гц), маска сканлистов, модуляция, полоса,
 * приёмник и флаг blacklist. 8 байт — выровнено под потоковую запись.
 * scanlists == 0 — канал во всех списках; иначе биты 0..7. Маска
 * сканлиста 0 (CH_SCANLIST_ALL) — все каналы. */

#define CH_MAX 200
#define CH_SCANLIST_COUNT 8
#define CH_SCANLIST_ALL 0u

typedef struct __attribute__((packed)) {
  uint32_t f;             /* частота, ед. 10 Гц */
  uint16_t scanlists;     /* 0 = все списки; иначе биты 0..7 */
  uint8_t modulation : 3; /* MOD_* */
  uint8_t bw : 3;
  uint8_t radio : 2; /* Radio */
  uint8_t blacklist : 1;
  uint8_t reserved : 7;
} CH;

static_assert(sizeof(CH) == 8, "CH must be 8 bytes");

void CH_Init(void);
/* Сбросить RAM-состояние после Store_Wipe (каналы стёрты). */
void CH_Reset(void);
/* Отложенная запись (смена маски) через 2,5 с без изменений. */
void CH_Poll(void);
void CH_MarkDirty(void);

uint16_t CH_Count(void);
uint16_t CH_GetScanlistMask(void);
void CH_SetScanlistMask(uint16_t mask);

bool CH_Read(uint16_t storageIdx, CH *out);
/* Индексы хранения: сначала обычные каналы по маске, затем blacklist.
 * Возвращает количество (≤ max). */
uint16_t CH_BuildList(uint16_t mask, uint8_t *out, uint16_t max);

/* Loot → каналы. Возвращают число сохранённых записей или -1 при ошибке.
 * Одна перезапись кольца на действие. */
int16_t CH_SaveWhitelistFromLoot(void);
int16_t CH_SaveBlacklistFromLoot(void);
/* Чёрный список каналов → loot (только вручную). */
int16_t CH_LoadBlacklistToLoot(void);

/* Добавить канал (по частоте): существующий обновляется, новый дописывается
 * в конец. Возвращает false при ошибке/переполнении. */
bool CH_Add(const CH *ch);

bool CH_Delete(uint16_t storageIdx);
bool CH_ToggleBlacklist(uint16_t storageIdx);

#endif /* CHANNELS_H */
