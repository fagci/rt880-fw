#ifndef MENU_OVERLAY_H
#define MENU_OVERLAY_H

#include "../driver/keyboard.h"
#include "lvgl.h"
#include <stdint.h>

/* Пункт меню: имя, текущее значение и изменение на -1/+1. id — необязательный
 * контекст пункта (индекс регистра в меню BK4819 и т.п.). */
struct MenuItem {
  const char *name;
  const char *(*value)(const MenuItem *item);
  void (*change)(const MenuItem *item, int8_t dir);
  uint16_t id;
};

/* Высота окна: подсказка + 4 пункта (строка F_S = 13 px) с рамкой. */
#define MENU_OVERLAY_BOX_H 67

/* Небольшое меню поверх текущего экрана. Если spectrumOverlay — разрешить
 * LVGL писать в зону спектра: спектр и водопад продолжают рисоваться
 * напрямую, а меню перерисовывается поверх своей области. */
void MenuOverlay_Open(const MenuItem *items, uint8_t count, lv_coord_t x,
                      lv_coord_t y, lv_coord_t w, lv_coord_t h,
                      bool spectrumOverlay);
void MenuOverlay_Close(void);
bool MenuOverlay_IsOpen(void);
/* true — клавиша обработана; STAR закрывает меню и возвращает false,
 * чтобы режим открыл loot. */
bool MenuOverlay_Key(KEY_Code_t key, Key_State_t state);
/* true — меню действительно перерисовалось (был dirty): это SPI-заливка,
 * после неё режиму нужно выдержать паузу перед замером. */
bool MenuOverlay_Render(void);
/* Режим нарисовал напрямую (DMA) полосу y1..y2 поверх части меню —
 * перерисовать только пересечение с окном меню. Без прямых заливок меню
 * не трогаем: лишние flush — это помеха в 437–441 МГц. */
void MenuOverlay_InvalidateBand(int16_t y1, int16_t y2);

#endif
