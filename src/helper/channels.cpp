#include "channels.h"

#include "lootlist.h"
#include "store.h"
#include "tone.h"

#include "../driver/board.h"
#include "../radio.h"

#include <stdio.h>
#include <string.h>

#define CH_SAVE_DELAY_MS 2500u

/* ─── RAM-состояние ─── */
static struct Ctx {
  uint16_t count; /* сколько каналов в текущей записи */
  uint16_t mask;  /* выбранные сканлисты; 0 = ALL */
  bool dirty;
  uint32_t dirtyAt;
} ctx;

/* ─── Бэкенд: на железе — кольцо W25Q32, в превью — RAM ─── */
#ifdef HOST_PREVIEW
static CH hostCh[CH_MAX];
static CH hostTmp[CH_MAX];
static uint16_t hostCount;
static uint16_t hostTmpCount;
static bool hostWriting;

static bool backendLoad(uint16_t *count, uint16_t *mask) {
  *count = 0;
  *mask = CH_SCANLIST_ALL;
  return false;
}
static bool backendRead(uint16_t idx, CH *out) {
  if (idx >= hostCount)
    return false;
  *out = hostCh[idx];
  return true;
}
static bool backendBegin(void) {
  hostTmpCount = 0;
  hostWriting = true;
  return true;
}
static bool backendPut(const CH *ch) {
  if (!hostWriting || hostTmpCount >= CH_MAX)
    return false;
  hostTmp[hostTmpCount++] = *ch;
  return true;
}
static bool backendCommit(uint16_t count, uint16_t mask) {
  (void)mask;
  if (!hostWriting || count != hostTmpCount)
    return false;
  memcpy(hostCh, hostTmp, (size_t)count * sizeof(CH));
  hostCount = count;
  hostWriting = false;
  return true;
}
static void backendAbort(void) { hostWriting = false; }
#else
static bool backendLoad(uint16_t *count, uint16_t *mask) {
  return Store_ChannelsLoad(count, mask);
}
static bool backendRead(uint16_t idx, CH *out) {
  return Store_ChannelsRead(idx, out);
}
static bool backendBegin(void) { return Store_ChannelsWriteBegin(); }
static bool backendPut(const CH *ch) { return Store_ChannelsWritePut(ch); }
static bool backendCommit(uint16_t count, uint16_t mask) {
  return Store_ChannelsWriteCommit(count, mask);
}
static void backendAbort(void) { Store_ChannelsWriteAbort(); }
#endif

/* B спит и антенны не имеет — такие каналы всегда на A. */
static uint8_t chRadio(uint8_t r) {
  return r == RADIO_BK4819B ? (uint8_t)RADIO_BK4819A : r;
}

/* Имя не должно ломать recolor LVGL и строку списка. */
static void chSanitize(CH *ch) {
  ch->modulation &= 7;
  ch->bw &= 15;
  if (ch->bw > 9)
    ch->bw = 0;
  ch->radio = chRadio(ch->radio & 3);
  ch->blacklist = ch->blacklist ? 1 : 0;
  if (ch->tone != CH_TONE_NONE) {
    if (ch->tone & TONE_CODE_DCS) {
      if ((ch->tone & 0x7Fu) >= 104)
        ch->tone = CH_TONE_NONE;
    } else if (ch->tone >= 50) {
      ch->tone = CH_TONE_NONE;
    }
  }
  if (ch->tone == CH_TONE_NONE)
    ch->toneSql = 0;
  ch->name[CH_NAME_LEN - 1] = 0;
  for (uint8_t i = 0; i < CH_NAME_LEN - 1; ++i) {
    uint8_t c = (uint8_t)ch->name[i];
    if (!c) {
      memset(&ch->name[i], 0, CH_NAME_LEN - i);
      break;
    }
    if (c < 32 || c > 126 || c == '#')
      ch->name[i] = ' ';
  }
}

static bool writeError(void) {
  backendAbort();
  return false;
}

/* Потоковая перезапись всего списка: skip — выбросить, flip — снять/поставить
 * blacklist, repl — заменить запись. Читает старую запись и пишет новую,
 * массива в RAM нет. */
static bool rewriteList(int16_t skip, int16_t flip, int16_t repl,
                        const CH *rep) {
  if (!backendBegin())
    return false;
  uint16_t n = 0;
  CH put;
  for (uint16_t i = 0; i < ctx.count; ++i) {
    CH ch;
    if (!backendRead(i, &ch))
      return writeError();
    if (i == skip)
      continue;
    if (i == flip)
      ch.blacklist = !ch.blacklist;
    if (i == repl && rep)
      ch = *rep;
    put = ch;
    chSanitize(&put);
    if (!backendPut(&put))
      return writeError();
    n++;
  }
  if (!backendCommit(n, ctx.mask))
    return writeError();
  ctx.count = n;
  ctx.mask &= 0xFFu;
  return true;
}

/* Loot → каналы: существующие по частоте обновляем, новые дописываем.
 * Один проход чтения кольца + проход по loot; битовая карта 25 байт
 * отмечает уже обработанные находки. */
static int16_t saveFromLoot(bool blacklist) {
  uint8_t seen[(LOOT_SIZE_MAX + 7) / 8];
  memset(seen, 0, sizeof(seen));

  if (!backendBegin())
    return -1;

  uint16_t n = 0;
  int16_t saved = 0;
  for (uint16_t i = 0; i < ctx.count; ++i) {
    CH ch;
    if (!backendRead(i, &ch)) {
      backendAbort();
      return -1;
    }
    Loot *l = LOOT_Get(ch.f);
    if (l && (blacklist ? l->blacklist : l->whitelist)) {
      ch.modulation = l->modulation;
      ch.bw = l->bw;
      ch.radio = chRadio(l->radio);
      ch.tone = l->code;
      ch.toneSql = l->code != LOOT_NO_CODE;
      if (blacklist) {
        ch.blacklist = 1;
        ch.scanlists = 0;
      } else {
        ch.blacklist = 0;
        if (ctx.mask && ch.scanlists)
          ch.scanlists |= ctx.mask;
      }
      int16_t li = LOOT_IndexOf(l);
      if (li >= 0)
        seen[(uint16_t)li >> 3] |= (uint8_t)(1u << ((uint16_t)li & 7u));
      saved++;
    }
    chSanitize(&ch);
    if (!backendPut(&ch)) {
      backendAbort();
      return -1;
    }
    n++;
  }

  for (uint16_t i = 0; i < LOOT_Size() && n < CH_MAX; ++i) {
    if (seen[i >> 3] & (uint8_t)(1u << (i & 7u)))
      continue;
    const Loot *l = LOOT_Item(i);
    if (!l || (blacklist ? !l->blacklist : !l->whitelist))
      continue;
    CH ch;
    memset(&ch, 0, sizeof(ch));
    ch.f = l->f;
    ch.modulation = l->modulation;
    ch.bw = l->bw;
    ch.radio = chRadio(l->radio);
    ch.tone = l->code;
    ch.toneSql = l->code != LOOT_NO_CODE;
    ch.blacklist = blacklist ? 1 : 0;
    ch.scanlists = blacklist ? 0 : ctx.mask;
    chSanitize(&ch);
    if (!backendPut(&ch)) {
      backendAbort();
      return -1;
    }
    n++;
    saved++;
  }

  if (!backendCommit(n, ctx.mask)) {
    backendAbort();
    return -1;
  }
  ctx.count = n;
  return saved;
}

void CH_Init(void) {
  ctx.count = 0;
  ctx.mask = CH_SCANLIST_ALL;
  ctx.dirty = false;
  ctx.dirtyAt = 0;

  uint16_t n, mask;
  if (backendLoad(&n, &mask)) {
    ctx.count = n > CH_MAX ? CH_MAX : n;
    ctx.mask = mask & 0xFFu;
  }
}

void CH_Reset(void) {
  ctx.count = 0;
  ctx.mask = CH_SCANLIST_ALL;
  ctx.dirty = false;
  ctx.dirtyAt = 0;
#ifdef HOST_PREVIEW
  hostCount = 0;
  hostTmpCount = 0;
  hostWriting = false;
#endif
}

void CH_MarkDirty(void) {
  ctx.dirty = true;
  ctx.dirtyAt = millis();
}

void CH_Poll(void) {
  if (!ctx.dirty)
    return;
  if (millis() - ctx.dirtyAt < CH_SAVE_DELAY_MS)
    return;
  /* Тот же список, новая маска: перезапись кольца одним снимком. */
  if (rewriteList(-1, -1, -1, nullptr))
    ctx.dirty = false;
  else
    ctx.dirtyAt = millis();
}

uint16_t CH_Count(void) { return ctx.count; }

uint16_t CH_GetScanlistMask(void) { return ctx.mask; }

void CH_SetScanlistMask(uint16_t mask) { ctx.mask = mask & 0xFFu; }

void CH_MaskString(char *b, size_t n, uint16_t mask) {
  if (!b || !n)
    return;
  if (!mask) {
    snprintf(b, n, "ALL");
    return;
  }
  char *p = b;
  char *end = b + n - 1;
  p[0] = 0;
  for (uint8_t i = 0; i < CH_SCANLIST_COUNT && p < end; ++i) {
    if (!(mask & (1u << i)))
      continue;
    int w = snprintf(p, (size_t)(end - p + 1), "%s%u",
                     p == b ? "" : ".", (unsigned)(i + 1));
    if (w < 0 || w > end - p)
      break;
    p += w;
  }
}

bool CH_Read(uint16_t storageIdx, CH *out) {
  if (!backendRead(storageIdx, out))
    return false;
  chSanitize(out);
  return true;
}

uint16_t CH_BuildList(uint16_t mask, uint8_t *out, uint16_t max) {
  uint16_t n = 0;
  /* pass 0 — обычные по маске, pass 1 — все blacklist в конце */
  for (uint8_t pass = 0; pass < 2; ++pass) {
    for (uint16_t i = 0; i < ctx.count && n < max; ++i) {
      CH ch;
      if (!backendRead(i, &ch))
        continue;
      bool bl = ch.blacklist != 0;
      if ((pass == 0) == bl)
        continue;
      if (!bl && mask && ch.scanlists && !(ch.scanlists & mask))
        continue;
      out[n++] = (uint8_t)i;
    }
  }
  return n;
}

int16_t CH_SaveWhitelistFromLoot(void) { return saveFromLoot(false); }

int16_t CH_SaveBlacklistFromLoot(void) { return saveFromLoot(true); }

int16_t CH_LoadBlacklistToLoot(void) {
  int16_t n = 0;
  for (uint16_t i = 0; i < ctx.count; ++i) {
    CH ch;
    if (!backendRead(i, &ch))
      break;
    if (!ch.blacklist)
      continue;
    Loot *l = LOOT_Add(ch.f);
    if (!l)
      continue;
    l->blacklist = true;
    l->whitelist = false;
    l->modulation = ch.modulation;
    l->bw = ch.bw;
    l->radio = ch.radio;
    n++;
  }
  if (n)
    Store_MarkDirty();
  return n;
}

bool CH_Add(const CH *in) {
  if (!in || !in->f)
    return false;

  CH add = *in;
  chSanitize(&add);
  add.blacklist = 0;
  if (!add.scanlists)
    add.scanlists = ctx.mask;

  if (!backendBegin())
    return false;

  uint16_t n = 0;
  bool found = false;
  for (uint16_t i = 0; i < ctx.count; ++i) {
    CH ch;
    if (!backendRead(i, &ch)) {
      backendAbort();
      return false;
    }
    if (!found && ch.f == add.f) {
      add.scanlists = ch.scanlists;
      if (ctx.mask && add.scanlists)
        add.scanlists |= ctx.mask;
      if (!add.name[0]) /* добавление из VFO имени не знает */
        memcpy(add.name, ch.name, CH_NAME_LEN);
      ch = add;
      chSanitize(&ch);
      found = true;
    }
    if (!backendPut(&ch)) {
      backendAbort();
      return false;
    }
    n++;
  }

  if (!found) {
    if (n >= CH_MAX || !backendPut(&add)) {
      backendAbort();
      return false;
    }
    n++;
  }

  if (!backendCommit(n, ctx.mask)) {
    backendAbort();
    return false;
  }
  ctx.count = n;
  return true;
}

bool CH_Update(uint16_t storageIdx, const CH *in) {
  if (storageIdx >= ctx.count || !in || !in->f)
    return false;
  CH ch = *in;
  chSanitize(&ch);
  return rewriteList(-1, -1, (int16_t)storageIdx, &ch);
}

bool CH_Delete(uint16_t storageIdx) {
  if (storageIdx >= ctx.count)
    return false;
  return rewriteList((int16_t)storageIdx, -1, -1, nullptr);
}

bool CH_ToggleBlacklist(uint16_t storageIdx) {
  if (storageIdx >= ctx.count)
    return false;
  return rewriteList(-1, (int16_t)storageIdx, -1, nullptr);
}
