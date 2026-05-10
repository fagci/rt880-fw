#ifndef MISC_H
#define MISC_H

#include <stdbool.h>
#include <stdint.h>

#define KHZ 100
#define MHZ 100000

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif

typedef struct {
  uint32_t f;
  uint32_t cd;
  uint16_t ct;
  uint16_t rssi;
  uint8_t noise;
  bool open;
  bool blacklist;
  bool goodKnown;
} Loot;

typedef struct {
  uint32_t start : 27;
  uint32_t end : 27;
} FRange;

#endif
