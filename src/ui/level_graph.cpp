#include "level_graph.h"
#include "../helper/measurements.h"

static uint16_t lgY = 0xFFFF;
static uint16_t lgX;
static uint8_t lgH = 48;
static int16_t lgPrevY = -1;

/* Больше RSSI — выше на графике (меньше y). Шкала — RSSI_MIN..RSSI_MAX,
 * как у SP_RenderGraph(GRAPH_RSSI) в k1-fw. */
static uint8_t levelY(uint16_t rssi) {
  uint8_t v = (uint8_t)ConvertDomain(rssi, RSSI_MIN, RSSI_MAX, 0, lgH - 1);
  return (uint8_t)(lgH - 1 - v);
}

/* Восстановить сетку в столбце x после затирания: рамка сверху/снизу и
 * штрих по центру каждые 4 px (2 px штрих / 2 px пропуск). */
static void gridColumn(uint8_t x) {
  FillRectNoInv(x, lgY, 1, 1, C_GRAY);
  FillRectNoInv(x, (int16_t)(lgY + lgH - 1), 1, 1, C_GRAY);
  if ((x & 3) < 2)
    FillRectNoInv(x, (int16_t)(lgY + lgH / 2), 1, 1, C_GRAY);
}

void LevelGraph_Hide(void) {
  lgY = 0xFFFF;
  lgX = 0;
  lgPrevY = -1;
}

void LevelGraph_Reset(uint16_t y, uint8_t h) {
  lgY = y;
  lgH = h;
  lgX = 0;
  lgPrevY = -1;
  FillRectNoInv(0, lgY, LCD_WIDTH, lgH, C_BLACK);
  FillRectNoInv(0, lgY, LCD_WIDTH, 1, C_GRAY);
  FillRectNoInv(0, (int16_t)(lgY + lgH - 1), LCD_WIDTH, 1, C_GRAY);
  for (uint8_t x = 0; x < LCD_WIDTH; x += 4)
    FillRectNoInv(x, (int16_t)(lgY + lgH / 2), 2, 1, C_GRAY);
}

void LevelGraph_Add(uint16_t y, uint16_t rssi, uint8_t h) {
  if (y != lgY || h != lgH)
    LevelGraph_Reset(y, h);
  uint8_t ry = levelY(rssi);
  uint8_t x = (uint8_t)lgX;

  /* Новый столбец затирает старую точку (окно ползёт по кругу). */
  FillRectNoInv(x, lgY, 1, lgH, C_BLACK);
  gridColumn(x);
  if (lgPrevY < 0) {
    FillRectNoInv(x, (int16_t)(lgY + ry), 1, 1, C_GREEN);
  } else {
    /* Шаг по x — 1 px, поэтому линия между точками вертикальна в столбце. */
    uint8_t a = (uint8_t)(lgPrevY < ry ? lgPrevY : ry);
    uint8_t b = (uint8_t)(lgPrevY < ry ? ry : lgPrevY);
    FillRectNoInv(x, (int16_t)(lgY + a), 1, (int16_t)(b - a + 1), C_GREEN);
  }
  lgPrevY = ry;

  lgX++;
  if (lgX >= LCD_WIDTH) {
    lgX = 0;
    lgPrevY = -1; /* на стыке круга линию через весь экран не рисуем */
  }
}
