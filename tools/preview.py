#!/usr/bin/env python3
"""Offline renderer for the Phyllotaxis watchface.

Parses PALETTE / constants straight out of src/c/phyllotaxis.c so the preview
can never drift from the firmware. Renders the seed field at any minute of the
day, for any Pebble display size.

Usage:  python3 tools/preview.py --minute 1164 --out renders/preview.png
        python3 tools/preview.py --grid 4 --out renders/day.png
"""
import argparse
import math
import re
import os
import sys

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


def load_palette():
    src = open(SRC).read()
    body = re.search(r"static const Stop PALETTE\[\] = \{(.*?)\};", src, re.S).group(1)
    stops = []
    for m in re.finditer(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", body):
        stops.append(tuple(int(g) for g in m.groups()))
    return stops


PALETTE = load_palette()


def phase_color(minute, palette=None):
    palette = palette or PALETTE
    for i in range(len(palette) - 1):
        a, b = palette[i], palette[i + 1]
        if a[0] <= minute < b[0]:
            span = b[0] - a[0]
            t = (minute - a[0]) / span if span else 0
            return tuple(a[k + 1] + (b[k + 1] - a[k + 1]) * t for k in (0, 1, 2))
    return palette[0][1:]


def geometry(w, h, round_):
    cx, cy = w // 2, h // 2
    R = (w // 2) if round_ else (min(w, h) // 2)
    # time font: biggest that leaves room for ~50% of the face
    hole_r = int(R * 0.48)
    r_in = hole_r + 6
    r_out = R - 9
    k = (r_out ** 2 - r_in ** 2) / (MAX_SEEDS - 1)
    pitch = math.sqrt(math.pi * k)
    return cx, cy, R, hole_r, r_in, k, pitch


def radius_for(steps, pitch):
    u = pitch
    if steps < 1:
        r = u * 0.34
    elif steps < 10:
        r = u * 0.44
    elif steps < 40:
        r = u * 0.55
    else:
        r = u * 0.68
    return max(1, min(4, int(r + 0.5)))


def brightness_for(steps):
    if steps < 1:
        return 150
    if steps < 10:
        return 200
    if steps < 40:
        return 235
    return 268


def clamp(v, lo=0, hi=255):
    return max(lo, min(hi, int(v)))


def render(platform="gabbro", minute=1164, steps_today=None, battery=74,
           bt=True, scale=3, show_time=True, activity=None):
    w, h, rnd = PLATFORMS[platform]
    W, H = w * scale, h * scale
    img = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy, R, hole_r, r_in, k, pitch = geometry(w, h, rnd)

    if steps_today is None:
        steps_today = int(minute * 7)
    avg = steps_today / minute if minute else 0
    # Simulated per-minute activity: morning/evening commute bursts.
    def steps_for(m):
        if activity is None:
            return avg
        return activity(m)

    for i in range(minute):
        ang = i * GOLDEN_ANGLE
        rad = math.sqrt(r_in ** 2 + k * i)
        x = cx + rad * math.cos(ang)
        y = cy + rad * math.sin(ang)
        st = steps_for(i)
        c = phase_color(i)
        br = brightness_for(st)
        age = minute - i
        if age < 90:
            br2 = 256 + (90 - age) * 2
        else:
            br2 = 256
        c = tuple(clamp(v * br / 256 * br2 / 256) for v in c)
        if not bt:
            c = tuple(clamp(v * 3 / 5) for v in c)
        if battery <= 15:
            c = tuple(clamp(v * 4 / 5) for v in c)
        r = radius_for(st, pitch)
        d.ellipse([(x - r) * scale, (y - r) * scale, (x + r) * scale, (y + r) * scale], fill=c)

    # gauges
    if rnd:
        ring = R - 4
        prog = min(1.0, steps_today / 10000)
        rw = max(1, round(1.5 * scale))
        d.arc([(cx - ring) * scale, (cy - ring) * scale, (cx + ring) * scale, (cy + ring) * scale],
              -90, -90 + 360 * prog, fill=(255, 170, 85), width=rw)
        br_ = hole_r + 2
        bc = (255, 92, 70) if battery < 20 else (236, 176, 120)
        d.arc([(cx - br_) * scale, (cy - br_) * scale, (cx + br_) * scale, (cy + br_) * scale],
              -90, -90 + 360 * battery / 100, fill=bc if bt else (70, 70, 80), width=max(1, scale))

    if show_time:
        hh, mm = minute // 60, minute % 60
        txt = f"{hh:02d}:{mm:02d}"
        fsize = int(hole_r * 0.62) * scale
        try:
            font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", fsize)
        except Exception:
            font = ImageFont.load_default()
        bb = d.textbbox((0, 0), txt, font=font)
        d.text(((W - (bb[2] - bb[0])) / 2 - bb[0], (H - (bb[3] - bb[1])) / 2 - bb[1] - 8 * scale),
               txt, font=font, fill=(255, 255, 255))
        days = ["MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"]
        small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                   int(hole_r * 0.24) * scale)
        s = f"{days[(minute // 1440) % 7]} 17"
        sb = d.textbbox((0, 0), s, font=small)
        d.text(((W - (sb[2] - sb[0])) / 2 - sb[0], H / 2 + hole_r * 0.42 * scale),
               s, font=small, fill=(150, 156, 176))

    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", default="gabbro")
    ap.add_argument("--minute", type=int, default=1164)
    ap.add_argument("--grid", type=int, default=0, help="render N times of day side by side")
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--out", default="renders/preview.png")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
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


if __name__ == "__main__":
    main()