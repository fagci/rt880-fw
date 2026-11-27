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
 *        28..40   шаг скана по центру (F_M, baseline 38)
 *
 * Анализатор (спектр + водопад):
 *        54..99   спектр (46 px)
 *       100..102  риски частот (3 px)
 *       103..118  строка 1 (F_M, baseline 116)
 *       122..134  строка 2 (F_S, baseline 132)
 *       138..319  водопад (аппаратный vscroll)
 *
 * Сканер и FC (общий «scan-экран», без спектра и водопада):
 *        25..41   поля границ/настройки и шаг (F_M, baseline 38)
 *        42..80   крупная текущая частота (F_XL, baseline 74)
 *        81..97   детали/время замера (F_S, baseline 94)
 *       100..260  список находок (10 строк по 16 px)
 *       270..317  «Level in VFO» (сканер, прямой DMA) / подсказки FC
 *
 * Между зонами зазор ≥1 px. Зоны спектра (54..99), рисок (100..102) и
 * водопада (138..319) рисуются напрямую через DMA: LVGL в них не пишет
 * (lv_port_set_direct_areas), поэтому текст и виджеты размещать только в
 * разрешённых полосах. Сканер скрывает эти зоны (lv_port_set_spectrum_hidden)
 * и пишет списком во всю высоту до графика уровня. Исключение — меню-оверлей
 * (lv_port_set_spectrum_overlay): LVGL пишет только область меню поверх
 * спектра; сам спектр и водопад продолжают рисоваться напрямую.
 */
#define UI_STATUS_H 24
#define UI_RANGE_BASE 38
#define UI_SPECTRUM_Y 54
#define UI_SPECTRUM_H 46
#define UI_TICKS_Y (UI_SPECTRUM_Y + UI_SPECTRUM_H) /* 100 */
#define UI_TICKS_H 3
#define UI_INFO1_BASE 116
#define UI_INFO2_BASE 132
#define UI_WATERFALL_Y 138

/* Сканер и FC: общая сетка. Крупная частота (F_XL) одна на своей строке,
 * шаг — в верхней строке (F_M), время замера — по центру строки деталей
 * (F_S) в обоих режимах. Список находок занимает зону до графика уровня. */
#define UI_SCAN_FREQ_BASE 74
#define UI_SCAN_INFO_BASE 94
#define UI_LOOT_Y 100
#define UI_LOOT_ROW_H 16
#define UI_LOOT_ROWS 10
#define UI_LEVEL_H 48
#define UI_LEVEL_Y (LCD_HEIGHT - UI_LEVEL_H - 2)

#define UI_LEFT_X 8
#define UI_RIGHT_X (LCD_WIDTH - 8)
