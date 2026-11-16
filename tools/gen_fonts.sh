#!/bin/sh
# Генерация LVGL-шрифтов интерфейса из JetBrains Mono (OFL).
# Нужно: node + `npm i -g lv_font_conv`, шрифты (apt: fonts-jetbrains-mono).
# Одна гарнитура, строгая и технична: Medium для текста, ExtraBold для акцентов.
# Запуск: sh tools/gen_fonts.sh   (результат — src/ui/lvgl_fonts/ui_font_*.c)
set -e
F=${JB_DIR:-/usr/share/fonts/truetype/jetbrains-mono}
OUT=$(dirname "$0")/../src/ui/lvgl_fonts
gen() { # имя файл-шрифта размер диапазон
  lv_font_conv --font "$F/$2" -r "$4" --size "$3" --bpp 4 --format lvgl --no-compress \
    --lv-font-name "$1" --lv-include lvgl.h -o "$OUT/$1.c"
  # в конфиге LVGL LV_USE_USER_DATA=0 — поля user_data нет
  sed -i '/\.user_data = NULL,/d' "$OUT/$1.c"
}
gen ui_font_s  JetBrainsMono-Medium.ttf    11 0x20-0x7E
gen ui_font_m  JetBrainsMono-Medium.ttf    14 0x20-0x7E
gen ui_font_l  JetBrainsMono-ExtraBold.ttf 20 0x20-0x7E
gen ui_font_xl JetBrainsMono-ExtraBold.ttf 38 0x20,0x25,0x2B-0x3A   # цифры частоты
