#pragma once

#include "menu_overlay.h"
#include <stdbool.h>
#include <stdint.h>

/* Пункты общего меню параметров приёмника. VFO добавляет фильтр и ручной
 * выбор приёмника; сканер и анализатор их не показывают (у них BK и своя
 * полоса скана). */
typedef enum {
  RADIO_MENU_STEP,
  RADIO_MENU_FILTER,
  RADIO_MENU_RADIO,
  RADIO_MENU_MOD,
  RADIO_MENU_BW,
  RADIO_MENU_SQL,
  RADIO_MENU_GAIN,
  RADIO_MENU_AFC,
} RadioMenuItemId;

/* Вызывается после изменения пункта: режим применяет параметр к своему
 * тракту/сетке (VFO — Radio_TuneTo, скан — BK напрямую). */
typedef void (*RadioMenuApply)(uint8_t id);

void RadioMenu_SetApply(RadioMenuApply fn);

/* Пункты держим в const-массивах режимов (flash), а не в BSS: на AT32F423
 * свободного стека всего ~2 КБ. */
const char *RadioMenu_Value(const MenuItem *item);
void RadioMenu_Change(const MenuItem *item, int8_t dir);

#define RADIO_MENU_ITEM_STEP                                                  \
  {"Step", RadioMenu_Value, RadioMenu_Change, RADIO_MENU_STEP}
#define RADIO_MENU_ITEM_FILTER                                                \
  {"Filter", RadioMenu_Value, RadioMenu_Change, RADIO_MENU_FILTER}
#define RADIO_MENU_ITEM_RADIO                                                 \
  {"Radio", RadioMenu_Value, RadioMenu_Change, RADIO_MENU_RADIO}
#define RADIO_MENU_ITEM_MOD                                                   \
  {"Mod", RadioMenu_Value, RadioMenu_Change, RADIO_MENU_MOD}
#define RADIO_MENU_ITEM_BW                                                    \
  {"BW", RadioMenu_Value, RadioMenu_Change, RADIO_MENU_BW}
#define RADIO_MENU_ITEM_SQL                                                   \
  {"SQL", RadioMenu_Value, RadioMenu_Change, RADIO_MENU_SQL}
#define RADIO_MENU_ITEM_GAIN                                                  \
  {"Gain", RadioMenu_Value, RadioMenu_Change, RADIO_MENU_GAIN}
#define RADIO_MENU_ITEM_AFC                                                   \
  {"AFC", RadioMenu_Value, RadioMenu_Change, RADIO_MENU_AFC}
