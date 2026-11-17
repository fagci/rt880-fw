#!/usr/bin/env python3
"""Правки шрифтов для gen_fonts.sh.
  nodot  in.ttf out.ttf   — убрать точку внутри нуля (3-й контур 'zero')
  gamma  file.c G         — поднять покрытие 4bpp-глифов: a' = a^G (G<1),
                            края плотнее, текст не «бледнеет» от сглаживания
  snap   file.c LO HI     — пороговое сглаживание: a<LO -> 0, a>=HI -> 15,
                            между — масштабирование. Контур полностью
                            плотный, тонкий AA остаётся только на скруглениях
"""
import re, sys

def nodot(src, dst):
    from fontTools.ttLib import TTFont
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    f = TTFont(src)
    gs = f.getGlyphSet(); glyf = f['glyf']
    g = glyf['zero']
    if g.numberOfContours >= 3:
        end = g.endPtsOfContours
        keep = end[-2] + 1                 # точки без последнего контура
        from fontTools.ttLib.tables._g_l_y_f import GlyphCoordinates
        g.coordinates = GlyphCoordinates(list(g.coordinates)[:keep])
        g.flags = g.flags[:keep]
        g.endPtsOfContours = end[:-1]
        g.numberOfContours -= 1
        g.program.fromBytecode(b'')
    f.save(dst)

def remap(path, tbl):
    s = open(path).read()
    m = re.search(r'(glyph_bitmap\[\] = \{)(.*?)(\n\};)', s, re.S)
    # lv_font_conv пишет малые значения короткой формой (0x0, 0x9, 0xd…),
    # поэтому ловим токены любой длины, а не только двухзначные
    body = re.sub(r'0x[0-9a-fA-F]+',
        lambda x: '0x%02X' % (tbl[int(x.group(0), 16) >> 4] << 4 | tbl[int(x.group(0), 16) & 15]),
        m.group(2))
    open(path, 'w').write(s[:m.start(2)] + body + s[m.end(2):])

def gamma(path, gm):
    remap(path, [round(15 * (v / 15) ** gm) for v in range(16)])

def snap(path, lo, hi):
    tbl = []
    for v in range(16):
        if v < lo: tbl.append(0)
        elif v >= hi: tbl.append(15)
        else: tbl.append(round(15 * (v - lo) / (hi - lo)))
    remap(path, tbl)

if sys.argv[1] == 'nodot': nodot(sys.argv[2], sys.argv[3])
elif sys.argv[1] == 'snap': snap(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
else: gamma(sys.argv[2], float(sys.argv[3]))
