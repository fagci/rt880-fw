#include "store.h"

#include "lootlist.h"

#include "../bands.h"
#include "../driver/board.h"
#include "../driver/uart.h"
#include "../driver/w25q32.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/spectrum.h"

#include <stddef.h>
#include <string.h>

/* Хвост флеш за стоковыми данными (проверено: 0x3E0000 и далее — 0xFF). */
#define STORE_BASE 0x3E0000u
#define STORE_SLOTS 32u
#define STORE_STATE_SLOTS 24u /* 0..23 — кольцо состояния */
#define STORE_MAGIC 0x30383852u /* "R880" */
#define STORE_VERSION 2u    /* v2: канал, закреплённый за VFO */
#define STORE_VERSION_V1 1u /* старый формат без vfoChannels */
#define STORE_BANDS_MAX 48u
#define STORE_SAVE_DELAY_MS 2500u

/* Диапазоны — слоты 24..27, каналы — 28..31: своя запись на сектор. */
#define STORE_BANDS_BASE (STORE_BASE + STORE_STATE_SLOTS * W25Q_SECTOR_SIZE)
#define STORE_BANDS_SLOTS 4u
#define STORE_BAND_MAGIC 0x444E4142u /* "BAND" */
#define STORE_BAND_VERSION 1u
#define STORE_BAND_DATA_OFF 256u
#define STORE_BAND_PAGE 32u

#define STORE_CH_BASE (STORE_BASE + 28u * W25Q_SECTOR_SIZE)
#define STORE_CH_SLOTS 4u
#define STORE_CH_MAGIC 0x4E414843u /* "CHAN" */
#define STORE_CH_VERSION 2u    /* v2: имя канала, полоса 0..9 */
#define STORE_CH_VERSION_V1 1u /* старый формат без имени, 8 байт */
#define STORE_CH_DATA_OFF 256u
#define STORE_CH_PAGE 32u /* кратно sizeof(CH), экономия RAM */

/* Запись: заголовок + состояние + необязательный хвост списка находок. */
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;
  uint16_t payloadLen;
  uint32_t seq;
  uint32_t crc; /* CRC32 полезной нагрузки */
} StoreRec;

typedef struct __attribute__((packed)) {
  uint16_t lootCount;
  uint8_t currentVfo;
  uint8_t squelch;
  int8_t filter;
  uint8_t flags; /* bit0 — светлая тема, bit1 — основной вход антенны */
  uint32_t rxF[DEVICE_VFO_COUNT];
  uint8_t step[DEVICE_VFO_COUNT];
  uint8_t mod[DEVICE_VFO_COUNT];
  uint8_t bw[DEVICE_VFO_COUNT];
  uint8_t bandCount;
  int8_t groupLast[CAT_COUNT];
  uint32_t bandMem[STORE_BANDS_MAX];
  VFOChannel vfoCh[DEVICE_VFO_COUNT];
} StoreFixed;

/* Размер записи v1: до полей закреплённых каналов. */
#define STORE_FIXED_V1_SIZE ((uint16_t)offsetof(StoreFixed, vfoCh))

/* Радио-часть состояния на входе в сканирующий режим (см. Store_PinRadio). */
typedef struct __attribute__((packed)) {
  uint8_t currentVfo;
  int8_t filter;
  uint32_t rxF[DEVICE_VFO_COUNT];
  uint8_t step[DEVICE_VFO_COUNT];
  uint8_t mod[DEVICE_VFO_COUNT];
  uint8_t bw[DEVICE_VFO_COUNT];
} StoreRadio;

/* Заголовок записи каналов: данные лежат с offset STORE_CH_DATA_OFF,
 * заголовок пишется последним. 20 байт. */
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint16_t scanlist;
  uint16_t reserved;
  uint32_t seq;
  uint32_t crc; /* CRC32 данных count * sizeof(CH) */
} ChRec;

/* Канал формата v1 (8 байт): читается при обновлении прошивки и
 * переписывается в v2 при первом изменении списка. */
typedef struct __attribute__((packed)) {
  uint32_t f;
  uint16_t scanlists;
  uint8_t modulation : 3;
  uint8_t bw : 3;
  uint8_t radio : 2;
  uint8_t blacklist : 1;
  uint8_t reserved : 7;
} CH_V1;

static_assert(sizeof(CH_V1) == 8, "CH v1 must be 8 bytes");

/* Заголовок записи диапазонов: данные с offset STORE_BAND_DATA_OFF,
 * заголовок пишется последним. */
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint32_t reserved;
  uint32_t seq;
  uint32_t crc; /* CRC32 данных count * sizeof(BandRec) */
} BandHdr;

static_assert(sizeof(ChRec) == 20, "ChRec must be 20 bytes");
static_assert(sizeof(BandHdr) == 20, "BandHdr must be 20 bytes");
static_assert(STORE_CH_DATA_OFF + CH_MAX * sizeof(CH) <= W25Q_SECTOR_SIZE,
              "channel record must fit one sector");
static_assert(STORE_BAND_DATA_OFF + BAND_MAX * sizeof(BandRec) <=
                  W25Q_SECTOR_SIZE,
              "band record must fit one sector");
static_assert(BAND_MAX <= STORE_BANDS_MAX, "band count exceeds stored arrays");

static_assert(sizeof(StoreFixed) + LOOT_SIZE_MAX * sizeof(Loot) <=
                  W25Q_SECTOR_SIZE - sizeof(StoreRec),
              "store record must fit one sector");

static bool s_inited;
static bool s_valid;
static bool s_dirty;
static uint32_t s_dirtyAt;
static uint8_t s_last; /* последняя целая запись */
static uint8_t s_next; /* куда писать следующую */
static uint32_t s_seq = 1;
static bool s_radioPinned;
static StoreRadio s_pinRadio;

/* Состояние кольца каналов. */
static bool s_chValid;
static uint8_t s_chLast;
static uint8_t s_chNext;
static uint32_t s_chSeq = 1;
static uint16_t s_chCount;
static uint16_t s_chScanlist;
static uint16_t s_chVersion; /* формат последней целой записи */

/* Незавершённая потоковая запись каналов. */
static bool s_chWriting;
static uint8_t s_chTarget;
static uint16_t s_chStaged; /* сколько каналов уже в буфере/на флеш */
static uint32_t s_chCrc;
static uint32_t s_chProg; /* сколько байт данных уже запрограммировано */
static uint8_t s_chPage[STORE_CH_PAGE];
static uint8_t s_chFill;

/* Состояние кольца диапазонов. */
static bool s_bdValid;
static uint8_t s_bdLast;
static uint8_t s_bdNext;
static uint32_t s_bdSeq = 1;
static uint16_t s_bdCount;

/* Незавершённая потоковая запись диапазонов. */
static bool s_bdWriting;
static uint8_t s_bdTarget;
static uint16_t s_bdStaged;
static uint32_t s_bdCrc;
static uint32_t s_bdProg;
static uint8_t s_bdPage[STORE_BAND_PAGE];
static uint8_t s_bdFill;

static uint32_t slot_addr(uint8_t slot) {
  return STORE_BASE + (uint32_t)slot * W25Q_SECTOR_SIZE;
}

static uint32_t ch_slot_addr(uint8_t slot) {
  return STORE_CH_BASE + (uint32_t)slot * W25Q_SECTOR_SIZE;
}

static uint32_t bd_slot_addr(uint8_t slot) {
  return STORE_BANDS_BASE + (uint32_t)slot * W25Q_SECTOR_SIZE;
}

static uint32_t crc32_update(uint32_t crc, const void *data, uint32_t len) {
  const uint8_t *p = (const uint8_t *)data;
  crc = ~crc;
  while (len--) {
    crc ^= *p++;
    for (uint8_t k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

static uint16_t fixed_size(uint16_t version) {
  return version == STORE_VERSION_V1 ? STORE_FIXED_V1_SIZE
                                     : (uint16_t)sizeof(StoreFixed);
}

static bool rec_valid(const StoreRec *r) {
  if (r->magic != STORE_MAGIC)
    return false;
  if (r->version != STORE_VERSION && r->version != STORE_VERSION_V1)
    return false;
  return r->payloadLen >= fixed_size(r->version) &&
         r->payloadLen <= W25Q_SECTOR_SIZE - sizeof(StoreRec);
}

static bool fixed_valid(const StoreFixed *f) {
  if (f->lootCount > LOOT_SIZE_MAX)
    return false;
  if (f->currentVfo >= DEVICE_VFO_COUNT || f->squelch > 10)
    return false;
  if (f->filter < -1 || f->filter > 3)
    return false;
  /* bandCount может отличаться от живого списка: пользователь добавил или
   * удалил диапазон. Память диапазонов переносится по min(). */
  if (f->bandCount > STORE_BANDS_MAX)
    return false;
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; ++i) {
    if (f->rxF[i] < 1530 || f->rxF[i] > 134000000u)
      return false;
    if (f->step[i] >= STEP_COUNT || f->mod[i] > MOD_WFM || f->bw[i] > 7)
      return false;
    const VFOChannel *c = &f->vfoCh[i];
    if (!(c->flags & VFO_CH_VALID)) {
      if (c->flags & VFO_CH_MODE)
        return false;
      continue;
    }
    if (c->f < 1530 || c->f > 134000000u)
      return false;
    if (c->modulation > MOD_WFM || c->bw > 9 ||
        c->radio > RADIO_SI4732)
      return false;
    if (!memchr(c->name, 0, VFO_CH_NAME_LEN))
      return false;
  }
  for (uint8_t c = 0; c < CAT_COUNT; ++c)
    if (f->groupLast[c] < -1 || f->groupLast[c] >= (int8_t)STORE_BANDS_MAX)
      return false;
  return true;
}

static uint32_t data_crc(uint32_t addr, uint16_t len) {
  uint8_t buf[64];
  uint32_t crc = 0;
  for (uint32_t off = 0; off < len;) {
    uint32_t n = len - off;
    if (n > sizeof(buf))
      n = sizeof(buf);
    W25Q_Read(addr + off, buf, n);
    crc = crc32_update(crc, buf, n);
    off += n;
  }
  return crc;
}

static uint32_t payload_crc(uint32_t addr, uint16_t len) {
  return data_crc(addr + sizeof(StoreRec), len);
}

void Store_Init(void) {
  if (s_inited)
    return;
  s_inited = true;

  if (!W25Q_Init())
    return;

  /* Ищем самую свежую целую запись. Если последняя оборвана по питанию,
   * CRC не сойдётся — берём предыдущую, а не теряем весь список. */
  bool tried[STORE_SLOTS] = {false};
  for (;;) {
    int best = -1;
    uint32_t bestSeq = 0;
    StoreRec rec;
    for (uint8_t i = 0; i < STORE_SLOTS; ++i) {
      if (tried[i])
        continue;
      W25Q_Read(slot_addr(i), &rec, sizeof(rec));
      if (rec_valid(&rec) && (best < 0 || rec.seq > bestSeq)) {
        best = (int)i;
        bestSeq = rec.seq;
      }
    }
    if (best < 0)
      return;

    uint32_t addr = slot_addr((uint8_t)best);
    W25Q_Read(addr, &rec, sizeof(rec));
    if (payload_crc(addr, rec.payloadLen) == rec.crc) {
      StoreFixed f;
      uint16_t flen = fixed_size(rec.version);
      memset(&f, 0, sizeof(f));
      W25Q_Read(addr + sizeof(StoreRec), &f, flen);
      uint32_t len = flen + (uint32_t)f.lootCount * sizeof(Loot);
      if (fixed_valid(&f) && len == rec.payloadLen) {
        for (uint16_t i = 0; i < f.lootCount; ++i) {
          Loot *l = LOOT_LoadSlot(i);
          if (!l)
            break;
          W25Q_Read(addr + sizeof(StoreRec) + flen +
                        (uint32_t)i * sizeof(Loot),
                    l, sizeof(Loot));
          l->open = false;
          l->lastTimeOpen = 0; /* время жизни не переживает выключение */
        }
        LOOT_LoadCommit(f.lootCount);

        s_valid = true;
        s_last = (uint8_t)best;
        if (best >= (int)STORE_STATE_SLOTS) {
          /* Самая свежая запись лежит в зоне каналов (старое 32-слотовое
           * кольцо): сразу перепишем состояние в безопасную зону, пока
           * запись каналов её не затёрла. */
          s_next = 0;
          s_dirty = true;
          s_dirtyAt = 0;
        } else {
          s_next = (uint8_t)((best + 1) % STORE_STATE_SLOTS);
        }
        s_seq = rec.seq + 1;
        return;
      }
    }
    tried[best] = true;
  }
}

bool Store_HasState(void) { return s_valid; }

void Store_MarkDirty(void) {
  s_dirty = true;
  s_dirtyAt = millis();
}

void Store_PinRadio(void) {
  s_pinRadio.currentVfo = currentVfo;
  s_pinRadio.filter = filterIndex;
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; ++i) {
    const VFO *v = VFO_FrequencyState(i);
    s_pinRadio.rxF[i] = v->rxF;
    s_pinRadio.step[i] = (uint8_t)v->step;
    s_pinRadio.mod[i] = v->modulation;
    s_pinRadio.bw[i] = v->bw;
  }
  s_radioPinned = true;
}

void Store_UnpinRadio(void) { s_radioPinned = false; }

static bool save_record(void) {
  if (!W25Q_Present())
    return false;

  StoreFixed f;
  memset(&f, 0, sizeof(f));
  f.lootCount = LOOT_Size();
  f.squelch = squelchLevel;
  /* flags: bit0 — тема, bit1 — антенна, bit2..4 — палитра водопада. */
  f.flags = (gThemeLight ? 1u : 0u) | (ant_is_main() ? 2u : 0u) |
            (uint8_t)((gWaterfallPalette & 7u) << 2);
  if (s_radioPinned) {
    f.currentVfo = s_pinRadio.currentVfo;
    f.filter = s_pinRadio.filter;
    for (uint8_t i = 0; i < DEVICE_VFO_COUNT; ++i) {
      f.rxF[i] = s_pinRadio.rxF[i];
      f.step[i] = s_pinRadio.step[i];
      f.mod[i] = s_pinRadio.mod[i];
      f.bw[i] = s_pinRadio.bw[i];
    }
  } else {
    f.currentVfo = currentVfo;
    f.filter = filterIndex;
    for (uint8_t i = 0; i < DEVICE_VFO_COUNT; ++i) {
      const VFO *v = VFO_FrequencyState(i);
      f.rxF[i] = v->rxF;
      f.step[i] = (uint8_t)v->step;
      f.mod[i] = v->modulation;
      f.bw[i] = v->bw;
    }
  }
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; ++i)
    f.vfoCh[i] = vfoChannels[i];
  f.bandCount = Band_Count();
  if (f.bandCount > STORE_BANDS_MAX)
    return false;
  for (uint8_t i = 0; i < f.bandCount; ++i)
    f.bandMem[i] = Band_Mem(i);
  for (uint8_t c = 0; c < CAT_COUNT; ++c)
    f.groupLast[c] = Band_GroupLast(c);

  const Loot *loot = LOOT_Data();
  uint32_t payloadLen = sizeof(f) + (uint32_t)f.lootCount * sizeof(Loot);
  if (payloadLen > W25Q_SECTOR_SIZE - sizeof(StoreRec))
    return false;

  StoreRec rec;
  rec.magic = STORE_MAGIC;
  rec.version = STORE_VERSION;
  rec.payloadLen = (uint16_t)payloadLen;
  rec.seq = s_seq;
  rec.crc = crc32_update(0, &f, sizeof(f));
  rec.crc = crc32_update(rec.crc, loot, (uint32_t)f.lootCount * sizeof(Loot));

  uint32_t addr = slot_addr(s_next);
  if (!W25Q_SectorErase(addr))
    return false;

  uint32_t total = sizeof(rec) + payloadLen;
  uint8_t page[W25Q_PAGE_SIZE];
  for (uint32_t off = 0; off < total; off += W25Q_PAGE_SIZE) {
    uint32_t chunk = total - off;
    if (chunk > W25Q_PAGE_SIZE)
      chunk = W25Q_PAGE_SIZE;
    for (uint32_t i = 0; i < chunk; ++i) {
      uint32_t o = off + i;
      uint8_t v;
      if (o < sizeof(rec))
        v = ((const uint8_t *)&rec)[o];
      else if (o - sizeof(rec) < sizeof(f))
        v = ((const uint8_t *)&f)[o - sizeof(rec)];
      else
        v = ((const uint8_t *)loot)[o - sizeof(rec) - sizeof(f)];
      page[i] = v;
    }
    if (!W25Q_PageProgram(addr + off, page, chunk))
      return false;
  }

  if (payload_crc(addr, (uint16_t)payloadLen) != rec.crc)
    return false;

  s_last = s_next;
  s_next = (uint8_t)((s_next + 1) % STORE_STATE_SLOTS);
  s_seq++;
  s_valid = true;
  return true;
}

void Store_Poll(void) {
  if (!s_dirty)
    return;
  if (!W25Q_Present()) {
    s_dirty = false;
    return;
  }
  if (millis() - s_dirtyAt < STORE_SAVE_DELAY_MS)
    return;
  if (save_record())
    s_dirty = false;
  else
    s_dirtyAt = millis();
}

static bool read_last_fixed(StoreFixed *f) {
  if (!s_valid)
    return false;
  StoreRec rec;
  W25Q_Read(slot_addr(s_last), &rec, sizeof(rec));
  memset(f, 0, sizeof(*f));
  W25Q_Read(slot_addr(s_last) + sizeof(StoreRec), f,
            fixed_size(rec.version));
  return fixed_valid(f);
}

bool Store_ApplyUi(void) {
  StoreFixed f;
  if (!read_last_fixed(&f))
    return false;
  gThemeLight = (f.flags & 1u) != 0;
  ant_sw((f.flags & 2u) != 0);
  SP_SetPalette((f.flags >> 2) & 7u);
  return true;
}

bool Store_ApplyRadio(void) {
  StoreFixed f;
  if (!read_last_fixed(&f))
    return false;

  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; ++i) {
    vfos[i].rxF = f.rxF[i];
    vfos[i].step = (Step)f.step[i];
    vfos[i].modulation = f.mod[i];
    vfos[i].bw = f.bw[i];
    vfos[i].radio = Radio_PickDevice(vfos[i].rxF, vfos[i].modulation);
    vfoChannels[i] = f.vfoCh[i];
  }
  currentVfo = f.currentVfo;
  squelchLevel = f.squelch;
  filterIndex = f.filter;
  uint8_t bn = f.bandCount < Band_Count() ? f.bandCount : Band_Count();
  for (uint8_t i = 0; i < bn; ++i)
    Band_MemSet(i, f.bandMem[i]);
  for (uint8_t c = 0; c < CAT_COUNT; ++c)
    Band_GroupLastSet(c, f.groupLast[c]);
  return true;
}

bool Store_Wipe(void) {
  if (!W25Q_Init())
    return false;
  bool ok = true;
  for (uint8_t i = 0; i < STORE_SLOTS; ++i)
    if (!W25Q_SectorErase(slot_addr(i)))
      ok = false;
  s_valid = false;
  s_dirty = false;
  s_last = 0;
  s_next = 0;
  s_seq = 1;
  s_chValid = false;
  s_chWriting = false;
  s_chLast = s_chNext = 0;
  s_chSeq = 1;
  s_chCount = 0;
  s_chScanlist = 0;
  s_chVersion = STORE_CH_VERSION;
  s_bdValid = false;
  s_bdWriting = false;
  s_bdLast = s_bdNext = 0;
  s_bdSeq = 1;
  s_bdCount = 0;
  CH_Reset();
  Band_Reset();
  return ok;
}

/* ─── Кольцо каналов ─── */

static uint16_t ch_stride(uint16_t version) {
  return version == STORE_CH_VERSION_V1 ? (uint16_t)sizeof(CH_V1)
                                        : (uint16_t)sizeof(CH);
}

static void ch_decode_v1(const CH_V1 *o, CH *c) {
  memset(c, 0, sizeof(*c));
  c->f = o->f;
  c->scanlists = o->scanlists;
  c->modulation = o->modulation;
  c->bw = o->bw;
  c->radio = o->radio;
  c->blacklist = o->blacklist;
}

static bool ch_rec_valid(const ChRec *r) {
  if (r->magic != STORE_CH_MAGIC ||
      (r->version != STORE_CH_VERSION && r->version != STORE_CH_VERSION_V1))
    return false;
  if (r->count > CH_MAX || (r->scanlist & ~0xFFu) != 0u)
    return false;
  return STORE_CH_DATA_OFF + (uint32_t)r->count * ch_stride(r->version) <=
         W25Q_SECTOR_SIZE;
}

static bool ch_valid(const CH *c) {
  /* 153 кГц..1340 МГц — как у частот VFO в записи состояния. */
  if (c->f < 1530u || c->f > 134000000u)
    return false;
  if (c->modulation > MOD_WFM || c->bw > 9 || c->radio > RADIO_SI4732)
    return false;
  return true;
}

static bool ch_slot_channels_valid(uint32_t base, uint16_t count,
                                   uint16_t version) {
  for (uint16_t i = 0; i < count; ++i) {
    if (version == STORE_CH_VERSION_V1) {
      CH_V1 o;
      CH c;
      W25Q_Read(base + STORE_CH_DATA_OFF + (uint32_t)i * sizeof(o), &o,
                sizeof(o));
      ch_decode_v1(&o, &c);
      if (!ch_valid(&c))
        return false;
    } else {
      CH c;
      W25Q_Read(base + STORE_CH_DATA_OFF + (uint32_t)i * sizeof(c), &c,
                sizeof(c));
      if (!ch_valid(&c))
        return false;
    }
  }
  return true;
}

bool Store_ChannelsLoad(uint16_t *count, uint16_t *scanlist) {
  s_chValid = false;
  if (count)
    *count = 0;
  if (scanlist)
    *scanlist = 0;
  if (!s_inited || !W25Q_Present())
    return false;

  int best = -1;
  uint32_t bestSeq = 0;
  ChRec bestRec;
  for (uint8_t i = 0; i < STORE_CH_SLOTS; ++i) {
    uint32_t base = ch_slot_addr(i);
    ChRec r;
    W25Q_Read(base, &r, sizeof(r));
    if (!ch_rec_valid(&r))
      continue;
    if (best >= 0 && r.seq <= bestSeq)
      continue;
    uint16_t stride = ch_stride(r.version);
    if (data_crc(base + STORE_CH_DATA_OFF, (uint16_t)(r.count * stride)) !=
        r.crc)
      continue;
    if (!ch_slot_channels_valid(base, r.count, r.version))
      continue;
    best = (int)i;
    bestSeq = r.seq;
    bestRec = r;
  }
  if (best < 0) {
    s_chLast = s_chNext = 0;
    s_chSeq = 1;
    s_chCount = 0;
    s_chScanlist = 0;
    s_chVersion = STORE_CH_VERSION;
    return false;
  }

  s_chValid = true;
  s_chLast = (uint8_t)best;
  s_chNext = (uint8_t)((best + 1) % STORE_CH_SLOTS);
  s_chSeq = bestRec.seq + 1;
  s_chCount = bestRec.count;
  s_chScanlist = bestRec.scanlist;
  s_chVersion = bestRec.version;
  if (count)
    *count = s_chCount;
  if (scanlist)
    *scanlist = s_chScanlist;
  return true;
}

bool Store_ChannelsRead(uint16_t idx, CH *ch) {
  if (!s_chValid || !ch || idx >= s_chCount)
    return false;
  uint32_t base = ch_slot_addr(s_chLast) + STORE_CH_DATA_OFF;
  if (s_chVersion == STORE_CH_VERSION_V1) {
    CH_V1 o;
    W25Q_Read(base + (uint32_t)idx * sizeof(o), &o, sizeof(o));
    ch_decode_v1(&o, ch);
  } else {
    W25Q_Read(base + (uint32_t)idx * sizeof(*ch), ch, sizeof(*ch));
    ch->name[CH_NAME_LEN - 1] = 0;
  }
  return true;
}

bool Store_ChannelsWriteBegin(void) {
  s_chWriting = false;
  if (!s_inited || !W25Q_Present())
    return false;

  uint8_t next = s_chNext >= STORE_CH_SLOTS ? 0 : s_chNext;
  /* Не стираем сектор, где лежит ещё не переписанное состояние
   * (старая раскладка могла держать его в зоне каналов). */
  for (uint8_t guard = 0; guard < STORE_CH_SLOTS; ++guard) {
    bool clash = s_valid && ch_slot_addr(next) == slot_addr(s_last);
    if (!clash)
      break;
    next = (uint8_t)((next + 1) % STORE_CH_SLOTS);
  }

  if (!W25Q_SectorErase(ch_slot_addr(next)))
    return false;

  s_chTarget = next;
  s_chStaged = 0;
  s_chCrc = 0;
  s_chProg = 0;
  s_chFill = 0;
  s_chWriting = true;
  return true;
}

static bool ch_flush_page(void) {
  if (!s_chFill)
    return true;
  if (!W25Q_PageProgram(ch_slot_addr(s_chTarget) + STORE_CH_DATA_OFF + s_chProg,
                        s_chPage, s_chFill))
    return false;
  s_chProg += s_chFill;
  s_chFill = 0;
  return true;
}

bool Store_ChannelsWritePut(const CH *ch) {
  if (!s_chWriting || !ch || s_chStaged >= CH_MAX)
    return false;

  const uint8_t *p = (const uint8_t *)ch;
  uint16_t left = sizeof(*ch);
  while (left) {
    uint16_t room = (uint16_t)(STORE_CH_PAGE - s_chFill);
    uint16_t take = left < room ? left : room;
    memcpy(s_chPage + s_chFill, p, take);
    s_chFill = (uint8_t)(s_chFill + take);
    p += take;
    left -= take;
    if (s_chFill >= STORE_CH_PAGE && !ch_flush_page()) {
      s_chWriting = false;
      return false;
    }
  }

  s_chCrc = crc32_update(s_chCrc, ch, sizeof(*ch));
  s_chStaged++;
  return true;
}

bool Store_ChannelsWriteCommit(uint16_t count, uint16_t scanlist) {
  if (!s_chWriting || count != s_chStaged)
    return false;
  if (!ch_flush_page()) {
    s_chWriting = false;
    return false;
  }

  ChRec rec;
  rec.magic = STORE_CH_MAGIC;
  rec.version = STORE_CH_VERSION;
  rec.count = count;
  rec.scanlist = scanlist;
  rec.reserved = 0;
  rec.seq = s_chSeq;
  rec.crc = s_chCrc;

  uint32_t base = ch_slot_addr(s_chTarget);
  /* Заголовок — после данных: обрыв питания оставит прошлый снимок. */
  if (!W25Q_PageProgram(base, (const uint8_t *)&rec, sizeof(rec))) {
    s_chWriting = false;
    return false;
  }
  if (data_crc(base + STORE_CH_DATA_OFF, (uint16_t)(count * sizeof(CH))) !=
      rec.crc) {
    s_chWriting = false;
    return false;
  }

  s_chLast = s_chTarget;
  s_chNext = (uint8_t)((s_chTarget + 1) % STORE_CH_SLOTS);
  s_chSeq++;
  s_chCount = count;
  s_chScanlist = scanlist;
  s_chVersion = STORE_CH_VERSION;
  s_chValid = true;
  s_chWriting = false;
  return true;
}

void Store_ChannelsWriteAbort(void) { s_chWriting = false; }

/* ─── Кольцо диапазонов ─── */

static bool bd_hdr_valid(const BandHdr *r) {
  return r->magic == STORE_BAND_MAGIC && r->version == STORE_BAND_VERSION &&
         r->count <= BAND_MAX &&
         STORE_BAND_DATA_OFF + (uint32_t)r->count * sizeof(BandRec) <=
             W25Q_SECTOR_SIZE;
}

static bool bd_rec_valid(const BandRec *r) {
  return r->startKhz >= 1 && r->endKhz > r->startKhz && r->endKhz <= 1340000u;
}

static bool bd_slot_records_valid(uint32_t base, uint16_t count) {
  for (uint16_t i = 0; i < count; ++i) {
    BandRec r;
    W25Q_Read(base + STORE_BAND_DATA_OFF + (uint32_t)i * sizeof(BandRec), &r,
              sizeof(r));
    if (!bd_rec_valid(&r))
      return false;
  }
  return true;
}

bool Store_BandsLoad(uint16_t *count) {
  s_bdValid = false;
  if (count)
    *count = 0;
  if (!s_inited || !W25Q_Present())
    return false;

  int best = -1;
  uint32_t bestSeq = 0;
  BandHdr bestHdr;
  for (uint8_t i = 0; i < STORE_BANDS_SLOTS; ++i) {
    uint32_t base = bd_slot_addr(i);
    BandHdr r;
    W25Q_Read(base, &r, sizeof(r));
    if (!bd_hdr_valid(&r))
      continue;
    if (best >= 0 && r.seq <= bestSeq)
      continue;
    if (data_crc(base + STORE_BAND_DATA_OFF,
                 (uint16_t)(r.count * sizeof(BandRec))) != r.crc)
      continue;
    if (!bd_slot_records_valid(base, r.count))
      continue;
    best = (int)i;
    bestSeq = r.seq;
    bestHdr = r;
  }
  if (best < 0) {
    s_bdLast = s_bdNext = 0;
    s_bdSeq = 1;
    s_bdCount = 0;
    return false;
  }

  s_bdValid = true;
  s_bdLast = (uint8_t)best;
  s_bdNext = (uint8_t)((best + 1) % STORE_BANDS_SLOTS);
  s_bdSeq = bestHdr.seq + 1;
  s_bdCount = bestHdr.count;
  if (count)
    *count = s_bdCount;
  return true;
}

bool Store_BandsRead(uint16_t idx, BandRec *out) {
  if (!s_bdValid || !out || idx >= s_bdCount)
    return false;
  W25Q_Read(bd_slot_addr(s_bdLast) + STORE_BAND_DATA_OFF +
                (uint32_t)idx * sizeof(BandRec),
            out, sizeof(*out));
  return true;
}

bool Store_BandsWriteBegin(void) {
  s_bdWriting = false;
  if (!s_inited || !W25Q_Present())
    return false;

  uint8_t next = s_bdNext >= STORE_BANDS_SLOTS ? 0 : s_bdNext;
  for (uint8_t guard = 0; guard < STORE_BANDS_SLOTS; ++guard) {
    bool clash = s_valid && bd_slot_addr(next) == slot_addr(s_last);
    if (!clash)
      break;
    next = (uint8_t)((next + 1) % STORE_BANDS_SLOTS);
  }

  if (!W25Q_SectorErase(bd_slot_addr(next)))
    return false;

  s_bdTarget = next;
  s_bdStaged = 0;
  s_bdCrc = 0;
  s_bdProg = 0;
  s_bdFill = 0;
  s_bdWriting = true;
  return true;
}

static bool bd_flush_page(void) {
  if (!s_bdFill)
    return true;
  if (!W25Q_PageProgram(bd_slot_addr(s_bdTarget) + STORE_BAND_DATA_OFF +
                            s_bdProg,
                        s_bdPage, s_bdFill))
    return false;
  s_bdProg += s_bdFill;
  s_bdFill = 0;
  return true;
}

bool Store_BandsWritePut(const BandRec *rec) {
  if (!s_bdWriting || !rec || s_bdStaged >= BAND_MAX)
    return false;

  const uint8_t *p = (const uint8_t *)rec;
  uint16_t left = sizeof(*rec);
  while (left) {
    uint16_t room = (uint16_t)(STORE_BAND_PAGE - s_bdFill);
    uint16_t take = left < room ? left : room;
    memcpy(s_bdPage + s_bdFill, p, take);
    s_bdFill = (uint8_t)(s_bdFill + take);
    p += take;
    left -= take;
    if (s_bdFill >= STORE_BAND_PAGE && !bd_flush_page()) {
      s_bdWriting = false;
      return false;
    }
  }

  s_bdCrc = crc32_update(s_bdCrc, rec, sizeof(*rec));
  s_bdStaged++;
  return true;
}

bool Store_BandsWriteCommit(uint16_t count) {
  if (!s_bdWriting || count != s_bdStaged)
    return false;
  if (!bd_flush_page()) {
    s_bdWriting = false;
    return false;
  }

  BandHdr hdr;
  hdr.magic = STORE_BAND_MAGIC;
  hdr.version = STORE_BAND_VERSION;
  hdr.count = count;
  hdr.reserved = 0;
  hdr.seq = s_bdSeq;
  hdr.crc = s_bdCrc;

  uint32_t base = bd_slot_addr(s_bdTarget);
  if (!W25Q_PageProgram(base, (const uint8_t *)&hdr, sizeof(hdr))) {
    s_bdWriting = false;
    return false;
  }
  if (data_crc(base + STORE_BAND_DATA_OFF,
               (uint16_t)(count * sizeof(BandRec))) != hdr.crc) {
    s_bdWriting = false;
    return false;
  }

  s_bdLast = s_bdTarget;
  s_bdNext = (uint8_t)((s_bdTarget + 1) % STORE_BANDS_SLOTS);
  s_bdSeq++;
  s_bdCount = count;
  s_bdValid = true;
  s_bdWriting = false;
  return true;
}

void Store_BandsWriteAbort(void) { s_bdWriting = false; }

void Store_DebugPrint(void) {
  if (!W25Q_Present()) {
    uart_puts("STORE no flash\r\n");
  } else if (!s_valid) {
    uart_puts("STORE empty\r\n");
  } else {
    StoreFixed f;
    read_last_fixed(&f);
    StoreRec rec;
    W25Q_Read(slot_addr(s_last), &rec, sizeof(rec));
    uart_printf("STORE slot=%u seq=%lu loot=%u vfo=%u sq=%u flt=%d light=%u "
                "ant=%u bands=%u\r\n",
                (unsigned)s_last, (unsigned long)rec.seq,
                (unsigned)f.lootCount, (unsigned)f.currentVfo,
                (unsigned)f.squelch, (int)f.filter,
                (unsigned)(f.flags & 1u), (unsigned)((f.flags >> 1) & 1u),
                (unsigned)f.bandCount);
  }
  uart_printf("LIVE vfo=%u f=%lu step=%u mod=%u sq=%u flt=%d light=%u ant=%u "
              "loot=%u\r\n",
              (unsigned)currentVfo, (unsigned long)vfos[currentVfo].rxF,
              (unsigned)vfos[currentVfo].step,
              (unsigned)vfos[currentVfo].modulation, (unsigned)squelchLevel,
              (int)filterIndex, (unsigned)gThemeLight, (unsigned)ant_is_main(),
              (unsigned)LOOT_Size());

  if (!W25Q_Present())
    uart_puts("CH no flash\r\n");
  else if (!s_chValid)
    uart_puts("CH empty\r\n");
  else
    uart_printf("CH count=%u mask=%u slot=%u seq=%lu ver=%u\r\n",
                (unsigned)s_chCount, (unsigned)s_chScanlist,
                (unsigned)s_chLast, (unsigned long)(s_chSeq - 1),
                (unsigned)s_chVersion);
}
