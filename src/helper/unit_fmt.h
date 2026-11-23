#ifndef UNIT_FMT_H
#define UNIT_FMT_H

#include <stdint.h>
#include "printf.h"

/* Единицы измерения — порядок имеет значение для unit_multiplier */
typedef enum {
  UNIT_RAW,
  UNIT_HZ,
  UNIT_KHZ,
  UNIT_MHZ,
  UNIT_VOLTS,
  UNIT_DBM,
  UNIT_PERCENT,
  UNIT_MS,
} InputUnit;

#define UHZ 1
#define UKHZ 100
#define UMHZ 100000

/* Порядок должен совпадать с InputUnit выше:
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

/* Частота (единицы 10 Гц) → "145.500" — 3 знака, как в анализаторе. */
static inline uint8_t fmt_mhz3(char *buf, uint8_t cap, uint32_t f) {
  return (uint8_t)snprintf(buf, cap, "%lu.%03lu", (unsigned long)(f / UMHZ),
                           (unsigned long)((f % UMHZ) / 100));
}

/* Частота (единицы 10 Гц) → "145.50000". */
static inline uint8_t fmt_mhz5(char *buf, uint8_t cap, uint32_t f) {
  return (uint8_t)snprintf(buf, cap, "%u.%05u", (unsigned)(f / UMHZ),
                           (unsigned)(f % UMHZ));
}

/* Шаг из StepFrequencyTable (единицы 10 Гц) → "5.00k". */
static inline uint8_t fmt_step(char *buf, uint8_t cap, uint32_t step) {
  return (uint8_t)snprintf(buf, cap, "%u.%02uk", (unsigned)(step / 100),
                           (unsigned)(step % 100));
}

/* Полоса (единицы 10 Гц) → "24.00k" / "1.20M" / "1.79G". */
static inline uint8_t fmt_span(char *buf, uint8_t cap, uint32_t f) {
  if (f >= 100000000u) /* ≥ 1 ГГц */
    return (uint8_t)snprintf(buf, cap, "%lu.%02luG",
                             (unsigned long)(f / 100000000u),
                             (unsigned long)((f % 100000000u) / 1000000u));
  if (f >= 100000u) /* ≥ 1 МГц */
    return (uint8_t)snprintf(buf, cap, "%lu.%02luM",
                             (unsigned long)(f / 100000u),
                             (unsigned long)((f % 100000u) / 1000u));
  return (uint8_t)snprintf(buf, cap, "%lu.%02luk", (unsigned long)(f / 100u),
                           (unsigned long)(f % 100u));
}

/* Компактное время: «45s», «12m», «3h» (3 символа). */
static inline void fmt_duration(char *out, uint8_t cap, uint32_t s) {
  if (s < 100)
    snprintf(out, cap, "%2us", (unsigned)s);
  else if (s < 6000)
    snprintf(out, cap, "%2um", (unsigned)(s / 60));
  else
    snprintf(out, cap, "%2uh", (unsigned)(s / 3600 > 99 ? 99 : s / 3600));
}

#endif
