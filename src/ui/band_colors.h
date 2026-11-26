#ifndef BAND_COLORS_H
#define BAND_COLORS_H

#include <stdbool.h>
#include <stdint.h>

#include "graphics.h"

/* Небольшая палитра цвета диапазона: индекс хранится в BandRec.color.
 * Для LVGL-разметки (#RRGGBB) в светлой теме берём более тёмные оттенки,
 * иначе пастель на белом не читается. */
#define BAND_COLOR_COUNT 8

static inline Color Band_Palette565(uint8_t i) {
  switch (i & 7) {
  case 0: return C_RED;
  case 1: return C_GREEN;
  case 2: return C_CYAN;
  case 3: return C_YELLOW;
  case 4: return C_BLUE;
  case 5: return C_MAGENTA;
  case 6: return C_WHITE;
  default: return C_GRAY;
  }
}

static inline const char *Band_PaletteName(uint8_t i) {
  static const char *const N[BAND_COLOR_COUNT] = {
      "RED", "GREEN", "CYAN", "YELLOW", "BLUE", "MAGENTA", "WHITE", "GRAY"};
  return N[i & 7];
}

static inline const char *Band_PaletteHex(uint8_t i, bool light) {
  static const char *const D[BAND_COLOR_COUNT] = {
      "F28B82", "8CE99A", "9ADCE8", "F5E6A3",
      "8AB4F8", "E8A3D4", "FFFFFF", "9AA0A6"};
  static const char *const L[BAND_COLOR_COUNT] = {
      "B03A2E", "1E8449", "117A8B", "8A6D00",
      "2D5BB8", "8E3D7B", "303030", "5F6368"};
  return light ? L[i & 7] : D[i & 7];
}

#endif
