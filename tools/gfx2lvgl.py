#!/usr/bin/env python3
"""Конвертирует Adafruit GFXfont (.h) в LVGL 8.3 lv_font_fmt_txt (.c/.h).

Метрики GFX -> LVGL (по формуле из lv_draw_sw_letter.c):
  baseline = label_y + (line_height - base_line)
  glyph_top = baseline + yOffset  =>  ofs_y = -box_h - yOffset
adv_w = xAdvance * 16 (8.4 fixed point).
Битмапы копируются как есть: у GFX и LVGL PLAIN это непрерывный
поток бит MSB-first, строки глифа не выравниваются по байтам.
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src" / "ui"
OUT = SRC / "lvgl_fonts"

# (входной заголовок, имя C-идентификатора)
FONTS = [
    ("fonts/TomThumb.h", "lv_font_tomthumb"),
    ("fonts/FreeMono9pt7b.h", "lv_font_mono9"),
    ("fonts/FreeMono12pt7b.h", "lv_font_mono12"),
    ("fonts/FreeMono18pt7b.h", "lv_font_mono18"),
]


def parse_gfxfont(path: Path):
    text = path.read_text()

    m = re.search(
        r"const\s+GFXfont\s+(\w+)\s*(?:\w+\s*)?=\s*\{[^}]*\}", text, re.S
    )
    font_name = m.group(1)

    # Битмап-массив
    bm = re.search(
        r"const\s+uint8_t\s+\w+Bitmaps\[\]\s*(?:\w+\s*)?=\s*\{(.*?)\};", text, re.S
    )
    bitmap = [
        int(x, 0)
        for x in re.findall(r"0[xX][0-9a-fA-F]+|\d+", bm.group(1))
    ]

    # Глифы: {bitmapOffset, width, height, xAdvance, xOffset, yOffset}
    glyphs = [
        tuple(int(v.strip(), 0) for v in g.split(","))
        for g in re.findall(
            r"\{\s*([^{}]+?)\s*\}",
            re.search(
                r"const\s+GFXglyph\s+\w+Glyphs\[\]\s*(?:\w+\s*)?=\s*\{(.*?)\};",
                text,
                re.S,
            ).group(1),
        )
    ]

    # first/last/yAdvance — последние три числа в инициализаторе GFXfont
    init = m.group(0)
    nums = [int(x, 0) for x in re.findall(r"0[xX][0-9a-fA-F]+|\d+", init)]
    first, last, y_advance = nums[-3], nums[-2], nums[-1]

    assert len(glyphs) == last - first + 1, (path, len(glyphs), first, last)
    return font_name, bitmap, glyphs, first, last, y_advance


def convert(src_name: Path, out_name: str):
    name, bitmap, glyphs, first, last, y_advance = parse_gfxfont(src_name)

    # Метрики строки: ascent = -min(yOffset), descent = max(yOffset+h-1)
    hs = [(yo, yo + h - 1) for (_, w, h, _, _, yo) in glyphs if h > 0]
    ascent = -min(yo for yo, _ in hs)
    descent = max(yb for _, yb in hs)
    line_height = ascent + descent + 1
    base_line = line_height - ascent  # descent + 1

    out_bitmap = bytearray()
    out_bitmap += b"\x00"  # dummy-глиф: в LVGL gid 0 означает «не найден»,
    out_glyphs = [(0, 0, 0, 0, 0, 0)]  # поэтому пробел должен быть не 0-м
    for (off, w, h, adv, xo, yo) in glyphs:
        nbytes = (w * h + 7) // 8
        assert off + nbytes <= len(bitmap)
        idx = len(out_bitmap)
        out_bitmap += bytes(bitmap[off : off + nbytes])
        ofs_y = -h - yo
        assert -128 <= ofs_y <= 127, (name, off, ofs_y)
        assert adv * 16 < 4096
        out_glyphs.append((idx, adv * 16, w, h, xo, ofs_y))
    # trailing-dummy: у LVGL проверка rcp > range_length (не >=), поэтому
    # символ last+1 (0x7F) попадал бы на gid за пределами массива
    out_glyphs.append((0, 0, 0, 0, 0, 0))

    c = OUT / f"{out_name}.c"
    guard = out_name.upper()
    h_file = f"""#ifndef {guard}_H
#define {guard}_H

/* Forward declaration — нельзя включать lvgl.h из заголовков шрифтов:
   lv_conf.h подключается внутри lvgl.h, получилась бы циклическая зависимость. */
#ifdef __cplusplus
extern "C" {{
#endif

struct _lv_font_t;
typedef struct _lv_font_t lv_font_t;

extern const lv_font_t {out_name};

#ifdef __cplusplus
}}
#endif

#endif
"""
    (OUT / f"{out_name}.h").write_text(h_file)

    glyph_lines = ",\n    ".join(
        "{ .bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, "
        ".ofs_x = %d, .ofs_y = %d }" % g
        for g in out_glyphs
    )
    bm_lines = ", ".join(f"0x{b:02X}" for b in out_bitmap)

    c.write_text(
        f'''/* Автогенерировано tools/gfx2lvgl.py из {src_name.name}. Не править руками. */
#include "{out_name}.h"
#include "../../external/lvgl/src/font/lv_font_fmt_txt.h"

static const uint8_t glyph_bitmap[] = {{
    {bm_lines}
}};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {{
    {glyph_lines}
}};

static const lv_font_fmt_txt_cmap_t cmaps[] = {{
    {{
        .range_start = {first},
        .range_length = {last - first + 2}, /* +1: см. trailing-dummy выше */
        .glyph_id_start = 1, /* 0-й — dummy, в LVGL gid 0 = «не найден» */
        .unicode_list = NULL,
        .glyph_id_ofs_list = NULL,
        .list_length = 0,
        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY,
    }},
}};

static lv_font_fmt_txt_glyph_cache_t cache;

static const lv_font_fmt_txt_dsc_t font_dsc = {{
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,
    .cache = &cache,
}};

const lv_font_t {out_name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_height},
    .base_line = {base_line},
    .subpx = 0,
    .underline_position = -1,
    .underline_thickness = 1,
    .dsc = &font_dsc,
    .fallback = NULL,
}};
'''
    )
    print(f"{out_name}: glyphs={len(glyphs)} bitmap={len(out_bitmap)}B "
          f"line_h={line_height} base={base_line} ascent={ascent}")


def main():
    OUT.mkdir(exist_ok=True)
    for src, out_name in FONTS:
        convert(SRC / src, out_name)


if __name__ == "__main__":
    main()
