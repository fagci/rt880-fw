#!/usr/bin/env python3
"""Правки шрифтов для gen_fonts.sh.
  nodot  in.ttf out.ttf   — убрать точку внутри нуля (3-й контур 'zero')
  gamma  file.c G         — поднять покрытие 4bpp-глифов: a' = a^G (G<1),
                            края плотнее, текст не «бледнеет» от сглаживания
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

def gamma(path, gm):
    s = open(path).read()
    tbl = [round(15 * (v / 15) ** gm) for v in range(16)]
    m = re.search(r'(glyph_bitmap\[\] = \{)(.*?)(\n\};)', s, re.S)
    body = re.sub(r'0x([0-9a-fA-F]{2})',
        lambda x: '0x%X%X' % (tbl[int(x.group(1), 16) >> 4], tbl[int(x.group(1), 16) & 15]),
        m.group(2))
    open(path, 'w').write(s[:m.start(2)] + body + s[m.end(2):])

if sys.argv[1] == 'nodot': nodot(sys.argv[2], sys.argv[3])
else: gamma(sys.argv[2], float(sys.argv[3]))
