"""Converts the Half Eighties TrueType font (an 8x8 pixel font: 100 font units per pixel) into the C bitmap
table the launcher draws with. Standard library only.

    python program/scripts/font_to_c.py libs/half-eighties/HalfEighties.ttf libs/half-eighties/half_eighties.h

Each glyph is 8 rows of 8 pixels, top row first, bit 7 = leftmost pixel. Covers ASCII 32..126."""
import struct
import sys

PIXEL = 100     # font units per pixel
CELL = 8


class TTF:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        n = struct.unpack('>H', self.d[4:6])[0]
        self.t = {}
        for i in range(n):
            tag, _, off, ln = struct.unpack('>4sIII', self.d[12 + 16 * i:28 + 16 * i])
            self.t[tag.decode('latin1')] = (off, ln)
        h = self.t['head'][0]
        self.upem = struct.unpack('>H', self.d[h + 18:h + 20])[0]
        self.loca_long = struct.unpack('>h', self.d[h + 50:h + 52])[0] == 1
        hh = self.t['hhea'][0]
        self.ascent = struct.unpack('>h', self.d[hh + 4:hh + 6])[0]
        self.cmap = self._cmap()

    def name(self, nid):
        o = self.t['name'][0]
        _, cnt, so = struct.unpack('>HHH', self.d[o:o + 6])
        for i in range(cnt):
            pid, _, _, n, ln, off = struct.unpack('>HHHHHH', self.d[o + 6 + 12 * i:o + 18 + 12 * i])
            if n == nid:
                raw = self.d[o + so + off:o + so + off + ln]
                return raw.decode('utf-16-be') if pid in (0, 3) else raw.decode('latin1')
        return ''

    def _cmap(self):
        o = self.t['cmap'][0]
        for i in range(struct.unpack('>H', self.d[o + 2:o + 4])[0]):
            off = struct.unpack('>I', self.d[o + 8 + 8 * i:o + 12 + 8 * i])[0]
            so = o + off
            if struct.unpack('>H', self.d[so:so + 2])[0] != 4:
                continue
            segx2 = struct.unpack('>H', self.d[so + 6:so + 8])[0]
            seg = segx2 // 2
            ends = struct.unpack('>%dH' % seg, self.d[so + 14:so + 14 + segx2])
            starts = struct.unpack('>%dH' % seg, self.d[so + 16 + segx2:so + 16 + 2 * segx2])
            deltas = struct.unpack('>%dh' % seg, self.d[so + 16 + 2 * segx2:so + 16 + 3 * segx2])
            ro = so + 16 + 3 * segx2
            ranges = struct.unpack('>%dH' % seg, self.d[ro:ro + segx2])
            m = {}
            for k in range(seg):
                for c in range(starts[k], min(ends[k], 0xFFFE) + 1):
                    if ranges[k] == 0:
                        g = (c + deltas[k]) & 0xFFFF
                    else:
                        a = ro + 2 * k + ranges[k] + 2 * (c - starts[k])
                        g = struct.unpack('>H', self.d[a:a + 2])[0]
                        g = (g + deltas[k]) & 0xFFFF if g else 0
                    if g:
                        m[c] = g
            return m
        raise ValueError('no format-4 cmap')

    def contours(self, g):
        lo = self.t['loca'][0]
        if self.loca_long:
            a, b = struct.unpack('>II', self.d[lo + 4 * g:lo + 4 * g + 8])
        else:
            a, b = (2 * x for x in struct.unpack('>HH', self.d[lo + 2 * g:lo + 2 * g + 4]))
        if a == b:
            return []
        o = self.t['glyf'][0] + a
        nc = struct.unpack('>h', self.d[o:o + 2])[0]
        if nc < 0:      # composite: components placed at x/y offsets
            p, out = o + 10, []
            while True:
                fl, gi = struct.unpack('>HH', self.d[p:p + 4]); p += 4
                if fl & 1:
                    dx, dy = struct.unpack('>hh', self.d[p:p + 4]); p += 4
                else:
                    dx, dy = struct.unpack('>bb', self.d[p:p + 2]); p += 2
                if not fl & 2 or fl & (8 | 0x40 | 0x80):
                    raise ValueError('scaled composite in glyph %d' % g)
                out += [[(x + dx, y + dy, on) for x, y, on in ct] for ct in self.contours(gi)]
                if not fl & 0x20:
                    return out
        ends = struct.unpack('>%dH' % nc, self.d[o + 10:o + 10 + 2 * nc])
        npts = ends[-1] + 1
        p = o + 10 + 2 * nc
        p += 2 + struct.unpack('>H', self.d[p:p + 2])[0]
        flags = []
        while len(flags) < npts:
            f = self.d[p]; p += 1
            flags.append(f)
            if f & 8:
                flags += [f] * self.d[p]; p += 1
        def coords(short, same):
            nonlocal p
            v, out = 0, []
            for f in flags:
                if f & short:
                    v += self.d[p] if f & same else -self.d[p]; p += 1
                elif not f & same:
                    v += struct.unpack('>h', self.d[p:p + 2])[0]; p += 2
                out.append(v)
            return out
        xs, ys = coords(2, 16), coords(4, 32)
        cs, s = [], 0
        for e in ends:
            cs.append([(xs[i], ys[i], flags[i] & 1) for i in range(s, e + 1)])
            s = e + 1
        return cs


def raster(f, cs):
    """Non-zero winding at each pixel centre; the cell's top is the font's ascent."""
    rows = []
    for py in range(CELL):
        y = f.ascent - py * PIXEL - PIXEL // 2
        bits = 0
        for px in range(CELL):
            x = px * PIXEL + PIXEL // 2
            w = 0
            for ct in cs:
                if any(not on for _, _, on in ct):
                    raise ValueError('curved outline: not a pixel font')
                for i in range(len(ct)):
                    x0, y0, _ = ct[i - 1]
                    x1, y1, _ = ct[i]
                    if (y0 <= y) != (y1 <= y) and x0 + (y - y0) * (x1 - x0) / (y1 - y0) > x:
                        w += 1 if y1 > y0 else -1
            if w:
                bits |= 0x80 >> px
        rows.append(bits)
    return rows


def main():
    f = TTF(sys.argv[1])
    out = ['/* Half Eighties, converted to an 8x8 bitmap by program/scripts/font_to_c.py. Do not edit.',
           ' * ' + f.name(0).replace('©', '(c)').replace('*/', '* /'),
           ' * Font: ' + f.name(4) + ' ' + f.name(5) + '. Licence: SIL Open Font License 1.1 (OFL.txt in this folder).',
           ' * Glyphs for ASCII 32..126; 8 rows each, top first, bit 7 = leftmost pixel. */',
           '#ifndef SBPC_HALF_EIGHTIES_H', '#define SBPC_HALF_EIGHTIES_H', '',
           '#define HALF_EIGHTIES_FIRST 32', '#define HALF_EIGHTIES_LAST 126', '',
           'static const unsigned char half_eighties[HALF_EIGHTIES_LAST - HALF_EIGHTIES_FIRST + 1][8] = {']
    for c in range(32, 127):
        g = f.cmap.get(c)
        rows = raster(f, f.contours(g)) if g else [0] * 8
        ch = chr(c) if c not in (92,) else 'backslash'
        out.append('    {%s}, /* %s */' % (', '.join('0x%02X' % r for r in rows), ch))
    out += ['};', '', '#endif', '']
    open(sys.argv[2], 'w', newline='\n').write('\n'.join(out))


if __name__ == '__main__':
    main()
