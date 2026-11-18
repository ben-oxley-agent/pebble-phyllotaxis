// Phyllotaxis — a Pebble watchface.
//
// Every minute one "seed" is added to a golden-angle (137.507°) spiral:
//   radius(n) = sqrt(r_in^2 + k*n)      angle(n) = n * 137.507°
// The sqrt makes the seeds area-uniform across the annulus, which is what gives
// the packed seed-head look rather than a bright rim and a hollow middle.
//
// Colour is temporal: a seed takes its hue from the minute it represents, and
// the hue walks a full 360 degrees around the wheel between midnight and
// midnight. Because the spiral is laid down in time order, that turns the seed
// head into a clock face of colour -- blue at the centre (midnight), sweeping
// through green morning, gold midday and magenta evening, closing back on blue
// at the rim. Brightness follows the sun as well, so the small hours are dim.
// Seed size is uniform; mixed sizes turn the lattice into confetti.
//
// The hub carries the time as HH over MM with a hairline rule, mirroring the
// sunflower-head reference.

#include <pebble.h>
#include <math.h>

#define GOLDEN_ANGLE   2.3999632297286535f   // 137.50776405° in radians
#define PIF            3.14159265358979f
#define DAY_MINUTES    1440

// Seeds per day. One seed per minute is the original concept, but 1,440 seeds
// on a 260 px dial sit on a 5.6 px lattice, and the largest circle that leaves
// a black gap at that pitch is 4 px -- which rasterises as a plus, not a dot.
// One seed per two minutes halves the count, doubles the pitch to 8 px and
// lets real 6 px circles sit on it. Set to 1 for the denser original.
#ifndef SEED_MINUTES
#define SEED_MINUTES   2
#endif
#define MAX_SEEDS      (DAY_MINUTES / SEED_MINUTES)

#define MIL_HOLE       315                   // hub radius, per mille of face R
#define PCT_FILL       72                    // seed diameter, % of lattice pitch

typedef struct {
  GColor8 lo, hi;                        // the two nearest palette entries
  uint8_t d   : 4;                       // diameter in px
  uint8_t mix : 4;                       // 0..16 share of rows drawn in `hi`
} Seed;

// Hue of the day, as a half wheel: amber at midnight, green around noon, blue
// by the next midnight. Stopping at 180 degrees rather than a full 360 keeps the
// field inside one family of warm-to-cool tones; a full wheel walks through
// magenta and rose every evening and the seed head starts to look like confetti.
#define HUE_START   30
#define HUE_ARC    180                    // degrees walked between midnights
#define HUE_SAT    215                    // 0..255
#define VAL_NIGHT  110                    // brightness at midnight
#define VAL_DAY    255                    // brightness at midday

// 1 = halftone each blob between the two nearest palette entries, 0 = solid.
#ifndef DITHER
#define DITHER     0
#endif

static Seed     s_seeds[MAX_SEEDS];
static uint16_t s_n;

// ---------------------------------------------------------------- ui state

static Window  *s_win;
static Layer   *s_canvas;
static GFont    s_f_time;

static int      s_w, s_h, s_cx, s_cy, s_R;
static int      s_hole_r;
static float    s_r_in, s_r_out;
static float    s_k;
static int      s_dia = 4;
static int      s_line_h, s_gap;

static int      s_battery = 100;
static bool     s_bt = true;

// ---------------------------------------------------------------- colour

typedef struct { uint8_t r, g, b; } Rgb;

static Rgb rgb_of(int r, int g, int b) {
  Rgb c = { (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r),
            (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g),
            (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b) };
  return c;
}

// HSV -> RGB with hue in degrees. Only one sextant of the circle is active at
// a time, so this is a couple of multiplies and a switch.
static Rgb hue_rgb(int hue, int sat, int val) {
  hue %= 360;
  if (hue < 0) hue += 360;
  int f = (hue % 60) * 255 / 60;
  int p = val * (255 - sat) / 255;
  int q = val * (255 - sat * f / 255) / 255;
  int t = val * (255 - sat * (255 - f) / 255) / 255;
  switch (hue / 60) {
    case 0:  return rgb_of(val, t, p);
    case 1:  return rgb_of(q, val, p);
    case 2:  return rgb_of(p, val, t);
    case 3:  return rgb_of(p, q, val);
    case 4:  return rgb_of(t, p, val);
    default: return rgb_of(val, p, q);
  }
}

// ---------------------------------------------------------------- dithering
//
// The display has four levels per channel, so a hue ramp that moves 0.125 deg
// per seed hits the same palette entry for a dozen seeds in a row and the
// gradient steps in visible bands. Ordered dithering between the two nearest
// levels buys back roughly two bits per channel: each seed still draws clean,
// but neighbouring seeds alternate, and the eye averages the pairs.
//
// A 4x4 Bayer matrix keyed on the seed index gives a spatially scattered,
// repeatable pattern -- random thresholds would just look like noise, and a
// screen-space matrix would clump because phyllotaxis neighbours are far apart
// in index.
static const uint8_t BAYER4[16] = {
   0,  8,  2, 10,
  12,  4, 14,  6,
   3, 11,  1,  9,
  15,  7, 13,  5,
};

static GColor8 make_color(int r2, int g2, int b2) {
  if (r2 > 3) r2 = 3;
  if (g2 > 3) g2 = 3;
  if (b2 > 3) b2 = 3;
  return (GColor8) { .a = 3, .r = r2, .g = g2, .b = b2 };
}

// Colour of the seed representing `minute` past midnight, as the two palette
// entries either side of it plus the share of the blob that should take the
// upper one.
static void seed_color(int minute, GColor8 *lo, GColor8 *hi, uint8_t *mix) {
  int t = minute % DAY_MINUTES;
  // A half wheel: amber at midnight through green at noon to blue at midnight,
  // so the two ends of the day meet on a family of cool tones rather than
  // wrapping through magenta. See HUE_ARC.
  int hue = (HUE_START + (HUE_ARC * t) / DAY_MINUTES) % 360;
  // Sun curve: dark at 00:00 and 24:00, full brightness at noon.
  int val = VAL_NIGHT +
            ((VAL_DAY - VAL_NIGHT) * (int)(255.0f * sinf(PIF * (float)t / DAY_MINUTES))) / 255;
  Rgb c = hue_rgb(hue, HUE_SAT, val);
  if (!s_bt) c = rgb_of(c.r * 3 / 5, c.g * 3 / 5, c.b * 3 / 5);
  if (s_battery <= 15) c = rgb_of(c.r * 4 / 5, c.g * 4 / 5, c.b * 4 / 5);

  int lr = c.r / 85, lg = c.g / 85, lb = c.b / 85;
  int fr = c.r - lr * 85, fg = c.g - lg * 85, fb = c.b - lb * 85;
  *lo = make_color(lr, lg, lb);
  *hi = make_color(lr + 1, lg + 1, lb + 1);
  // Average the three channel fractions: one blob can only mix two colours,
  // so it aims for the same overall lift rather than a per-channel one.
  *mix = (uint8_t)(((fr + fg + fb) * 16) / (3 * 85 + 1));
}

// ---------------------------------------------------------------- geometry

// Half-width of each row of a filled seed. Below ~4 px a "circle" drawn from a
// span function collapses into a plus sign, so 3 px is a deliberate plus: at
// that size it reads rounder than any true circle the display can manage.
static const uint8_t ROWS_3[] = { 0, 1, 0 };          // plus
static const uint8_t ROWS_4[] = { 1, 2, 2, 1 };
static const uint8_t ROWS_5[] = { 1, 2, 2, 2, 1 };
static const uint8_t ROWS_7[] = { 2, 3, 3, 3, 3, 3, 2 };
static const uint8_t ROWS_9[] = { 3, 4, 4, 4, 4, 4, 4, 4, 3 };
static const uint8_t ROWS_8[] = { 1, 3, 3, 4, 4, 3, 3, 1 };
static const uint8_t ROWS_6[] = { 1, 2, 3, 3, 2, 1 };

static void gcolor_fill_disc(GContext *ctx, GPoint p, const uint8_t *rows,
                             int n, GColor8 c) {
  graphics_context_set_fill_color(ctx, c);
  int top = p.y - (n - 1) / 2;
  for (int i = 0; i < n; i++) {
    graphics_fill_rect(ctx, GRect(p.x - rows[i], top + i, 2 * rows[i] + 1, 1),
                       0, GCornerNone);
  }
}

// Draws one seed as a halftone of the two nearest palette entries: the disc is
// laid down in the lower colour, then a 4x4 ordered-dither mask punches the
// upper colour through it. The blob keeps its shape and its average colour is
// the one the gradient wanted, which is the extra two bits per channel the
// 64-colour display cannot otherwise give.
static void fill_seed(GContext *ctx, GPoint p, int d, GColor8 lo, GColor8 hi,
                      uint8_t mix) {
  const uint8_t *rows;
  int n;
  switch (d) {
    case 9:  rows = ROWS_9; n = 9; break;
    case 8:  rows = ROWS_8; n = 8; break;
    case 6:  rows = ROWS_6; n = 6; break;
    case 7:  rows = ROWS_7; n = 7; break;
    case 5:  rows = ROWS_5; n = 5; break;
    case 4:  rows = ROWS_4; n = 4; break;
    case 3:  rows = ROWS_3; n = 3; break;
    case 2:
      graphics_context_set_fill_color(ctx, mix >= 8 ? hi : lo);
      graphics_fill_rect(ctx, GRect(p.x - 1, p.y - 1, 2, 2), 0, GCornerNone);
      return;
    default:
      graphics_context_set_fill_color(ctx, mix >= 8 ? hi : lo);
      graphics_fill_rect(ctx, GRect(p.x, p.y, 1, 1), 0, GCornerNone);
      return;
  }

  if (mix <= 0) {
    gcolor_fill_disc(ctx, p, rows, n, lo);
    return;
  }
  if (mix >= 16) {
    gcolor_fill_disc(ctx, p, rows, n, hi);
    return;
  }

  gcolor_fill_disc(ctx, p, rows, n, lo);
  graphics_context_set_fill_color(ctx, hi);
  int top = p.y - (n - 1) / 2;
  for (int i = 0; i < n; i++) {
    int half = rows[i];
    for (int dx = -half; dx <= half; dx++) {
      int x = p.x + dx, y = top + i;
      if (BAYER4[(x & 3) + (y & 3) * 4] < mix) {
        graphics_fill_rect(ctx, GRect(x, y, 1, 1), 0, GCornerNone);
      }
    }
  }
}

static void recompute_geometry(GRect bounds) {
  s_w = bounds.size.w;
  s_h = bounds.size.h;
  s_cx = s_w / 2;
  s_cy = s_h / 2;
#if defined(PBL_ROUND)
  s_R = s_w / 2;
#else
  s_R = (s_w < s_h ? s_w : s_h) / 2;
#endif

  s_hole_r = (s_R * MIL_HOLE) / 1000;

  int r_in  = s_hole_r + 3;
  int r_out = s_R - 2;
  if (r_out <= r_in + 6) r_out = r_in + 6;

  s_r_in  = (float)r_in;
  s_r_out = (float)r_out;
  s_k = (s_r_out * s_r_out - s_r_in * s_r_in) / (float)(MAX_SEEDS - 1);

  float pitch = sqrtf(PIF * s_k);          // mean nearest-neighbour distance
  int d = (int)(pitch * (float)PCT_FILL / 100.0f + 0.5f);
  if (d < 2) d = 2;
  if (d > 9) d = 9;
  s_dia = d;

  // Time font: the largest variant whose two stacked lines fit the hub.
  static const char *cands[] = {
    FONT_KEY_LECO_28_LIGHT_NUMBERS,
    FONT_KEY_LECO_20_BOLD_NUMBERS,
    FONT_KEY_GOTHIC_18_BOLD,
    FONT_KEY_GOTHIC_14_BOLD,
  };
  s_f_time = fonts_get_system_font(cands[0]);
  for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
    GFont f = fonts_get_system_font(cands[i]);
    GSize sz = graphics_text_layout_get_content_size("88", f,
                                                     GRect(0, 0, s_w, s_h),
                                                     GTextOverflowModeFill,
                                                     GTextAlignmentCenter);
    if (sz.w * 10 <= s_hole_r * 13 && sz.h * 5 <= s_hole_r * 7) {
      s_f_time = f;
      s_line_h = sz.h;
      break;
    }
  }
  if (s_line_h == 0) s_line_h = 20;
  s_gap = s_hole_r * 12 / 100;             // half-spacing around the rule
}

// ---------------------------------------------------------------- seeds

static void push_seed(void) {
  if (s_n >= MAX_SEEDS) return;
  GColor8 lo, hi;
  uint8_t mix;
  seed_color((int)s_n * SEED_MINUTES, &lo, &hi, &mix);
  if (!DITHER) mix = 0;
  s_seeds[s_n].lo = lo;
  s_seeds[s_n].hi = hi;
  s_seeds[s_n].mix = mix;
  s_seeds[s_n].d = (uint8_t)s_dia;
  s_n++;
}

static void fill_to(int minute) {
  s_n = 0;
  int want = minute / SEED_MINUTES;
  if (want > MAX_SEEDS) want = MAX_SEEDS;
  for (int i = 0; i < want; i++) push_seed();
}

// ---------------------------------------------------------------- drawing

static void draw_field(GContext *ctx) {
  for (int i = 0; i < (int)s_n; i++) {
    float ang = i * GOLDEN_ANGLE;
    float rad = sqrtf(s_r_in * s_r_in + s_k * (float)i);
    GPoint p = GPoint(s_cx + (int)lrintf(rad * cosf(ang)),
                      s_cy + (int)lrintf(rad * sinf(ang)));
    fill_seed(ctx, p, s_seeds[i].d, s_seeds[i].lo, s_seeds[i].hi,
              s_seeds[i].mix);
  }
}

static void draw_centre(GContext *ctx, struct tm *t) {
  // Clean circular void: without it the inner edge is a scallop of half-drawn
  // seeds and the hub looks torn.
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, GPoint(s_cx, s_cy), s_hole_r);

  char top[4], bot[4];
  if (clock_is_24h_style()) {
    snprintf(top, sizeof(top), "%02d", t->tm_hour);
  } else {
    int h = t->tm_hour % 12;
    snprintf(top, sizeof(top), "%d", h == 0 ? 12 : h);
  }
  snprintf(bot, sizeof(bot), "%02d", t->tm_min);

  GColor8 ink = GColorFromRGB(206, 184, 108);
  graphics_context_set_text_color(ctx, ink);

  // Two boxes, not one multi-line string: a "\n" in a full-width box is laid
  // out as a space, and a box only as tall as the type makes Pebble crush the
  // glyphs against the edge. A box two lines tall aligns the text from the top
  // with a constant gutter, so position it by hand from that constant.
  int cap  = s_line_h * 70 / 100;          // glyph height
  int lead = s_line_h * 32 / 100;          // space above the glyphs in the box
  int g    = s_line_h * 14 / 100;          // half the gutter between the lines
  GRect a = GRect(0, s_cy - g - cap - lead, s_w, s_line_h * 2);
  GRect b = GRect(0, s_cy + g - lead, s_w, s_line_h * 2);
  graphics_draw_text(ctx, top, s_f_time, a, GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, bot, s_f_time, b, GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  // Rule sits in the gutter between the two lines, with even air each side.
  // A warm rule collapses into the same pixel colour as the digits under the
  // display's colour correction, so the rule is neutral grey: it has to read as
  // a divider, not as a third line of type.
  GSize sz = graphics_text_layout_get_content_size(top, s_f_time,
                                                   GRect(0, 0, s_w, s_h),
                                                   GTextOverflowModeFill,
                                                   GTextAlignmentCenter);
  int half = sz.w * 3 / 8;
  if (half < 12) half = 12;
  if (half > s_hole_r - 4) half = s_hole_r - 4;
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_rect(ctx, GRect(s_cx - half, s_cy, 2 * half, 1), 0,
                     GCornerNone);
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  time_t now = time(NULL);
  struct tm t = *localtime(&now);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

  // Ticks are not guaranteed after a time jump (emulator, phone sync, DST), so
  // the field is re-checked against the clock on every draw.
  int minute = t.tm_hour * 60 + t.tm_min;
  if (minute != (int)s_n) fill_to(minute);

  draw_field(ctx);
  draw_centre(ctx, &t);
}

// ---------------------------------------------------------------- services

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  layer_mark_dirty(s_canvas);
}

// Recolour the seeds already in the field without rebuilding the spiral.
static void recolour(void) {
  int save = s_n;
  fill_to(save);
}

static void battery_handler(BatteryChargeState s) {
  s_battery = s.charge_percent;
  recolour();
  layer_mark_dirty(s_canvas);
}

static void bt_handler(bool connected) {
  s_bt = connected;
  recolour();
  layer_mark_dirty(s_canvas);
}

// ---------------------------------------------------------------- lifecycle

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  recompute_geometry(bounds);
  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);

  s_battery = battery_state_service_peek().charge_percent;
  s_bt = bluetooth_connection_service_peek();

  time_t now = time(NULL);
  struct tm t = *localtime(&now);
  fill_to(t.tm_hour * 60 + t.tm_min);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);
  bluetooth_connection_service_subscribe(bt_handler);
}

static void window_unload(Window *window) {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  bluetooth_connection_service_unsubscribe();
  layer_destroy(s_canvas);
}

static void init(void) {
  s_win = window_create();
  window_set_background_color(s_win, GColorBlack);
  window_set_window_handlers(s_win, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_win, true);
}

static void deinit(void) {
  window_destroy(s_win);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}