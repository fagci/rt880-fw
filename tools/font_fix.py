#!/usr/bin/env python3
"""Правки шрифтов для gen_fonts.sh.
  slashzero in.ttf out.ttf — заменить точку/перекладину в нуле на
                            диагональную черту (мелкий Cozette)
  nodot  in.ttf out.ttf   — убрать точку внутри нуля (JetBrains/Liberation)
  gamma  file.c G         — поднять покрытие 4bpp-глифов: a' = a^G (G<1),
                            края плотнее, текст не «бледнеет» от сглаживания
  snap   file.c LO HI     — пороговое сглаживание: a<LO -> 0, a>=HI -> 15,
                            между — масштабирование
"""
import math, re, sys

def _signed_area(pts):
    a = 0
    for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
        a += x0 * y1 - x1 * y0
    return a / 2

def nodot(src, dst):
    """Убрать точку внутри нуля: у JetBrains/Liberation это отдельный
    третий контур, поэтому просто отрезаем последний контур."""
    from fontTools.ttLib import TTFont
    from fontTools.ttLib.tables._g_l_y_f import GlyphCoordinates

    f = TTFont(src)
    glyf = f['glyf']
    g = glyf['zero']
    if g.numberOfContours >= 3:
        end = g.endPtsOfContours
        keep = end[-2] + 1
        g.coordinates = GlyphCoordinates(list(g.coordinates)[:keep])
        g.flags = g.flags[:keep]
        g.endPtsOfContours = end[:-1]
        g.numberOfContours -= 1
        g.program.fromBytecode(b'')
    f.save(dst)

def slashzero(src, dst):
    """Заменить точку/перекладину в нуле на диагональную черту.

    У CozetteVector regular ноль — кольцо с точкой (2 контура), у Bold —
    заполненный силуэт с двумя вырезами-счётчиками (3 контура). В обоих
    случаях оставляем внешний контур, при необходимости добавляем одно
    внутреннее отверстие и рисуем внутри диагональную перекладину."""
    from fontTools.ttLib import TTFont
    from fontTools.ttLib.tables._g_l_y_f import GlyphCoordinates

    f = TTFont(src)
    glyf = f['glyf']
    g = glyf['zero']
    if g.numberOfContours <= 0:
        raise SystemExit('slashzero: composite zero is not supported')

    coords = list(g.coordinates)
    contours = []
    start = 0
    for end in g.endPtsOfContours:
        contours.append(coords[start:end + 1])
        start = end + 1

    def bbox(c):
        xs = [p[0] for p in c]
        ys = [p[1] for p in c]
        return min(xs), min(ys), max(xs), max(ys)

    areas = []
    for c in contours:
        x0, y0, x1, y1 = bbox(c)
        areas.append((x1 - x0) * (y1 - y0))
    outer = contours[areas.index(max(areas))]
    x0, y0, x1, y1 = bbox(outer)

    ux = sorted(set(p[0] for p in outer))
    uy = sorted(set(p[1] for p in outer))
    ix0, ix1 = ux[1], ux[-2]
    iy0, iy1 = uy[1], uy[-2]
    iw, ih = ix1 - ix0, iy1 - iy0
    stroke = min(ix0 - x0, x1 - ix1, iy0 - y0, y1 - iy1)
    bar = max(1, round(stroke * 0.8))

    sx = ix0 + iw * 0.12
    sy = iy0 + ih * 0.12
    ex = ix1 - iw * 0.12
    ey = iy1 - ih * 0.12
    dx, dy = ex - sx, ey - sy
    ln = math.hypot(dx, dy)
    nx, ny = -dy / ln, dx / ln
    h = bar / 2

    slash = [(round(sx + nx * h), round(sy + ny * h)),
             (round(ex + nx * h), round(ey + ny * h)),
             (round(ex - nx * h), round(ey - ny * h)),
             (round(sx - nx * h), round(sy - ny * h))]
    if _signed_area(outer) * _signed_area(slash) < 0:
        slash.reverse()

    # Bold: вместо двух вырезов-счётчиков делаем одно внутреннее отверстие.
    hole = []
    if len(contours) == 3:
        hole = [(ix0, iy0), (ix1, iy0), (ix1, iy1), (ix0, iy1)]
        if _signed_area(outer) * _signed_area(hole) > 0:
            hole.reverse()

    newpts = list(outer) + hole + slash
    ends = [len(outer) - 1]
    if hole:
        ends.append(len(outer) + len(hole) - 1)
    ends.append(len(newpts) - 1)
    g.coordinates = GlyphCoordinates(newpts)
    g.flags = bytearray([1]) * len(newpts)
    g.endPtsOfContours = ends
    g.numberOfContours = len(ends)
    g.program.fromBytecode(b'')
    g.recalcBounds(glyf)
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

if __name__ == '__main__':
    op = sys.argv[1]
    if op == 'slashzero': slashzero(sys.argv[2], sys.argv[3])
    elif op == 'nodot': nodot(sys.argv[2], sys.argv[3])
    elif op == 'snap': snap(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
    else: gamma(sys.argv[2], float(sys.argv[3]))
