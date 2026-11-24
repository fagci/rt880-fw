#ifndef MENU_OVERLAY_H
#define MENU_OVERLAY_H

#include "../driver/keyboard.h"
#include "lvgl.h"
#include <stdint.h>

/* Пункт меню: имя, текущее значение и изменение на -1/+1. */
struct MenuItem {
  const char *name;
  const char *(*value)(void);
  void (*change)(int8_t dir);
};

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
void MenuOverlay_Render(void);
/* Режим только что нарисовал спектр напрямую (DMA) поверх области меню —
 * вернуть меню на следующем кадре. Без прямых заливок меню не трогаем:
 * лишние flush — это помеха в 437–441 МГц. */
void MenuOverlay_Invalidate(void);

#endif
