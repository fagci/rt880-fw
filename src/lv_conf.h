/* LVGL 8.3 — конфигурация под RT-880 (AT32F423, 246K flash, ~40K RAM).
 * Подключён через -DLV_CONF_INCLUDE_SIMPLE -Isrc (этот файл = src/lv_conf.h).
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ── Цвет ── */
#define LV_COLOR_DEPTH 16
/* ST7789 принимает RGB565 big-endian по SPI — LVGL будет хранить байты
   в переключённом порядке и flush шлёт буфер как есть. */
#define LV_COLOR_16_SWAP 1

/* ── Память: статический пул внутри LVGL ── */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (12 * 1024)
#define LV_MEM_ADR 0
#define LV_MEM_BUF_MAX_NUM 8

/* ── Ядро ── */
#define LV_DISP_DEF_REFR_PERIOD 33 /* ~30 fps */
/* Буфер dirty-областей: при переполнении LVGL перерисовывает ВЕСЬ экран
 * (~600 мс). 32 по умолчанию мало для смены активного VFO. */
#define LV_INV_BUF_SIZE 64
#define LV_INDEV_DEF_READ_PERIOD 30
#define LV_DPI_DEF 130
#define LV_USE_ANIM 0
#define LV_USE_SHADOW 0
#define LV_USE_BLEND_MODES 0
#define LV_USE_OPA_SCALE 0
#define LV_USE_GPU 0
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0
#define LV_USE_ASSERT_HANDLER(...)                                                \
  do {                                                                            \
  } while (0)
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_REFR_DEBUG 0
#define LV_SPRINTF_USE_FLOAT 0
#define LV_USE_USER_DATA 0
#define LV_ENABLE_GC 0
#define LV_USE_LARGE_COORD 0

/* ── Раскладки не нужны — всё позиционируем абсолютно ── */
#define LV_USE_FLEX 0
#define LV_USE_GRID 0

/* ── Текст ── */
#define LV_TXT_UTF8 0
#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/* ── Отрисовка: только прямоугольники и текст ── */
#define LV_DRAW_COMPLEX 0
#define LV_IMG_CACHE_DEF_SIZE 0
#define LV_USE_IMG_TRANSFORM 0
#define LV_USE_IMG_DECODER_BMP 0 /* не наш формат, но выключим явно */

/* ── Виджеты: только label (+ базовый obj) ── */
#define LV_USE_LABEL 1
#define LV_USE_ARC 0
#define LV_USE_BAR 0
#define LV_USE_BTN 0
#define LV_USE_BTNMATRIX 0
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_CONT 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMG 0
#define LV_USE_IMGBTN 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LINE 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_METER 0
#define LV_USE_MSGBOX 0
#define LV_USE_OBJMASK 0
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

#define LV_USE_GROUP 0
#define LV_USE_FILESYSTEM 0
#define LV_USE_THEME_DEFAULT 0 /* стили выставляем вручную */
#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO 0
#define LV_USE_SNAPSHOT 0
#define LV_USE_SYSMON 0
#define LV_USE_EXTRA 0

/* extra/* включается из core/lv_obj.c напрямую — глушим все опции extra,
   иначе их заголовки #error'ятся на выключенных зависимостях */
#define LV_USE_ANIMIMG 0
#define LV_USE_IMGFONT 0
#define LV_USE_OBSERVER 0
#define LV_USE_MONKEY 0
#define LV_USE_GRIDNAV 0
#define LV_USE_FRAGMENT 0
#define LV_USE_FFMPEG 0
#define LV_USE_PNG 0
#define LV_USE_GIF 0
#define LV_USE_QRCODE 0
#define LV_USE_FREETYPE 0
#define LV_USE_RLOTTIE 0
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_THORVG 0
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_MEMFS 0
#define LV_USE_FS_LITTLEFS 0

/* ── Шрифты: builtin выключены, используем сконвертированные из GFX ── */
#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_SUBPX 0
#define LV_USE_FONT_PLACEHOLDER 0
#define LV_FONT_DEFAULT &lv_font_mono9

#define LV_FONT_MONTSERRAT_8 0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0
#define LV_FONT_MONTSERRAT_12_SUBPX 0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK 0
#define LV_FONT_UNSCII_8 0
#define LV_FONT_UNSCII_16 0

/* LV_FONT_DEFAULT ссылается на наш шрифт — нужен extern везде,
   где lv_conf встречается раньше объявления. */
struct _lv_font_t;
typedef struct _lv_font_t lv_font_t;
extern const lv_font_t lv_font_mono9;

#endif /* LV_CONF_H */
