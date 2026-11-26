#ifndef NAMES_H
#define NAMES_H

/* Названия модуляций по индексу ModulationType; [7] — запасной "?".
 * Использовать как MOD_NAMES[modulation & 7]. */
extern const char *const MOD_NAMES[8];
extern const char *const RADIO_NAMES[3];
/* Полоса BK4819_FilterBandwidth_t 0..9: "6k".."26k". */
extern const char *const BW_NAMES[10];

#endif
