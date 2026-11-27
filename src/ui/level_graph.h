#pragma once

#include "graphics.h"
#include <stdint.h>

/* «Level in VFO»: история RSSI во времени (как SP_RenderGraph в
 * k1-fw/s0v4). Один столбец на выборку; при 250 мс окно 240 px — 60 с.
 * Рисуется прямым DMA маленькими порциями, поэтому обновление в HOLD не
 * заливает весь экран и не даёт большой SPI-помехи. Высота задаётся
 * вызывающим: 48 px в сканере, 20 px в активной строке VFO. */

/* Сбросить состояние, не рисуя (экран очищается отдельно). */
void LevelGraph_Hide(void);
void LevelGraph_Reset(uint16_t y, uint8_t h);
/* Следующая выборка: затирает самый старый столбец и рисует новый отрезок. */
void LevelGraph_Add(uint16_t y, uint16_t rssi, uint8_t h);
