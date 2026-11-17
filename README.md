# Phyllotaxis — a Pebble watchface

A golden-angle seed spiral that grows one seed per minute. At midnight the dial
is blank; by the end of the day it is a dense sunflower head that doubles as a
record of your day.

```
radius(n) = sqrt(r_in² + k·n)      angle(n) = n · 137.50776405°
```

`k` is chosen so 1,440 seeds fill the annulus with *uniform* area density.
Naive `r = r0 + c·√n` crams every seed against the inner edge (the offset breaks
the uniform-density property of the spiral), which is why the first build looked
like mush at the hub.

## What the seeds encode

| Property | Meaning |
| --- | --- |
| Position | minute of the day (index in the spiral) |
| Colour | that minute's slot in the day palette — navy night → magenta dawn → rose → amber → gold → coral dusk → magenta → navy |
| Size | steps taken during that minute: 3 px idle, 4 px light, 5 px active, 7 px moving |
| Glow | the last 30 minutes are boosted, so the leading edge is "now" |
| Extra | crossing each 1,000-step milestone stamps a bright accent seed |

Palette stops sit on the 4-level-per-channel grid the display can actually
render (0/85/170/255) so the requested colour is the drawn colour.

## Layout

* Centre hub: `HH:MM`, weekday + date + battery %, and a slim step-goal bar.
* Seeds occupy the annulus between the hub and the screen edge.
* Rect platforms: step bar along the bottom edge, battery along the top.
* Bluetooth disconnected: field desaturates, gauges go grey.
* Battery ≤ 15 %: whole field dims.

## Build

```bash
pebble sdk install latest     # once
cd pebble-phyllotaxis
pebble build                  # -> build/pebble-phyllotaxis.pbw
```

Targets: `gabbro` (Round 2, 260×260), `emery` (Time 2), `chalk` (Time Round,
180×180), `basalt`, `diorite`, `flint`, `aplite`.

## Run it in the emulator

```bash
pebble install --emulator gabbro dist/phyllotaxis.pbw
pebble emu-set-time --emulator gabbro 19:24:00   # watch the field grow
pebble screenshot --emulator gabbro --no-open shot.png
```

`emu-set-time` only takes `HH:MM:SS` or unix seconds — a date string is rejected.

## Offline preview renderer

`tools/preview.py` parses `PALETTE` straight out of `src/c/phyllotaxis.c`, so
previews cannot drift from the firmware.

```bash
python3 tools/preview.py --minute 1164 --scale 3 --out renders/one.png
python3 tools/preview.py --grid 4 --out renders/day.png
```

## Notes / gotchas found the hard way

* `graphics_draw_pixel` uses the **stroke** colour, not the fill colour — 1 px
  seeds drawn with it are invisible on black. Use a 1×1 `graphics_fill_rect`.
* A rounded rect with corner radius = half its side renders as a `+` at 3–5 px.
  Explicit row tables are the only way to get round seeds.
* `graphics_draw_arc(ctx, rect, GOvalScaleModeFillCircle, start, end)` — rect
  comes *before* the scale mode. And the angles are `int32_t`: casting to
  `int16_t` overflows above 50 % of a full turn.
* `M_PI` is not defined by the Pebble toolchain's `math.h`.