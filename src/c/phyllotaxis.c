// Phyllotaxis — a Pebble watchface.
//
// Every minute one "seed" is added to a golden-angle (137.507°) spiral.
// radius(n) = r0 + c*sqrt(n)   angle(n) = n * 137.507°
// Seed colour follows the time of day through a dawn→day→dusk→night palette;
// seed size and brightness follow the steps you took during that minute;
// crossing each 1,000-step milestone stamps a bright accent seed.
// The outer ring is step-goal progress, the inner ring is battery.

#include <pebble.h>
#include <math.h>

#define GOLDEN_ANGLE     2.3999632297286535f   // 137.50776405° in radians
#define PIF              3.14159265358979f
#define MAX_SEEDS        1440                  // one minute per day
#define STEP_GOAL        10000
#define MILESTONE        1000

// ---------------------------------------------------------------- data model

typedef struct {
  GColor8 c;
  uint8_t r;
} Seed;

typedef struct {
  uint16_t min;         // minutes since midnight
  uint8_t  r, g, b;
} Stop;

// Colour stops sit on the 4-level-per-channel grid the display can actually
// show (0/85/170/255), so what we ask for is what gets drawn.
static const Stop PALETTE[] = {
  {    0,   0,   0,  85 },   // 00:00  navy
  {  270,  85,   0, 170 },   // 04:30  indigo
  {  390, 170,   0, 170 },   // 06:30  magenta dawn
  {  510, 255,  85, 170 },   // 08:30  rose
  {  720, 255, 170,  85 },   // 12:00  amber
  {  900, 255, 255,  85 },   // 15:00  gold
  { 1080, 255, 170,  85 },   // 18:00  amber dusk
  { 1200, 255,  85,  85 },   // 20:00  coral
  { 1320, 170,   0, 170 },   // 22:00  magenta
  { 1440,   0,   0,  85 },   // 24:00  wrap
};
#define N_STOPS ((int)(sizeof(PALETTE) / sizeof(PALETTE[0])))

static Seed     s_seeds[MAX_SEEDS];
static uint16_t s_n;                 // seeds currently in the field

// ---------------------------------------------------------------- ui state

static Window  *s_win;
static Layer   *s_canvas;
static GFont    s_f_time, s_f_info;

static int      s_w, s_h, s_cx, s_cy, s_R;
static int      s_hole_r;
static float    s_r_in, s_r_out;     // radial extent of the seed field
static float    s_k;                 // r(n) = sqrt(r_in^2 + k*n)
static float    s_pitch;             // mean nearest-neighbour distance
static GRect    s_time_box;

static uint32_t s_steps, s_steps_prev, s_next_milestone;
static int      s_battery = 100;
static bool     s_bt = true;

// ---------------------------------------------------------------- colour utils

typedef struct { uint8_t r, g, b; } Rgb;

static Rgb rgb_of(int r, int g, int b) {
  Rgb c = { (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r),
            (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g),
            (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b) };
  return c;
}

static Rgb rgb_lerp(Rgb a, Rgb b, int t) {         // t: 0..256
  return rgb_of(a.r + ((b.r - a.r) * t) / 256,
                a.g + ((b.g - a.g) * t) / 256,
                a.b + ((b.b - a.b) * t) / 256);
}

static Rgb rgb_scale(Rgb c, int num, int den) {
  return rgb_of(c.r * num / den, c.g * num / den, c.b * num / den);
}

static GColor8 rgb_pack(Rgb c) {
  return GColorFromRGB(c.r, c.g, c.b);
}

// Colour of the day phase at `minute` (0..1439).
static Rgb phase_color(int minute) {
  for (int i = 0; i < N_STOPS - 1; i++) {
    if (minute >= PALETTE[i].min && minute < PALETTE[i + 1].min) {
      int span = PALETTE[i + 1].min - PALETTE[i].min;
      int t = span > 0 ? ((minute - PALETTE[i].min) * 256) / span : 0;
      Rgb a = rgb_of(PALETTE[i].r, PALETTE[i].g, PALETTE[i].b);
      Rgb b = rgb_of(PALETTE[i + 1].r, PALETTE[i + 1].g, PALETTE[i + 1].b);
      return rgb_lerp(a, b, t);
    }
  }
  return rgb_of(PALETTE[0].r, PALETTE[0].g, PALETTE[0].b);
}

// ---------------------------------------------------------------- geometry

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

  // Pick the largest time font that still leaves a decent hole.
  static const char *cands[] = {
    FONT_KEY_BITHAM_42_BOLD,
    FONT_KEY_LECO_38_BOLD_NUMBERS,
    FONT_KEY_BITHAM_34_MEDIUM_NUMBERS,
    FONT_KEY_BITHAM_30_BLACK,
    FONT_KEY_GOTHIC_28_BOLD,
    FONT_KEY_GOTHIC_24_BOLD,
  };
  int best_w = 0;
  s_f_time = fonts_get_system_font(cands[0]);
  for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
    GFont f = fonts_get_system_font(cands[i]);
    GRect box = GRect(0, 0, s_w, 60);
    GSize sz = graphics_text_layout_get_content_size("23:59", f, box,
                                                     GTextOverflowModeFill,
                                                     GTextAlignmentCenter);
    if (sz.w <= (int)(s_R * 1.15f)) {    // leave room for the seed field
      s_f_time = f;
      best_w = sz.w;
      break;
    }
  }
  if (best_w == 0) best_w = (int)(s_R * 1.4f);

  s_hole_r = best_w / 2 + 8;
  if (s_hole_r > (int)(s_R * 0.62f)) s_hole_r = (int)(s_R * 0.62f);
  if (s_hole_r < 24) s_hole_r = 24;

  int r_in  = s_hole_r + 6;
  int r_out = s_R - 7;                   // leave room for the step ring
  if (r_out <= r_in + 4) r_out = r_in + 4;

  // Area-preserving phyllotaxis: r(n) = sqrt(r_in^2 + k*n) keeps the seed
  // density uniform across the annulus (a plain r0 + c*sqrt(n) crams
  // everything against the inner edge).
  s_r_in  = (float)r_in;
  s_r_out = (float)r_out;
  s_k = (s_r_out * s_r_out - s_r_in * s_r_in) / (float)(MAX_SEEDS - 1);
  s_pitch = sqrtf(PIF * s_k);    // sqrt(area per seed)

  GSize tsz = graphics_text_layout_get_content_size("23:59", s_f_time,
                                                    GRect(0, 0, s_w, 60),
                                                    GTextOverflowModeFill,
                                                    GTextAlignmentCenter);
  s_time_box = GRect(0, s_cy - tsz.h / 2 - 7, s_w, tsz.h + 4);
}

// ---------------------------------------------------------------- seeds

// Seed *diameter* in pixels for `steps` taken in one minute. Most minutes are
// quiet, so most seeds stay small and leave black space around themselves;
// movement is what makes a seed fat.
static uint8_t size_for(float steps) {
  float want;
  if (steps < 1.0f)       want = 2.6f;
  else if (steps < 10.0f) want = 3.4f;
  else if (steps < 40.0f) want = 4.4f;
  else                    want = 6.0f;
  // Scale to the room actually available: small screens must not overlap.
  want *= s_pitch / 4.6f;
  int d = (int)(want + 0.5f);
  if (d < 1) d = 1;
  if (d > 7) d = 7;
  if (d == 6) d = 5;                      // 6 has no shape table
  return (uint8_t)d;
}

// Brightness of a seed representing `steps` taken in one minute.
static int brightness_for(float steps) {
  if (steps < 1.0f)       return 200;    // idle minute
  if (steps < 10.0f)      return 220;
  if (steps < 40.0f)      return 240;
  return 255;
}

static void push_seed(int seed_minute, int now_minute, float steps,
                      bool milestone) {
  if (s_n >= MAX_SEEDS) return;
  Rgb c;
  if (milestone) {
    c = rgb_of(255, 244, 190);
  } else {
    c = phase_color(seed_minute);
    c = rgb_scale(c, brightness_for(steps), 256);
  }
  // The freshest seeds glow: the leading edge of the spiral is "now".
  int age = now_minute - seed_minute;     // 0 for the seed being added
  if (age < 30) c = rgb_scale(c, 256 + (30 - age) * 3, 256);
  if (!s_bt) c = rgb_scale(c, 3, 5);
  if (s_battery <= 15) c = rgb_scale(c, 4, 5);
  s_seeds[s_n].c = rgb_pack(c);
  s_seeds[s_n].r = size_for(steps);
  if (milestone && s_seeds[s_n].r < 5) s_seeds[s_n].r = 5;
  s_n++;
}

static void fill_field_to(int minute, uint32_t steps_today) {
  s_n = 0;
  if (minute <= 0) return;
  float avg = (float)steps_today / (float)minute;
  for (int i = 0; i < minute; i++) push_seed(i, minute, avg, false);
}

// ---------------------------------------------------------------- drawing

// Half-width of each row of a filled circle, r = 1..4.
// Hand-tuned: a sqrt() span or a rounded rect both come out as plus signs at
// these sizes, which turns the spiral into a lattice.
// Seed shapes, by diameter. Below 5px any "circle" is really a plus sign, so
// the small sizes are deliberate square dots.
static const uint8_t ROWS_3[] = { 1, 1, 1 };
static const uint8_t ROWS_4[] = { 1, 2, 2, 1 };
static const uint8_t ROWS_5[] = { 1, 2, 2, 2, 1 };
static const uint8_t ROWS_7[] = { 2, 3, 3, 3, 3, 3, 2 };

static void fill_disc(GContext *ctx, GPoint p, int d) {
  const uint8_t *rows;
  int n;
  switch (d) {
    case 7:  rows = ROWS_7; n = 7; break;
    case 5:  rows = ROWS_5; n = 5; break;
    case 4:  rows = ROWS_4; n = 4; break;
    case 3:  rows = ROWS_3; n = 3; break;
    case 2:                       // 2x2 block
      graphics_fill_rect(ctx, GRect(p.x - 1, p.y - 1, 2, 2), 0, GCornerNone);
      return;
    default:
      graphics_fill_rect(ctx, GRect(p.x, p.y, 1, 1), 0, GCornerNone);
      return;
  }
  int top = p.y - (n - 1) / 2;
  for (int i = 0; i < n; i++) {
    int half = rows[i];
    graphics_fill_rect(ctx, GRect(p.x - half, top + i, 2 * half + 1, 1), 0,
                       GCornerNone);
  }
}

static void draw_field(GContext *ctx) {
  for (int i = 0; i < (int)s_n; i++) {
    float ang = i * GOLDEN_ANGLE;
    float rad = sqrtf(s_r_in * s_r_in + s_k * (float)i);
    GPoint p = GPoint(s_cx + (int)lrintf(rad * cosf(ang)),
                      s_cy + (int)lrintf(rad * sinf(ang)));
    graphics_context_set_fill_color(ctx, s_seeds[i].c);
    fill_disc(ctx, p, s_seeds[i].r);
  }
}

static void draw_gauges(GContext *ctx) {
  if (!s_bt) return;
  float prog = (float)s_steps / (float)STEP_GOAL;
  if (prog > 1.0f) prog = 1.0f;
  GColor8 step_col = rgb_pack(rgb_scale(phase_color(s_n), 230, 256));

#if !defined(PBL_ROUND)
  // Rect screens: step bar along the bottom, battery along the top.
  graphics_context_set_fill_color(ctx, GColorFromRGB(32, 34, 44));
  graphics_fill_rect(ctx, GRect(6, s_h - 8, s_w - 12, 4), 2, GCornersAll);
  graphics_fill_rect(ctx, GRect(6, 4, s_w - 12, 3), 1, GCornersAll);
  if (prog > 0.001f) {
    graphics_context_set_fill_color(ctx, step_col);
    graphics_fill_rect(ctx, GRect(6, s_h - 8, (int)((s_w - 12) * prog), 4),
                       2, GCornersAll);
  }
  if (s_battery > 0) {
    graphics_context_set_fill_color(ctx,
        s_battery < 20 ? GColorFromRGB(255, 92, 70) : GColorFromRGB(236, 176, 120));
    graphics_fill_rect(ctx, GRect(6, 4, ((s_w - 12) * s_battery) / 100, 3), 1,
                       GCornersAll);
  }
#else
  (void)ctx; (void)prog; (void)step_col;
#endif
}

static void draw_centre(GContext *ctx, struct tm *t) {
  // Clear the hub so the time is always legible.
  graphics_context_set_fill_color(ctx, GColorBlack);
  fill_disc(ctx, GPoint(s_cx, s_cy), s_hole_r - 1);

  static char buf[8];
  if (clock_is_24h_style()) {
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
  } else {
    int h = t->tm_hour % 12;
    if (h == 0) h = 12;
    snprintf(buf, sizeof(buf), "%d:%02d", h, t->tm_min);
  }
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, buf, s_f_time, s_time_box,
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  static char info[20];
  static const char *days[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
  snprintf(info, sizeof(info), "%s %d  %d%%", days[t->tm_wday % 7], t->tm_mday,
           s_battery);
  GRect ibox = GRect(0, s_time_box.origin.y + s_time_box.size.h - 4,
                     s_w, 20);
  graphics_context_set_text_color(ctx, GColorFromRGB(150, 156, 176));
  graphics_draw_text(ctx, info, s_f_info, ibox,
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);

#if defined(PBL_ROUND)
  // Step-goal progress as a slim bar under the date: keeps the spiral clean.
  int barw = (s_hole_r * 2) - 16;
  if (barw > 20) {
    int by = ibox.origin.y + 18;
    int bx = s_cx - barw / 2;
    float prog = (float)s_steps / (float)STEP_GOAL;
    if (prog > 1.0f) prog = 1.0f;
    graphics_context_set_fill_color(ctx, GColorFromRGB(38, 40, 52));
    graphics_fill_rect(ctx, GRect(bx, by, barw, 3), 1, GCornersAll);
    if (prog > 0.001f && s_bt) {
      graphics_context_set_fill_color(ctx,
          rgb_pack(rgb_scale(phase_color(s_n), 230, 256)));
      graphics_fill_rect(ctx, GRect(bx, by, (int)(barw * prog), 3), 1,
                         GCornersAll);
    }
  }
#endif
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  time_t now = time(NULL);
  struct tm t = *localtime(&now);

  draw_field(ctx);
  draw_gauges(ctx);
  draw_centre(ctx, &t);
}

// ---------------------------------------------------------------- services

static void refresh_steps(void) {
#ifdef PBL_HEALTH
  s_steps = (uint32_t)health_service_sum_today(HealthMetricStepCount);
#else
  s_steps = 0;
#endif
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  int minute = tick_time->tm_hour * 60 + tick_time->tm_min;
  uint32_t prev = s_steps;
  refresh_steps();

  if (minute == 0 && s_n > 0) {           // new day: start blank
    s_n = 0;
    s_next_milestone = MILESTONE;
  }

  if (minute != (int)s_n) {
    if (minute < (int)s_n) {
      // Clock moved backwards (DST / sync): rebuild the field.
      fill_field_to(minute, s_steps);
      s_next_milestone = (s_steps / MILESTONE + 1) * MILESTONE;
    } else {
      uint32_t delta = s_steps >= prev ? s_steps - prev : 0;
      while ((int)s_n < minute) {
        bool milestone = s_steps >= s_next_milestone;
        push_seed((int)s_n, minute, (float)delta, milestone);
        if (milestone) s_next_milestone += MILESTONE;
      }
    }
  }
  layer_mark_dirty(s_canvas);
}

static void battery_handler(BatteryChargeState s) {
  s_battery = s.charge_percent;
  layer_mark_dirty(s_canvas);
}

static void bt_handler(bool connected) {
  s_bt = connected;
  layer_mark_dirty(s_canvas);
}

#ifdef PBL_HEALTH
static void health_handler(HealthEventType type, void *context) {
  if (type == HealthEventMovementUpdate || type == HealthEventSignificantUpdate) {
    refresh_steps();
    if (s_canvas) layer_mark_dirty(s_canvas);
  }
}
#endif

// ---------------------------------------------------------------- lifecycle

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  recompute_geometry(bounds);
  s_f_info = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);

  refresh_steps();
  s_steps_prev = s_steps;
  s_next_milestone = (s_steps / MILESTONE + 1) * MILESTONE;

  BatteryChargeState bs = battery_state_service_peek();
  s_battery = bs.charge_percent;
  s_bt = bluetooth_connection_service_peek();

  time_t now = time(NULL);
  struct tm t = *localtime(&now);
  fill_field_to(t.tm_hour * 60 + t.tm_min, s_steps);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);
  bluetooth_connection_service_subscribe(bt_handler);
#ifdef PBL_HEALTH
  health_service_events_subscribe(health_handler, NULL);
#endif
}

static void window_unload(Window *window) {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  bluetooth_connection_service_unsubscribe();
#ifdef PBL_HEALTH
  health_service_events_unsubscribe();
#endif
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