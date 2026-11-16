#!/usr/bin/env python3
"""The six NXT colour icons for launchers with a coloured background (Stock firmware, …).

Source: the 256x256 colour PNGs of NXT Icon Repair (icons/color). Each glyph is centred on a transparent
256x256 canvas at 70 % of its size (the room other apps' icons leave), with a thin dark edge around the
shape so white and blue parts stay readable on blue, light or themed backgrounds.

    python3 tools/icons/make_nxt_icons.py <icons/color dir> <out dir>
"""
import sys
from PIL import Image, ImageFilter

NAMES = ['menu_icon_nxt', 'menu_icon_nxt_tube', 'menu_icon_nxt_retro_movie', 'menu_icon_nxt_booknest', 'menu_icon_nxt_bricktok', 'menu_icon_nxt_file']
S, CONTENT = 256, 0.70

def make(src):
    im = Image.open(src).convert('RGBA')
    im = im.crop(im.getbbox())
    k = S * CONTENT / max(im.size)
    g = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
    t = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    t.alpha_composite(g, ((S - g.width) // 2, (S - g.height) // 2))
    edge = t.split()[3].filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(2.2))
    out = Image.new('RGBA', (S, S), (12, 12, 18, 255))
    out.putalpha(edge.point(lambda v: min(255, int(v * 0.9))))
    out.alpha_composite(t)
    return out

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    for n in NAMES:
        make('%s/%s.png' % (src, n)).save('%s/%s.png' % (dst, n), optimize=True)
        print(n)
