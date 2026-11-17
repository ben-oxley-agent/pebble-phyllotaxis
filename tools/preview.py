#!/usr/bin/env python3
"""Offline renderer for the Phyllotaxis watchface.

Parses the palette straight out of src/c/phyllotaxis.c so the preview can never
drift from the firmware. Renders the seed field at any minute of the day, for
any Pebble display size.

Usage:  python3 tools/preview.py --minute 767 --out renders/preview.png
        python3 tools/preview.py --grid 4 --out renders/day.png
        python3 tools/preview.py --variants --out renders/OPTIONS.png
"""
import argparse
import math
import os
import re

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "c", "phyllotaxis.c")

PLATFORMS = {
    "gabbro": (260, 260, True),
    "emery": (200, 228, False),
    "chalk": (180, 180, True),
    "basalt": (144, 168, False),
    "flint": (144, 168, False),
    "diorite": (144, 168, False),
    "aplite": (144, 168, False),
}
GOLDEN_ANGLE = 2.3999632297286535
MAX_SEEDS = 1440

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

# ---------------------------------------------------------------- palette

def load_consts():
    src = open(SRC).read()
    out = {}
    for name in ("HUE_START", "HUE_SAT", "VAL_NIGHT", "VAL_DAY", "SEED_MINUTES", "DAY_MINUTES"):
        m = re.search(r"#define\s+%s\s+(\d+)" % name, src)
        out[name] = int(m.group(1)) if m else None
    return out


C = load_consts()


def hue_rgb(hue, sat, val):
    """Mirror of hue_rgb() in the firmware."""
    hue %= 360
    f = (hue % 60) * 255 // 60
    p = val * (255 - sat) // 255
    q = val * (255 - sat * f // 255) // 255
    t = val * (255 - sat * (255 - f) // 255) // 255
    r = hue // 60
    return [(val, t, p), (q, val, p), (p, val, t),
            (p, q, val), (t, p, val), (val, p, q)][r]


def seed_rgb(minute, hue_start=None, sat=None, v_night=None, v_day=None):
    """Colour of the seed for `minute` past midnight, matching seed_color()."""
    hue_start = C["HUE_START"] if hue_start is None else hue_start
    sat = C["HUE_SAT"] if sat is None else sat
    v_night = C["VAL_NIGHT"] if v_night is None else v_night
    v_day = C["VAL_DAY"] if v_day is None else v_day
    t = minute % C["DAY_MINUTES"]
    hue = (hue_start + (360 * t) // C["DAY_MINUTES"]) % 360
    val = v_night + ((v_day - v_night) * int(255 * math.sin(math.pi * t / C["DAY_MINUTES"]))) // 255
    return tuple(int(v) for v in hue_rgb(hue, sat, val))


# ---------------------------------------------------------------- geometry

HUE = {}          # per-variant override of hue_start/sat/v_night/v_day

# Tunables (the firmware mirrors these)
HOLE = 0.315      # hub radius / face radius
FILL = 0.72       # seed diameter / lattice pitch
FULL = 1.0        # fraction of the day the field represents


def geometry(w, h, round_):
    cx, cy = w // 2, h // 2
    R = (w // 2) if round_ else (min(w, h) // 2)
    hole_r = int(R * HOLE)
    r_in = hole_r + 3
    r_out = R - 3
    n = int((C["DAY_MINUTES"] // C["SEED_MINUTES"]) * FULL)
    k = (r_out ** 2 - r_in ** 2) / (n - 1)
    pitch = math.sqrt(math.pi * k)
    return cx, cy, R, hole_r, r_in, k, pitch, n


def seed_diameter(pitch):
    d = int(pitch * FILL + 0.5)
    if d % 2 == 0:          # even diameters render lopsided; step to odd
        pass
    return max(2, min(9, d))


# ---------------------------------------------------------------- render

def render(platform="gabbro", minute=767, scale=3, show_time=True,
           steps_today=None, battery=74, bt=True):
    w, h, rnd = PLATFORMS[platform]
    W, H = w * scale, h * scale
    img = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy, R, hole_r, r_in, k, pitch, n_visible = geometry(w, h, rnd)

    n = min(minute // C["SEED_MINUTES"], n_visible)
    dia = seed_diameter(pitch) * scale

    for i in range(n):
        ang = i * GOLDEN_ANGLE
        rad = math.sqrt(r_in ** 2 + k * i)
        x = cx + rad * math.cos(ang)
        y = cy + rad * math.sin(ang)
        c = seed_rgb(i * C["SEED_MINUTES"], **HUE)
        if not bt:
            c = tuple(int(v * 3 / 5) for v in c)
        if battery <= 15:
            c = tuple(int(v * 4 / 5) for v in c)
        d.ellipse([(x * scale - dia / 2), (y * scale - dia / 2),
                   (x * scale + dia / 2), (y * scale + dia / 2)], fill=c)

    if show_time:
        # Clear the hub, exactly as the firmware does, so the inner edge is a
        # clean circle rather than a scallop of half-covered seeds.
        d.ellipse([(cx - hole_r) * scale, (cy - hole_r) * scale,
                   (cx + hole_r) * scale, (cy + hole_r) * scale], fill=(0, 0, 0))
        hh, mm = minute // 60, minute % 60
        big = f"{hh if hh else 12}" if hh < 13 else f"{hh:02d}"
        big = f"{hh:02d}"
        small_txt = f"{mm:02d}"
        fsize = int(hole_r * 0.60) * scale
        font = ImageFont.truetype(FONT, fsize)
        gold = (206, 184, 108)

        def centred(txt, ycentre):
            bb = d.textbbox((0, 0), txt, font=font)
            d.text((W / 2 - (bb[2] - bb[0]) / 2 - bb[0],
                    ycentre - (bb[3] - bb[1]) / 2 - bb[1]), txt,
                   font=font, fill=gold)

        centred(big, H / 2 - hole_r * 0.36 * scale)
        centred(small_txt, H / 2 + hole_r * 0.36 * scale)
        # hairline rule between the two lines
        half = hole_r * 0.46 * scale
        y = H / 2
        d.rectangle([W / 2 - half, y, W / 2 + half, y + max(1, scale // 2)],
                    fill=(150, 138, 90))

    return img


def label(im, text, scale=3):
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(FONT, int(11 * scale))
    bb = d.textbbox((0, 0), text, font=f)
    d.text(((im.width - (bb[2] - bb[0])) / 2, im.height - int(16 * scale)),
           text, font=f, fill=(120, 124, 136))
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", default="gabbro")
    ap.add_argument("--minute", type=int, default=767)
    ap.add_argument("--grid", type=int, default=0)
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--variants", action="store_true")
    ap.add_argument("--out", default="renders/preview.png")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    if args.variants:
        render_variants(args.out, args.scale)
        print("wrote", args.out)
        return
    if args.grid:
        times = [int(1440 * (i + 0.5) / args.grid) for i in range(args.grid)]
        imgs = [render(args.platform, m, scale=args.scale) for m in times]
        gap = 12 * args.scale
        W = sum(i.width for i in imgs) + gap * (len(imgs) - 1)
        out = Image.new("RGB", (W, imgs[0].height), (18, 18, 22))
        x = 0
        for im in imgs:
            out.paste(im, (x, 0))
            x += im.width + gap
        out.save(args.out)
    else:
        render(args.platform, args.minute, scale=args.scale).save(args.out)
    print("wrote", args.out)


VARIANTS = [
    ("A  uniform 4px  |  1440 seeds", dict(HOLE=0.315, FILL=0.72, FULL=1.0)),
    ("B  chunky 5px  |  1440 seeds", dict(HOLE=0.315, FILL=0.90, FULL=1.0)),
    ("C  airy 6px  |  720 seeds (1/2min)", dict(HOLE=0.315, FILL=0.74, FULL=0.5)),
    ("D  airy 6px  |  480 seeds (1/3min)", dict(HOLE=0.315, FILL=0.74, FULL=1 / 3)),
    ("E  small hub, dense rim", dict(HOLE=0.26, FILL=0.72, FULL=1.0)),
    ("F  big hub, 720 seeds", dict(HOLE=0.34, FILL=0.74, FULL=0.5)),
]


def render_variants(out_path, scale=2):
    global HOLE, FILL, FULL
    base = (HOLE, FILL, FULL)
    imgs = []
    for cap, cfg in VARIANTS:
        HOLE, FILL, FULL = cfg["HOLE"], cfg["FILL"], cfg["FULL"]
        im = render("gabbro", 767, scale=scale)
        imgs.append(label(im, cap, scale))
    HOLE, FILL, FULL = base
    gap = 10 * scale
    cols = 3
    rows = (len(imgs) + cols - 1) // cols
    cw, ch = imgs[0].width, imgs[0].height
    out = Image.new("RGB", (cols * cw + (cols - 1) * gap,
                            rows * ch + (rows - 1) * gap), (18, 18, 22))
    for i, im in enumerate(imgs):
        out.paste(im, ((i % cols) * (cw + gap), (i // cols) * (ch + gap)))
    out.save(out_path)


if __name__ == "__main__":
    main()