#!/bin/sh
# Генерация LVGL-шрифтов интерфейса.
# Мелкий — Cozette 13 px / 1 bpp (MIT, tools/fonts/), совпадает с
# растровым cozette.bdf пиксель в пиксель. M/L/XL — JetBrains Mono
# (Medium/ExtraBold); L — ExtraBold 26 px для имён, XL — 38 px для цифр.
# Во всех шрифтах убран/заменён ноль: у Cozette — диагональная черта,
# у JetBrains — точка просто удаляется.
# Нужно: node + `npm i -g lv_font_conv` (1.5.3), fonts-jetbrains-mono.
# Запуск: sh tools/gen_fonts.sh   (результат — src/ui/lvgl_fonts/ui_font_*.c)
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT="$DIR/../src/ui/lvgl_fonts"
FONTS="$DIR/fonts"
JB=${JB_DIR:-/usr/share/fonts/truetype/jetbrains-mono}
if [ -f "$JB/JetBrainsMono-Medium.ttf" ]; then
  JB_MED=JetBrainsMono-Medium.ttf
  JB_XB=JetBrainsMono-ExtraBold.ttf
else
  JB=${JB_DIR:-$HOME/.fonts}
  JB_MED=JetBrainsMonoNerdFontMono-Medium.ttf
  JB_XB=JetBrainsMonoNerdFontMono-ExtraBold.ttf
fi

gen() { # имя файл-шрифта размер диапазон bpp подпись
  lv_font_conv --font "$2" -r "$4" --size "$3" --bpp "$5" \
    --format lvgl --no-compress --lv-font-name "$1" --lv-include lvgl.h \
    -o "$OUT/$1.c"
  # в конфиге LVGL LV_USE_USER_DATA=0 — поля user_data нет
  sed -i '/\.user_data = NULL,/d' "$OUT/$1.c"
  # стабильный заголовок вместо пути к временному файлу
  sed -i "/^ \* Opts:/c\\ * Opts: $6" "$OUT/$1.c"
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 "$DIR/font_fix.py" slashzero "$FONTS/CozetteVector.ttf" \
  "$TMP/CozetteVector-slash.ttf"
python3 "$DIR/font_fix.py" nodot "$JB/$JB_MED" "$TMP/jb-medium.ttf"
python3 "$DIR/font_fix.py" nodot "$JB/$JB_XB" "$TMP/jb-extrabold.ttf"

gen ui_font_s  "$TMP/CozetteVector-slash.ttf" 13 0x20-0x7E 1 \
  "Cozette 1.30.0 (tools/fonts), zero = slash"
gen ui_font_m  "$TMP/jb-medium.ttf"           14 0x20-0x7E 4 \
  "JetBrains Mono Medium 14 px, zero = nodot"
gen ui_font_l  "$TMP/jb-extrabold.ttf"        26 0x20-0x7E 4 \
  "JetBrains Mono ExtraBold 26 px, zero = nodot"
gen ui_font_xl "$TMP/jb-extrabold.ttf"        38 0x20,0x25,0x2B-0x3A 4 \
  "JetBrains Mono ExtraBold 38 px, zero = nodot"
