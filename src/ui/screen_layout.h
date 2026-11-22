#pragma once

#include "../helper/unit_fmt.h"
#include "graphics.h"

/* Общая сетка экранов «Сканер» и «Анализатор» (240×320).
 *
 * Шрифты (tools/gen_fonts.sh):
 *   F_S: line 13, ascent 10, descent 3
 *   F_M: line 16, ascent 13, descent 3
 *
 *   y =   0..23   статуслайн
 *        25..40   поля границ (F_M, baseline 38)
 *        28..40   шаг скана по центру (F_S, baseline 38)
 *        54..99   спектр (46 px, тики внизу)
 *       103..118  строка 1 (F_M, baseline 116)
 *       122..134  строка 2 (F_S, baseline 132)
 *       138..319  водопад (аппаратный vscroll)
 *
 * Между зонами зазор ≥1 px. Зоны спектра (54..99) и водопада (138..319)
 * рисуются напрямую через DMA: LVGL в них не пишет (lv_port_set_direct_areas),
 * поэтому текст и виджеты размещать только в разрешённых полосах.
 */
#define UI_STATUS_H 24
#define UI_RANGE_BASE 38
#define UI_SPECTRUM_Y 54
#define UI_SPECTRUM_H 46
#define UI_INFO1_BASE 116
#define UI_INFO2_BASE 132
#define UI_WATERFALL_Y 138

#define UI_LEFT_X 8
#define UI_RIGHT_X (LCD_WIDTH - 8)
