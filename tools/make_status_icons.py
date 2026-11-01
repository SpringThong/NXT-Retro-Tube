from PIL import Image, ImageDraw
import math, sys
OUT = sys.argv[1]
S = 768; k = S / 24.0; W = int(2 * k)          # Lucide: 24 grid, stroke 2
def new(): 
    im = Image.new('L', (S, S), 0); return im, ImageDraw.Draw(im)
def P(x, y): return (x * k, y * k)
def line(d, pts):
    for a, b in zip(pts, pts[1:]):
        d.line([P(*a), P(*b)], fill=255, width=W)
    for p in pts:
        x, y = P(*p); d.ellipse([x - W / 2, y - W / 2, x + W / 2, y + W / 2], fill=255)
def arc(d, cx, cy, r, a0, a1):
    n = 60; pts = [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]
    line(d, pts)
def dot(d, x, y, r=1.4):
    X, Y = P(x, y); R = r * k; d.ellipse([X - R, Y - R, X + R, Y + R], fill=255)
def save(im, name):
    a = im.resize((192, 192), Image.LANCZOS)
    out = Image.new('RGBA', (192, 192), (255, 255, 255, 0)); out.putalpha(a)
    out.save(f'{OUT}/{name}.png')
for bars in (1, 2, 3):
    im, d = new()
    dot(d, 12, 19.5)
    radii = [5.0, 9.4, 13.8]
    for i in range(bars): arc(d, 12, 20.2, radii[i], 225, 315)
    save(im, 'wifi' if bars == 3 else f'wifi-{bars}')
im, d = new(); line(d, [(7, 7), (17, 17), (12, 22), (12, 2), (17, 7), (7, 17)]); save(im, 'bluetooth')
im, d = new(); d.polygon([P(13.5, 2), P(4, 14), P(11, 14), P(10.5, 22), P(20, 10), P(13, 10)], fill=255); save(im, 'zap')
