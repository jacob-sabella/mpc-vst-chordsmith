#!/usr/bin/env python3
"""Chordsmith browser tile: bold wordmark + a keyboard with a lit chord (voicing ladder above it).

Draws once at 8x of 270x110, then downsamples (LANCZOS) to 540x220 and 270x110 so edges stay clean.
Run (host python has no Pillow):
  docker run --rm -u $(id -u):$(id -g) -v $PWD:/w -v ~/Projects/mpc-vst-nam/vst/fonts:/fonts:ro -w /w \
    python:3.11-slim sh -c 'pip -q install pillow --target /tmp/p && PYTHONPATH=/tmp/p python3 gen.py'
Font: Titillium Web SemiBold (SIL OFL 1.1). Override with FONT=/path/to.ttf.
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

FONT = os.environ.get("FONT", "/fonts/TitilliumWeb-SemiBold.ttf")
BASE_W, BASE_H = 270, 110
S = 8                           # master scale over 270x110 (2160x880)
W, H = BASE_W * S, BASE_H * S
STROKE = int(0.45 * S)          # faux-bold: SemiBold is the heaviest Titillium we ship

BG = (0x12, 0x0d, 0x24)
PANEL = (0x21, 0x18, 0x43)
TEAL = (0x2a, 0xd4, 0xc0)
PINK = (0xff, 0x2b, 0xd6)
PURPLE = (0xa6, 0x4d, 0xff)
INK = (0xf2, 0xf5, 0xfa)
MUTED = (0xb9, 0xa3, 0xe0)


def u(v):
    """270x110 units -> master pixels."""
    return int(round(v * S))


def lerp(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def background():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    # soft vertical gradient: panel colour at the top fading to the base navy
    for y in range(H):
        d.line([(0, y), (W, y)], fill=lerp(PANEL, BG, y / (H - 1)))
    # faint purple glow behind the keyboard (right side)
    glow = Image.new("L", (W, H), 0)
    ImageDraw.Draw(glow).ellipse([u(150), u(-10), u(300), u(120)], fill=70)
    glow = glow.filter(ImageFilter.GaussianBlur(u(28)))
    img = Image.composite(Image.new("RGB", (W, H), lerp(BG, PURPLE, 0.45)), img, glow)
    return img


def keyboard(img):
    """One octave of keys (C to C) at the right, C-E-G-C voicing lit, ladder of dots above."""
    d = ImageDraw.Draw(img)
    x0, kw, top, bot = 180.0, 10.5, 50.0, 98.0
    n_white = 8                                   # C D E F G A B C
    lit = {0: PINK, 2: TEAL, 4: TEAL, 7: TEAL}   # white-key index -> colour (root pink)
    r = u(1.6)
    for i in range(n_white):
        x = x0 + i * kw
        fill = lit.get(i, (0xdc, 0xe2, 0xec))
        d.rounded_rectangle([u(x + 0.6), u(top), u(x + kw - 0.6), u(bot)], radius=r, fill=fill)
    # black keys after C D, F G A (white index 0,1,3,4,5,7)
    for i in (0, 1, 3, 4, 5):
        x = x0 + (i + 1) * kw - 3.4
        d.rounded_rectangle([u(x), u(top - 0.5), u(x + 6.8), u(top + 28)], radius=u(1.2), fill=(0x10, 0x15, 0x22))
    # voicing ladder: one dot per chord tone, stepping up, joined by a thin rail
    pts = []
    for step, (i, col) in enumerate(sorted(lit.items())):
        cx = x0 + i * kw + kw / 2
        cy = 40.0 - step * 8.0
        pts.append((cx, cy, col))
    d.line([(u(p[0]), u(p[1])) for p in pts], fill=lerp(BG, PURPLE, 0.75), width=u(1.4), joint="curve")
    for cx, cy, col in pts:
        d.line([(u(cx), u(cy)), (u(cx), u(top - 1))], fill=lerp(BG, col, 0.35), width=u(0.8))
    for cx, cy, col in pts:
        rr = 3.6
        d.ellipse([u(cx - rr - 1.2), u(cy - rr - 1.2), u(cx + rr + 1.2), u(cy + rr + 1.2)], fill=BG)
        d.ellipse([u(cx - rr), u(cy - rr), u(cx + rr), u(cy + rr)], fill=col)


def wordmark(img):
    d = ImageDraw.Draw(img)
    text, max_w = "CHORDSMITH", 154.0
    size = u(40)
    while True:
        f = ImageFont.truetype(FONT, size)
        l, t, rgt, b = d.textbbox((0, 0), text, font=f, stroke_width=STROKE)
        if rgt - l + 2 * STROKE <= u(max_w):
            break
        size -= 2
    x, y_base = u(12), u(64)                     # cap baseline
    ascent = f.getmetrics()[0]
    pos = (x - l, y_base - ascent)
    # subtle drop shadow for contrast over the gradient
    sh = Image.new("L", (W, H), 0)
    ImageDraw.Draw(sh).text((pos[0], pos[1] + u(1.2)), text, font=f, fill=200, stroke_width=STROKE, stroke_fill=200)
    sh = sh.filter(ImageFilter.GaussianBlur(u(1.5)))
    img.paste(Image.new("RGB", (W, H), (4, 6, 10)), (0, 0), sh)
    d = ImageDraw.Draw(img)
    d.text(pos, text, font=f, fill=INK, stroke_width=STROKE, stroke_fill=INK)
    bb = d.textbbox(pos, text, font=f, stroke_width=STROKE)
    # accent bar under the wordmark: teal with an amber lead, like the root + chord tones
    yb = bb[3] + u(5)
    d.rounded_rectangle([bb[0], yb, bb[0] + u(14), yb + u(3)], radius=u(1.5), fill=PINK)
    d.rounded_rectangle([bb[0] + u(17), yb, bb[0] + u(48), yb + u(3)], radius=u(1.5), fill=TEAL)
    return bb


def main():
    img = background()
    keyboard(img)
    wordmark(img)
    for mult in (2, 1):
        size = (BASE_W * mult, BASE_H * mult)
        out = img.resize(size, Image.LANCZOS)
        name = "chordsmith_tile_%dx%d.png" % size
        out.save(name, optimize=True)
        print("wrote", name)


if __name__ == "__main__":
    main()
