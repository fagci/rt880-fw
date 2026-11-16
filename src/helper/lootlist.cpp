#include "lootlist.h"
#include "../driver/board.h"

static Loot loot[LOOT_SIZE_MAX];
static uint16_t lootCount;

Loot *gLastActiveLoot;
static uint32_t lastActiveF; /* восстановление указателя после сдвига/сортировки */

/* накопление длительности: кому и сколько миллисекунд «недосчитано» */
static const Loot *durOwner;
static uint16_t durFracMs;
static uint32_t lastTickMs;

static void relinkLast(void) {
  gLastActiveLoot = lastActiveF ? LOOT_Get(lastActiveF) : nullptr;
}

uint16_t LOOT_Size(void) { return lootCount; }

Loot *LOOT_Item(uint16_t i) { return i < lootCount ? &loot[i] : nullptr; }

Loot *LOOT_Get(uint32_t f) {
  for (uint16_t i = 0; i < lootCount; ++i)
    if (loot[i].f == f)
      return &loot[i];
  return nullptr;
}

int16_t LOOT_IndexOf(const Loot *item) {
  if (item < loot || item >= loot + lootCount)
    return -1;
  return (int16_t)(item - loot);
}

void LOOT_Remove(uint16_t i) {
  if (i >= lootCount)
    return;
  if (durOwner == &loot[i])
    durOwner = nullptr;
  for (uint16_t j = i; j + 1 < lootCount; ++j)
    loot[j] = loot[j + 1];
  lootCount--;
  /* сдвиг массива инвалидировал указатели */
  durOwner = nullptr;
  relinkLast();
}

void LOOT_Clear(void) {
  lootCount = 0;
  gLastActiveLoot = nullptr;
  lastActiveF = 0;
  durOwner = nullptr;
}

Loot *LOOT_Add(uint32_t f) {
  Loot *p = LOOT_Get(f);
  if (p)
    return p;

  if (lootCount >= LOOT_SIZE_MAX) {
    /* FIFO: вытесняем самый старый незащищённый (список упорядочен по
     * добавлению, пока его не отсортировали — тогда «старый» условен) */
    int16_t evict = -1;
    for (uint16_t i = 0; i < lootCount; ++i) {
      if (!loot[i].whitelist && !loot[i].blacklist) {
        evict = (int16_t)i;
        break;
      }
    }
    if (evict < 0)
      return nullptr; /* всё защищено пользователем */
    LOOT_Remove((uint16_t)evict);
  }

  Loot *n = &loot[lootCount++];
  *n = Loot{};
  n->f = f;
  n->lastTimeOpen = (uint16_t)(millis() / 1000);
  n->code = LOOT_NO_CODE;
  return n;
}

Loot *LOOT_Hit(uint32_t f, uint16_t rssi, uint8_t modulation, uint8_t bw,
               uint8_t radio) {
  Loot *it = LOOT_Add(f);
  if (!it)
    return nullptr;

  uint32_t now = millis();

  if (durOwner != it) {
    durOwner = it;
    durFracMs = 0;
    lastTickMs = now;
  }
  uint32_t dms = now - lastTickMs;
  lastTickMs = now;
  durFracMs += (uint16_t)(dms > 999 ? 999 : dms); /* защита от скачков */
  while (durFracMs >= 1000) {
    durFracMs -= 1000;
    if (it->duration < 0xFFFF)
      it->duration++;
  }

  it->lastTimeOpen = (uint16_t)(now / 1000);
  it->open = true;
  it->modulation = modulation;
  it->bw = bw;
  it->radio = radio;
  uint8_t pk = (uint8_t)(rssi > 511 ? 255 : rssi >> 1);
  if (pk > it->peak)
    it->peak = pk;

  gLastActiveLoot = it;
  lastActiveF = it->f;
  return it;
}

void LOOT_Standby(void) {
  for (uint16_t i = 0; i < lootCount; ++i)
    loot[i].open = false;
  durOwner = nullptr;
}

bool LOOT_IsSkipped(uint32_t f, uint32_t tol, bool monitor) {
  for (uint16_t i = 0; i < lootCount; ++i) {
    const Loot *l = &loot[i];
    if (!l->blacklist && !l->whitelist)
      continue;
    uint32_t d = l->f > f ? l->f - f : f - l->f;
    if (d > tol)
      continue;
    if (l->blacklist)
      return true; /* чёрный — всегда */
    if (!monitor)
      return true; /* белый — вне мониторинга */
  }
  return false;
}

void LOOT_BlacklistLast(void) {
  if (gLastActiveLoot) {
    gLastActiveLoot->whitelist = false;
    gLastActiveLoot->blacklist = true;
  }
}

void LOOT_WhitelistLast(void) {
  if (gLastActiveLoot) {
    gLastActiveLoot->blacklist = false;
    gLastActiveLoot->whitelist = true;
  }
}

void LOOT_ToggleBlacklist(Loot *item) {
  if (!item)
    return;
  item->whitelist = false;
  item->blacklist = !item->blacklist;
}

void LOOT_ToggleWhitelist(Loot *item) {
  if (!item)
    return;
  item->blacklist = false;
  item->whitelist = !item->whitelist;
}

uint32_t LOOT_SecondsAgo(const Loot *item) {
  /* lastTimeOpen — секунды uint16 с переполнением (~18 ч): вычитание в
   * uint16 даёт верную разницу и через границу */
  uint16_t nowS = (uint16_t)(millis() / 1000);
  return (uint16_t)(nowS - item->lastTimeOpen);
}

bool LOOT_SortByLastOpenTime(const Loot *a, const Loot *b) {
  /* свежие (меньше «секунд назад») раньше */
  return LOOT_SecondsAgo(a) > LOOT_SecondsAgo(b);
}

bool LOOT_SortByDuration(const Loot *a, const Loot *b) {
  return a->duration < b->duration;
}

bool LOOT_SortByF(const Loot *a, const Loot *b) { return a->f > b->f; }

bool LOOT_SortByBlacklist(const Loot *a, const Loot *b) {
  return a->blacklist < b->blacklist;
}

void LOOT_Sort(LootCmp compare, bool reverse) {
  /* пузырёк: n ≤ 200, вызывается по нажатию — хватает */
  for (uint16_t i = 0; i + 1 < lootCount; i++) {
    bool swapped = false;
    for (uint16_t j = 0; j + 1 < lootCount - i; j++) {
      if (compare(&loot[j], &loot[j + 1]) ^ reverse) {
        Loot tmp = loot[j];
        loot[j] = loot[j + 1];
        loot[j + 1] = tmp;
        swapped = true;
      }
    }
    if (!swapped)
      break;
  }
  durOwner = nullptr;
  relinkLast();
}
