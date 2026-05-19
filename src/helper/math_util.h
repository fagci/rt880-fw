#ifndef MATH_UTIL_H
#define MATH_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline uint32_t sqrt32(uint32_t v) {
  if (v <= 1) return v;
  uint32_t x = v;
  uint32_t y = (x + 1) >> 1;
  while (y < x) { x = y; y = (y + v / y) >> 1; }
  return x;
}

static inline uint32_t clz32(uint32_t v) {
  return v ? (uint32_t)__builtin_clz(v) : 32;
}

static inline uint32_t popcount32(uint32_t v) {
  return (uint32_t)__builtin_popcount(v);
}

static inline uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

#endif
