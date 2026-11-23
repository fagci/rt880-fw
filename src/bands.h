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
} BandPlan;

/* Сетка вещательных КВ-диапазонов: 5 кГц. Частоты — в единицах 10 Гц. */
#define BC_CH_STEP 500U

typedef struct {
  const char *name;
  uint32_t start; /* ед. 10 Гц */
  uint32_t end;   /* ед. 10 Гц, исключительно */
} BcBand;

extern const BcBand BC_BANDS[];
extern const uint8_t BC_BAND_COUNT;

uint8_t Band_Count(void);
const BandPlan &Band_At(uint8_t i);
int8_t Band_Index(uint32_t f10);
const BandPlan *Band_Find(uint32_t f10);
const char *Band_Name(uint32_t f10);
bool Band_Range(uint32_t f10, uint32_t *start, uint32_t *end);

uint32_t Band_Mem(uint8_t i);
void Band_MemSet(uint8_t i, uint32_t f);
int8_t Band_GroupLast(uint8_t cat);
void Band_GroupLastSet(uint8_t cat, int8_t i);

#endif
