#!/usr/bin/env python3
"""Boot intro for bike-os: a 412x412 GIF that plays once at power-on.

    python3 tools/make_intro_gif.py [out.gif]      (default main/assets/intro.gif)

Any 412x412 (or smaller, it is centred) GIF can replace main/assets/intro.gif;
the firmware plays whatever is there once, then shows the home screen.

The look: a rev-counter ring sweeps round the rim into the red, speed streaks
run across, GIXXER slides in and settles, SUZUKI fades in above it, a glint
crosses the name, and it holds. Plain text in Montserrat (OFL), no logos.
"""
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W = 412
C = W / 2
FPS = 25
DUR = 3.0
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "main", "assets", "intro.gif")

ITALIC = "/usr/share/fonts/truetype/google-fonts/ofl/montserrat/Montserrat-Italic[wght].ttf"
UPRIGHT_LOCAL = os.path.join(HERE, "Montserrat-Medium.ttf")

BLUE = (30, 110, 255)
RED = (255, 45, 45)
WHITE = (245, 247, 250)
DIM = (70, 76, 86)


def font(path, size, weight=None):
    f = ImageFont.truetype(path, size)
    if weight is not None:
        try:
            f.set_variation_by_axes([weight])
        except Exception:
            pass
    return f


F_BIG = font(ITALIC, 72, 900)
F_SMALL = font(ITALIC, 30, 700)
F_TINY = font(ITALIC, 15, 600)


def clamp(v, a=0.0, b=1.0):
    return max(a, min(b, v))


def ramp(t, a, b):
    return clamp((t - a) / (b - a))


def ease_out(x):
    return 1 - (1 - x) ** 3


def ease_back(x):
    c = 1.9
    y = x - 1
    return 1 + (c + 1) * y ** 3 + c * y ** 2


def mix(a, b, k):
    return tuple(int(a[i] + (b[i] - a[i]) * k) for i in range(3))


def scale(col, k):
    return tuple(int(v * clamp(k)) for v in col)


def text_img(txt, f, col, spacing=0):
    """Text on a transparent layer, with optional letter spacing."""
    widths = [f.getbbox(ch)[2] - f.getbbox(ch)[0] for ch in txt]
    asc, desc = f.getmetrics()
    tw = sum(f.getlength(ch) for ch in txt) + spacing * (len(txt) - 1)
    im = Image.new("RGBA", (int(tw) + 30, asc + desc + 10), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x = 10
    for ch in txt:
        d.text((x, 4), ch, font=f, fill=col + (255,))
        x += f.getlength(ch) + spacing
    return im


GIX = text_img("GIXXER", F_BIG, WHITE, spacing=2)
SUZ = text_img("SUZUKI", F_SMALL, WHITE, spacing=10)
SUB = text_img("RIDE  READY", F_TINY, DIM, spacing=4)

# ring geometry: a rev counter from 7:30 round to 4:30 (270 degrees)
A0, SPAN = 135.0, 270.0
R_RING = 196

random.seed(7)
STREAKS = [(random.uniform(120, 300), random.uniform(40, 160), random.uniform(1.5, 3.5),
            random.uniform(0.0, 0.5)) for _ in range(22)]


def ring(d, t):
    sweep = ease_out(ramp(t, 0.05, 0.85))
    back = ramp(t, 0.85, 1.25)  # the needle falls back a little like a blip
    level = sweep - 0.25 * math.sin(math.pi * back) * (1 if t < 1.25 else 0)
    level = clamp(level)
    n = 54
    for i in range(n + 1):
        k = i / n
        a = math.radians(A0 + SPAN * k)
        on = k <= level + 1e-6
        major = i % 6 == 0
        r1 = R_RING - (16 if major else 9)
        col = mix(WHITE, RED, ramp(k, 0.72, 0.9)) if on else DIM
        if not on and t < 0.05:
            continue
        w = 4 if major else 2
        x0, y0 = C + r1 * math.cos(a), C + r1 * math.sin(a)
        x1, y1 = C + R_RING * math.cos(a), C + R_RING * math.sin(a)
        d.line((x0, y0, x1, y1), fill=col if on else scale(DIM, ramp(t, 0.0, 0.4)), width=w)
    # the sweep's bright arc just inside the ticks
    if level > 0.01:
        end = A0 + SPAN * level
        steps = max(2, int(SPAN * level / 3))
        for s in range(steps):
            k0 = s / steps
            k1 = (s + 1) / steps
            a0 = A0 + (end - A0) * k0
            a1 = A0 + (end - A0) * k1
            kk = (a1 - A0) / SPAN
            col = mix(BLUE, RED, ramp(kk, 0.7, 0.92))
            d.arc((C - R_RING - 8, C - R_RING - 8, C + R_RING + 8, C + R_RING + 8),
                  a0, a1 + 0.6, fill=col, width=5)


def streaks(d, t):
    k = ramp(t, 0.25, 1.15)
    if k <= 0 or k >= 1:
        return
    for y_off, length, w, delay in STREAKS:
        p = ramp(k, delay, delay + 0.5)
        if p <= 0 or p >= 1:
            continue
        x = W + 40 - (W + length + 80) * ease_out(p)
        y = C - 80 + y_off
        fade = math.sin(math.pi * p)
        d.line((x, y, x + length, y), fill=scale(mix(BLUE, WHITE, 0.3), 0.85 * fade), width=int(w))


def frame(t):
    im = Image.new("RGB", (W, W), (0, 0, 0))
    d = ImageDraw.Draw(im)
    ring(d, t)
    streaks(d, t)

    # GIXXER: slides in from the right, overshoots, settles; a blur trail on the way
    k = ramp(t, 0.55, 1.15)
    if k > 0:
        x_end = int(C - GIX.width / 2)
        x = int(x_end + (1 - ease_back(k)) * 330)
        y = int(C - GIX.height / 2 + 8)
        if k < 0.85:
            trail = GIX.copy()
            trail.putalpha(trail.getchannel("A").point(lambda v: int(v * 0.35 * (1 - k))))
            trail = trail.filter(ImageFilter.BoxBlur(6))
            for i in range(1, 4):
                im.paste(trail, (x + i * 18, y), trail)
        im.paste(GIX, (x, y), GIX)

    # a blue bar under the name grows out from the middle
    kb = ease_out(ramp(t, 1.05, 1.5))
    if kb > 0:
        half = int(104 * kb)
        yb = int(C + 46)
        d.polygon([(C - half + 6, yb), (C + half + 6, yb), (C + half, yb + 6), (C - half, yb + 6)], fill=BLUE)

    # SUZUKI fades in and the letter spacing closes up
    ks = ease_out(ramp(t, 1.15, 1.7))
    if ks > 0:
        sz = SUZ.copy()
        sz.putalpha(sz.getchannel("A").point(lambda v: int(v * ks)))
        x = int(C - sz.width / 2 + (1 - ks) * 30)
        im.paste(sz, (x, int(C - 84)), sz)

    kr = ramp(t, 1.7, 2.1)
    if kr > 0:
        sb = SUB.copy()
        sb.putalpha(sb.getchannel("A").point(lambda v: int(v * kr)))
        im.paste(sb, (int(C - sb.width / 2), int(C + 64)), sb)

    # glint: a bright slanted band crosses the name
    kg = ramp(t, 1.75, 2.35)
    if 0 < kg < 1:
        band = Image.new("L", (W, W), 0)
        bd = ImageDraw.Draw(band)
        gx = -60 + kg * (W + 120)
        bd.polygon([(gx, 0), (gx + 26, 0), (gx - 54, W), (gx - 80, W)], fill=140)
        band = band.filter(ImageFilter.GaussianBlur(6))
        mask = Image.new("L", (W, W), 0)
        mx = int(C - GIX.width / 2)
        mask.paste(GIX.getchannel("A"), (mx, int(C - GIX.height / 2 + 8)))
        from PIL import ImageChops
        m = ImageChops.multiply(band, mask)
        im.paste((255, 255, 255), (0, 0), m)

    # round glass: nothing outside the circle
    circle = Image.new("L", (W, W), 0)
    ImageDraw.Draw(circle).ellipse((0, 0, W - 1, W - 1), fill=255)
    black = Image.new("RGB", (W, W), (0, 0, 0))
    return Image.composite(im, black, circle)


def main():
    n = int(DUR * FPS)
    frames = [frame(i / FPS) for i in range(n)]
    # one shared palette from a few representative frames keeps colours steady
    # and lets the encoder store only the parts that change
    probe = Image.new("RGB", (W * 4, W))
    for j, t in enumerate([0.9, 1.3, 2.0, 2.9]):
        probe.paste(frame(t), (W * j, 0))
    pal = probe.quantize(colors=64, method=Image.Quantize.MEDIANCUT)
    q = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]
    durations = [int(1000 / FPS)] * n
    durations[-1] = 400  # hold the last picture a moment before home
    os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
    q[0].save(OUT, save_all=True, append_images=q[1:], duration=durations, loop=1,
              optimize=True, disposal=1)
    # loop=1 in Pillow writes NETSCAPE loop count 1 = play twice in some
    # viewers; strip the extension so it plays exactly once everywhere
    data = open(OUT, "rb").read()
    i = data.find(b"NETSCAPE2.0")
    if i > 0:
        data = data[:i - 3] + data[i + 16:]
        open(OUT, "wb").write(data)
    print(f"{OUT}: {n} frames, {len(data) / 1024:.0f} KB")
    # a preview strip for the README / review
    strip = Image.new("RGB", (W * 4, W))
    for j, t in enumerate([0.5, 1.0, 1.6, 2.9]):
        strip.paste(frame(t), (W * j, 0))
    strip.save(os.path.join(os.path.dirname(os.path.abspath(OUT)), "intro_preview.png"))


if __name__ == "__main__":
    main()
