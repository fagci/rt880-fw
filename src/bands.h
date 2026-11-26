#ifndef BANDS_H
#define BANDS_H

#include <stdbool.h>
#include <stdint.h>

/* Группы диапазонов: SIDE1/SIDE2 перебирают диапазоны своей группы.
 * BC — вещание (AM/WFM), HAM — любительские SSB, SVC — службы (CB, авиа,
 * 2 м/70 см, морской, PMR). Общие диапазоны вне групп. */
typedef enum { CAT_BC, CAT_HAM, CAT_SVC, CAT_COUNT, CAT_NONE = 0xFF } BandCat;

typedef struct {
  uint32_t startKhz;
  uint32_t endKhz;
  uint8_t mod;
  uint8_t step;
  const char *name;
  uint8_t cat;
  bool loop; /* шаг UP/DOWN зацикливается внутри диапазона; выбирается SIDE1/2 */
  int16_t corr10;  /* смещение приёма, ед. 10 Гц */
  uint8_t squelch; /* 0..10; BAND_SQL_NONE — не менять глобальный */
  uint8_t color;   /* 0..BAND_COLOR_COUNT-1 */
} BandPlan;

/* ── Пользовательский список диапазонов ───────────────────────────────
 * План редактируется во флеше: при первом изменении заводской PLAN
 * копируется в записи, дальше всё читается из кольца W25Q32. В RAM
 * лежит только сам список; Band_At/Band_Find возвращают общий снимок
 * BandPlan, поэтому указатель нельзя держать через другие вызовы
 * Band_* (значение полей — копировать сразу). */
#define BAND_MAX 40
#define BAND_NAME_LEN 12
#define BAND_SQL_NONE 0xFF

typedef struct __attribute__((packed)) {
  uint32_t startKhz;
  uint32_t endKhz;
  int16_t corr10;
  uint8_t mod;
  uint8_t step;
  uint8_t cat;
  uint8_t color;
  uint8_t squelch;
  uint8_t reserved;
  char name[BAND_NAME_LEN];
} BandRec;

static_assert(sizeof(BandRec) == 28, "BandRec must be 28 bytes");

/* Сетка вещательных КВ-диапазонов: 5 кГц. Частоты — в единицах 10 Гц. */
#define BC_CH_STEP 500U

typedef struct {
  const char *name;
  uint32_t start; /* ед. 10 Гц */
  uint32_t end;   /* ед. 10 Гц, исключительно */
} BcBand;

extern const BcBand BC_BANDS[];
extern const uint8_t BC_BAND_COUNT;

int8_t Band_Index(uint32_t f10);
const BandPlan *Band_Find(uint32_t f10);
const char *Band_Name(uint32_t f10);
bool Band_Range(uint32_t f10, uint32_t *start, uint32_t *end);

/* Инициализация после Store_Init (идемпотентно). Пустая флеш — заводской
 * PLAN в RAM; запись появится после первого изменения. */
void Band_Init(void);
void Band_Reset(void);
void Band_Poll(void);
void Band_Touch(void); /* поля записи изменились: отложенная запись */

uint8_t Band_Count(void);
const BandPlan &Band_At(uint8_t i);

const BandRec *Band_Rec(uint16_t i);
BandRec *Band_RecEdit(uint16_t i);
int16_t Band_Add(const BandRec *rec); /* индекс или -1 */
bool Band_Delete(uint16_t i);

int16_t Band_Corr10(uint32_t f10);
uint8_t Band_Squelch(uint32_t f10);
uint8_t Band_Color(uint32_t f10);

uint32_t Band_Mem(uint8_t i);
void Band_MemSet(uint8_t i, uint32_t f);
int8_t Band_GroupLast(uint8_t cat);
void Band_GroupLastSet(uint8_t cat, int8_t i);

#endif
