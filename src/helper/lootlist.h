/* Список находок (loot): накопление частот, найденных сканером.
 *
 * Идея и раскладка записи — из k1-fw (src/inc/loot.h, src/helper/lootlist.c).
 * Запись 12 байт; 200 штук = 2.4 КБ RAM (RAM здесь в обрез: _estack 40 КБ).
 *
 * Чёрный список (blacklist) — частота никогда не останавливает сканер
 *   (мусор, гармоники, помехи).
 * Белый список (whitelist) — «известная/хорошая»: при обычном сканировании
 *   пропускается (ищем новое); в режиме мониторинга сканер останавливается
 *   только на них.
 * Записи с blacklist/whitelist не вытесняются при переполнении.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifndef LOOT_SIZE_MAX
#define LOOT_SIZE_MAX 200
#endif

#define LOOT_NO_CODE 0xFF

typedef struct {
  uint32_t f; /* частота, ед. 10 Гц */

  uint16_t lastTimeOpen; /* сек (millis()/1000), wrap каждые ~18 ч */
  uint16_t duration;     /* сек, накопительно (clamp 0xFFFF) */

  uint8_t modulation : 3; /* MOD_* (0..7) */
  uint8_t bw : 3;         /* полоса BK (0..7) */
  uint8_t radio : 2;      /* Radio (0..2) */

  uint8_t blacklist : 1;
  uint8_t whitelist : 1;
  uint8_t open : 1; /* runtime: сигнал есть сейчас */
  uint8_t _pad : 5;

  uint8_t code; /* CTCSS/DCS, LOOT_NO_CODE = нет */
  uint8_t peak; /* пик RSSI: raw >> 1 (0..255) */
} Loot;

static_assert(sizeof(Loot) == 12, "Loot must be 12 bytes");

typedef bool (*LootCmp)(const Loot *a, const Loot *b);

/* Последняя активная находка (для быстрого чёрного/белого списка) */
extern Loot *gLastActiveLoot;

uint16_t LOOT_Size(void);
Loot *LOOT_Item(uint16_t i);
Loot *LOOT_Get(uint32_t f);
int16_t LOOT_IndexOf(const Loot *item);

/* Находка есть — вернуть; нет — добавить (FIFO-вытеснение незащищённых).
 * NULL, если все слоты защищены. */
Loot *LOOT_Add(uint32_t f);
void LOOT_Remove(uint16_t i);
void LOOT_Clear(void);

/* Сигнал на частоте f зафиксирован: создаёт/обновляет запись, копит
 * длительность, фиксирует параметры приёма. Возвращает запись (или NULL). */
Loot *LOOT_Hit(uint32_t f, uint16_t rssi, uint8_t modulation, uint8_t bw,
               uint8_t radio);
/* Сигнал пропал: сбросить флаги open и остановить накопление длительности */
void LOOT_Standby(void);

/* Должен ли сканер игнорировать f (чёрный список; белый — вне мониторинга) */
bool LOOT_IsSkipped(uint32_t f, bool monitor);

void LOOT_BlacklistLast(void);
void LOOT_WhitelistLast(void);
void LOOT_ToggleBlacklist(Loot *item);
void LOOT_ToggleWhitelist(Loot *item);

uint32_t LOOT_SecondsAgo(const Loot *item);

bool LOOT_SortByLastOpenTime(const Loot *a, const Loot *b);
bool LOOT_SortByDuration(const Loot *a, const Loot *b);
bool LOOT_SortByF(const Loot *a, const Loot *b);
bool LOOT_SortByBlacklist(const Loot *a, const Loot *b);
void LOOT_Sort(LootCmp compare, bool reverse);
