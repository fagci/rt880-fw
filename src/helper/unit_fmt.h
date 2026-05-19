#ifndef UNIT_FMT_H
#define UNIT_FMT_H

#include <stdint.h>
#include "printf.h"

#define UHZ 1
#define UKHZ 100
#define UMHZ 100000

/* Порядок должен совпадать с InputUnit из finput.h:
   UNIT_RAW=0, UNIT_HZ=1, UNIT_KHZ=2, UNIT_MHZ=3,
   UNIT_VOLTS=4, UNIT_DBM=5, UNIT_PERCENT=6, UNIT_MS=7  */

static inline uint32_t unit_multiplier(uint8_t unit) {
  switch (unit) {
    case 3: return UMHZ;  /* UNIT_MHZ */
    case 2: return UKHZ;  /* UNIT_KHZ */
    case 4: return 100;   /* UNIT_VOLTS */
    default: return 1;
  }
}

static inline uint8_t unit_frac_width(uint32_t mul) {
  uint8_t n = 0;
  while (mul >= 10) { mul /= 10; n++; }
  return n;
}

/* Форматирует значение value с unit-мультипликатором в строку.
   Для MHZ всегда 3 знака после точки. Возвращает длину строки. */
static inline uint8_t fmt_value(char *buf, uint8_t cap, uint32_t value,
                                 uint32_t mul, uint8_t unit) {
  if (mul <= 1)
    return (uint8_t)snprintf(buf, cap, "%lu", (unsigned long)value);

  uint32_t ip = value / mul;
  uint32_t fp = value % mul;
  uint8_t fw = unit_frac_width(mul);

  if (unit == 3) { /* UNIT_MHZ — всегда 3 знака */
    if (fw > 3) {
      uint32_t d = 1;
      for (uint8_t i = fw - 3; i; i--) d *= 10;
      fp /= d;
    } else while (fw < 3) { fp *= 10; fw++; }
    return (uint8_t)snprintf(buf, cap, "%lu.%03lu",
                              (unsigned long)ip, (unsigned long)fp);
  }

  while (fw > 0 && fp % 10 == 0) { fp /= 10; fw--; }
  if (fw > 0) {
    if (fw > 6) fw = 6;
    return (uint8_t)snprintf(buf, cap, "%lu.%0*lu",
                              (unsigned long)ip, fw, (unsigned long)fp);
  }
  return (uint8_t)snprintf(buf, cap, "%lu", (unsigned long)ip);
}

#endif
