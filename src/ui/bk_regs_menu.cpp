#include "bk_regs_menu.h"

#include "../driver/bk4819.h"
#include "../ui/graphics.h"
#include "../ui/menu_overlay.h"
#include "../ui/screen_layout.h"
#include <stdio.h>

/* Описание поля регистра: если width == 0 — правим регистр целиком.
 * idx нужен только для генерации const-массива пунктов меню. */
typedef struct {
  const char *name;
  uint8_t reg;
  uint8_t shift;
  uint8_t width;
  uint16_t maxVal;
  uint16_t step;
} RegDesc;

#define REG_LIST(X)                                                           \
  X(0, "VCO LDO", 0x1F, 12, 4, 15, 1)                                        \
  X(1, "vReg", 0x1A, 12, 4, 15, 1)                                           \
  X(2, "iBit", 0x1A, 8, 4, 15, 1)                                            \
  X(3, "pllCp", 0x1F, 0, 4, 15, 1)                                           \
  X(4, "BndSelTh", 0x3E, 0, 16, 0xFFFF, 100)                                 \
  X(5, "DAC DRV2", 0x37, 8, 1, 1, 1)                                         \
  X(6, "RF LDO", 0x37, 9, 1, 1, 1)                                           \
  X(7, "ANA LDO", 0x37, 11, 1, 1, 1)                                         \
  X(8, "DSP V", 0x37, 12, 3, 7, 1)                                           \
  X(9, "GltchBW", 0x47, 1, 7, 0x7F, 1)                                       \
  X(10, "Compand", 0x31, 3, 1, 1, 1)                                         \
  X(11, "AVC", 0x4B, 5, 1, 1, 1)                                             \
  X(12, "VCO Calib", 0x30, 15, 1, 1, 1)                                      \
  X(13, "PLL VCO En", 0x30, 4, 4, 15, 1)                                     \
  X(14, "RX Link En", 0x30, 10, 4, 15, 1)                                    \
  X(15, "RF Filt BW", 0x43, 12, 3, 7, 1)                                     \
  X(16, "WB Filt", 0x43, 9, 3, 7, 1)                                         \
  X(17, "BW Mode", 0x43, 4, 2, 3, 1)                                         \
  X(18, "LNA Short", 0x13, 8, 2, 3, 1)                                       \
  X(19, "LNA", 0x13, 5, 3, 7, 1)                                             \
  X(20, "MIX", 0x13, 3, 2, 3, 1)                                             \
  X(21, "PGA", 0x13, 0, 3, 7, 1)                                             \
  X(22, "IF Sel", 0x3D, 0, 16, 0xFFFF, 100)                                  \
  X(23, "RX DCF", 0x7E, 3, 3, 7, 1)                                          \
  X(24, "AFC Dis", 0x73, 4, 1, 1, 1)                                         \
  X(25, "AFC Range", 0x73, 11, 3, 7, 1)                                      \
  X(26, "AGC Disable", 0x7E, 15, 1, 1, 1)                                    \
  X(27, "AGC Cnt", 0x7E, 12, 3, 7, 1)                                        \
  X(28, "AGC Hi", 0x49, 7, 7, 127, 1)                                        \
  X(29, "AGC Lo", 0x49, 0, 7, 127, 1)

#define REG_DESC(i, nm, r, sh, w, mx, st) {nm, r, sh, w, mx, st},
static const RegDesc REGS[] = {REG_LIST(REG_DESC)};

static char s_buf[8];

static uint16_t regMaxValue(const RegDesc *d) {
  if (d->maxVal)
    return d->maxVal;
  if (d->width)
    return (uint16_t)((1u << d->width) - 1u);
  return 0xFFFF;
}

static uint16_t readField(const RegDesc *d) {
  BK4819_SelectChip(0);
  uint16_t v = BK4819_ReadRegister(d->reg);
  if (d->width == 0)
    return v;
  return (uint16_t)((v >> d->shift) & ((1u << d->width) - 1u));
}

static void writeField(const RegDesc *d, uint16_t val) {
  BK4819_SelectChip(0);
  if (d->width == 0) {
    BK4819_WriteRegister(d->reg, val);
    return;
  }
  uint16_t v = BK4819_ReadRegister(d->reg);
  uint16_t mask = (uint16_t)(((1u << d->width) - 1u) << d->shift);
  v = (uint16_t)((v & ~mask) | ((val << d->shift) & mask));
  BK4819_WriteRegister(d->reg, v);
}

static const char *getValue(const MenuItem *item) {
  const RegDesc *d = &REGS[item->id];
  uint16_t v = readField(d);
  uint16_t step = d->step ? d->step : 1;
  snprintf(s_buf, sizeof(s_buf), "%u", (unsigned)(v / step));
  return s_buf;
}

static void changeValue(const MenuItem *item, int8_t dir) {
  const RegDesc *d = &REGS[item->id];
  uint16_t v = readField(d);
  uint16_t maxV = regMaxValue(d);
  uint16_t step = d->step ? d->step : 1;

  if (dir > 0) {
    v = (uint16_t)((v + step > maxV) ? 0 : v + step);
  } else {
    v = (uint16_t)((v < step) ? maxV : v - step);
  }
  writeField(d, v);
}

#define REG_ITEM(i, nm, r, sh, w, mx, st) {nm, getValue, changeValue, i},
static const MenuItem REG_ITEMS[] = {REG_LIST(REG_ITEM)};

#define REG_COUNT ((uint16_t)(sizeof(REGS) / sizeof(REGS[0])))
static_assert(sizeof(REGS) / sizeof(REGS[0]) ==
                  sizeof(REG_ITEMS) / sizeof(REG_ITEMS[0]),
              "REG_LIST: REGS and REG_ITEMS must match");

void BKRegsMenu_Open(void) {
  MenuOverlay_Open(REG_ITEMS, (uint8_t)REG_COUNT, 0, UI_STATUS_H, LCD_WIDTH / 2,
                   UI_WATERFALL_Y - UI_STATUS_H - 2, true);
}
