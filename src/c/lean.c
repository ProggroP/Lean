// Lean -- grosse Ziffern wie bei Ebb, aber ohne Ueberblendung und mit
// einstellbarem Anzeigewinkel. Wer den Arm nicht so weit drehen kann, dass
// die Uhr gerade vor dem Auge liegt, dreht stattdessen die Zeit.
//
// Pebble kann Text nicht gedreht zeichnen. Deshalb wird die Zeit zuerst ganz
// normal gezeichnet, danach aus dem Framebuffer in eine 1-Bit-Maske gelesen
// und dabei geloescht, und zum Schluss gedreht wieder hineingeschrieben. Jedes
// Zielpixel tastet die Maske an vier Stellen ab (2x2-Supersampling), daraus
// ergibt sich eine Mischfarbe zwischen Ziffer und Hintergrund -- die Kanten
// bleiben so auch bei schiefen Winkeln glatt.
//
// Gedreht wird um die Mitte des Ziffernblocks, die dabei auf die
// Displaymitte faellt. Passt der gedrehte Block nicht mehr aufs Display (nur
// auf Emery um 45 Grad herum knapp moeglich), wird er beim Drehen ein wenig
// verkleinert.
//
// Gezeichnet wird nur einmal pro Minute und bei geaenderten Einstellungen.
//
// Nur Emery und Gabbro: die 104er-LECO braucht das 512-Byte-Glyph-Limit, und
// der Framebuffer ist auf beiden 8 Bit je Pixel.

#include <pebble.h>

// --------------------------------------------------------------- Diagnose

// Nur fuer Store-Screenshots: haelt die Anzeige auf 10:08. pebble
// emu-set-time greift in dieser Umgebung nicht. Vor dem Release wieder
// auskommentieren.
// #define LEAN_DEMO_TIME

// --------------------------------------------------------------- Farben

#define COLOR_BG_DEFAULT   GColorBlack
#define COLOR_FG_DEFAULT   GColorWhite

// --------------------------------------------------------------- Layout

// Ungedrehte Lage wie bei Ebb, relativ zur Displaymitte. Die genaue Lage ist
// unkritisch: gedreht wird ohnehin um die Mitte der tatsaechlich gezeichneten
// Pixel. Sie muss nur vollstaendig aufs Display passen, damit nichts
// abgeschnitten in die Maske geraet.
#define DIGIT_H      80
#define TEXT_LIFT    22
#define HOUR_TOP    -84     // Oberkante der Stundenzeile relativ zur Mitte
#define MIN_TOP       4     // Oberkante der Minutenzeile relativ zur Mitte

// Mindestabstand des gedrehten Blocks zum Displayrand
#if defined(PBL_PLATFORM_EMERY)
  #define FIT_MARGIN  4
#else                                      // gabbro
  #define FIT_MARGIN  8
#endif

// Groesster Framebuffer der Zielplattformen (gabbro 260 x 260)
#define FB_MAX_W    260
#define FB_MAX_H    260

// --------------------------------------------------------------- Einstellungen

#define WRIST_LEFT     0
#define WRIST_RIGHT    1

#define TILT_DEFAULT  20
#define TILT_MAX      90

#define PKEY_COLOR_BG   101
#define PKEY_COLOR_FG   102
#define PKEY_WRIST      103
#define PKEY_TILT       104
#define PKEY_FLIP       105

static GColor s_color_bg;
static GColor s_color_fg;
static int    s_wrist = WRIST_LEFT;
static int    s_tilt  = TILT_DEFAULT;
static bool   s_flip  = false;

// --------------------------------------------------------------- Zustand

static Window *s_window;
static Layer  *s_canvas;
static GFont   s_digit_font;
static int     s_ref_w;      // Breite von "00", fuer eine stabile Skalierung

static char s_hour_text[4] = "";
static char s_min_text[4]  = "";

// Ziffernmaske, 1 Bit je Pixel, Zeilenlaenge = Framebufferbreite
static uint8_t s_mask[(FB_MAX_W * FB_MAX_H + 7) / 8];

// --------------------------------------------------------------- Zeittext

static void update_time_text(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
#ifdef LEAN_DEMO_TIME
  struct tm demo = *t;
  demo.tm_hour = 10;
  demo.tm_min = 8;
  t = &demo;
#endif
  // Klemmung gegen -Wformat-truncation, die Werte liegen ohnehin im Bereich
  int hour = t->tm_hour % 24;
  int min  = t->tm_min % 60;
  if (hour < 0) hour = 0;
  if (min < 0) min = 0;

  if (!clock_is_24h_style()) {
    hour = hour % 12;
    if (hour == 0) hour = 12;
    snprintf(s_hour_text, sizeof(s_hour_text), "%d", hour);
  } else {
    snprintf(s_hour_text, sizeof(s_hour_text), "%02d", hour);
  }
  snprintf(s_min_text, sizeof(s_min_text), "%02d", min);
}

// --------------------------------------------------------------- Drehung

// Drehwinkel in Grad, im Uhrzeigersinn positiv. Am linken Handgelenk liegt
// die Uhr beim Hinsehen gegen den Uhrzeigersinn verdreht, die Zeit muss also
// im Uhrzeigersinn gegensteuern -- rechts genau umgekehrt.
static int rotation_deg(void) {
  int deg = (s_wrist == WRIST_LEFT) ? s_tilt : -s_tilt;
  if (s_flip) deg += 180;
  return ((deg % 360) + 360) % 360;
}

static uint32_t isqrt(uint32_t n) {
  uint32_t root = 0;
  uint32_t bit = 1u << 30;
  while (bit > n) bit >>= 2;
  while (bit) {
    if (n >= root + bit) {
      n -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

// Skalierung (16.16, hoechstens 1.0), mit der ein w x h grosser Block um den
// Winkel mit |cos| = ac und |sin| = as noch aufs Display passt.
static int32_t fit_scale(int w, int h, int32_t ac, int32_t as,
                         int fb_w, int fb_h) {
  int64_t k = 1 << 16;
#if defined(PBL_ROUND)
  // Auf dem runden Display zaehlt nur die Diagonale, der Winkel ist egal
  (void)ac; (void)as; (void)fb_h;
  int64_t need = isqrt((uint32_t)(w * w + h * h));
  int64_t avail = fb_w - 2 * FIT_MARGIN;
  if (need > avail) k = (avail << 16) / need;
#else
  int64_t need_w = ((int64_t)w * ac + (int64_t)h * as) >> 16;
  int64_t need_h = ((int64_t)w * as + (int64_t)h * ac) >> 16;
  int64_t avail_w = fb_w - 2 * FIT_MARGIN;
  int64_t avail_h = fb_h - 2 * FIT_MARGIN;
  if (need_w > avail_w) k = (avail_w << 16) / need_w;
  if (need_h > avail_h && (avail_h << 16) / need_h < k) k = (avail_h << 16) / need_h;
#endif
  return (int32_t)k;
}

// Mischt zwei Farben kanalweise, cov von 0 (Hintergrund) bis 4 (Ziffer)
static uint8_t blend(uint8_t fg, uint8_t bg, int cov) {
  uint8_t out = 0xC0;                      // voll deckend
  for (int shift = 0; shift <= 4; shift += 2) {
    int f = (fg >> shift) & 3;
    int b = (bg >> shift) & 3;
    int v = (f * cov + b * (4 - cov) + 2) / 4;
    out |= (uint8_t)(v << shift);
  }
  return out;
}

static inline int mask_get(int x, int y, int w, int h) {
  if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return 0;
  int i = y * w + x;
  return (s_mask[i >> 3] >> (i & 7)) & 1;
}

// Liest alle Ziffernpixel in die Maske, loescht sie im Framebuffer und
// schreibt sie gedreht zurueck.
static void rotate_digits(GContext *ctx) {
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;

  GRect fb_bounds = gbitmap_get_bounds(fb);
  int w = fb_bounds.size.w;
  int h = fb_bounds.size.h;
  if (w > FB_MAX_W || h > FB_MAX_H) {
    graphics_release_frame_buffer(ctx, fb);
    return;
  }
  uint8_t bg = s_color_bg.argb;

  // 1. Maske fuellen, Ziffern loeschen, Ausdehnung merken
  memset(s_mask, 0, (w * h + 7) / 8);
  int min_x = w, max_x = -1, min_y = h, max_y = -1;
  for (int y = 0; y < h; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    for (int x = row.min_x; x <= row.max_x; x++) {
      if (row.data[x] == bg) continue;
      int i = y * w + x;
      s_mask[i >> 3] |= (uint8_t)(1 << (i & 7));
      row.data[x] = bg;
      if (x < min_x) min_x = x;
      if (x > max_x) max_x = x;
      if (y < min_y) min_y = y;
      if (y > max_y) max_y = y;
    }
  }
  if (max_x < 0) {
    graphics_release_frame_buffer(ctx, fb);
    return;
  }

  int ink_w = max_x - min_x + 1;
  int ink_h = max_y - min_y + 1;

  // Ganzzahlige Mittelpunkte: bei 0, 90 und 180 Grad treffen die Abtastpunkte
  // dann genau die Quellpixel, und die Kanten bleiben scharf.
  int src_cx = (min_x + max_x + 1) / 2;
  int src_cy = (min_y + max_y + 1) / 2;
  int dst_cx = w / 2;
  int dst_cy = h / 2;

  int32_t angle = DEG_TO_TRIGANGLE(rotation_deg());
  int32_t cs = cos_lookup(angle);
  int32_t sn = sin_lookup(angle);

  int block_w = (ink_w > s_ref_w) ? ink_w : s_ref_w;
  int32_t k = fit_scale(block_w, ink_h, cs < 0 ? -cs : cs, sn < 0 ? -sn : sn, w, h);
  int64_t inv_k = ((int64_t)1 << 32) / k;

  // Rueckwaertsabbildung Ziel -> Quelle: Schritt je Zielpixel in x (u) und y (v)
  int32_t ux = (int32_t)(( (int64_t)cs * inv_k) >> 16);
  int32_t uy = (int32_t)((-(int64_t)sn * inv_k) >> 16);
  int32_t vx = (int32_t)(( (int64_t)sn * inv_k) >> 16);
  int32_t vy = (int32_t)(( (int64_t)cs * inv_k) >> 16);

  // Vier Abtastpunkte je Zielpixel, um +-1/4 Pixel gegen die Pixelmitte versetzt
  int32_t off_x[4], off_y[4];
  for (int i = 0; i < 4; i++) {
    int a = (i & 1) ? 1 : -1;
    int b = (i & 2) ? 1 : -1;
    off_x[i] = (a * ux + b * vx) / 4;
    off_y[i] = (a * uy + b * vy) / 4;
  }

  uint8_t lut[5];
  for (int cov = 0; cov <= 4; cov++) lut[cov] = blend(s_color_fg.argb, bg, cov);

  // Nur den Kreis abfahren, den der gedrehte Block ueberstreichen kann
  int radius = (int)(((int64_t)(isqrt((uint32_t)(ink_w * ink_w + ink_h * ink_h)) / 2 + 2) * k) >> 16) + 2;
  int y0 = dst_cy - radius;
  int y1 = dst_cy + radius;
  if (y0 < 0) y0 = 0;
  if (y1 > h - 1) y1 = h - 1;

  for (int y = y0; y <= y1; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int x0 = dst_cx - radius;
    int x1 = dst_cx + radius;
    if (x0 < row.min_x) x0 = row.min_x;
    if (x1 > row.max_x) x1 = row.max_x;
    if (x0 > x1) continue;

    int64_t dx = ((int64_t)(x0 - dst_cx) << 16) + (1 << 15);
    int64_t dy = ((int64_t)(y - dst_cy) << 16) + (1 << 15);
    int32_t sx = (src_cx << 16) + (int32_t)((dx * ux + dy * vx) >> 16);
    int32_t sy = (src_cy << 16) + (int32_t)((dx * uy + dy * vy) >> 16);

    for (int x = x0; x <= x1; x++) {
      int cov = 0;
      for (int i = 0; i < 4; i++) {
        cov += mask_get((sx + off_x[i]) >> 16, (sy + off_y[i]) >> 16, w, h);
      }
      if (cov) row.data[x] = lut[cov];
      sx += ux;
      sy += uy;
    }
  }

  graphics_release_frame_buffer(ctx, fb);
}

// --------------------------------------------------------------- Zeichnen

static void draw_row(GContext *ctx, GRect bounds, int y, const char *text) {
  GRect box = GRect(bounds.origin.x, y - TEXT_LIFT,
                    bounds.size.w, DIGIT_H + TEXT_LIFT);
  graphics_draw_text(ctx, text, s_digit_font, box,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     NULL);
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int cy = bounds.size.h / 2;

  graphics_context_set_fill_color(ctx, s_color_bg);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  graphics_context_set_text_color(ctx, s_color_fg);
  draw_row(ctx, bounds, cy + HOUR_TOP, s_hour_text);
  draw_row(ctx, bounds, cy + MIN_TOP, s_min_text);

  // Auch bei 0 Grad: der Block landet dann exakt mittig
  rotate_digits(ctx);
}

// --------------------------------------------------------------- Zeit

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time_text();
  layer_mark_dirty(s_canvas);
}

// --------------------------------------------------------------- Einstellungen

// Faengt die Farbwahl FG == BG ab, die das Watchface unlesbar machen wuerde.
// Clay kann das nicht verhindern, also tut es die Uhr.
static void enforce_contrast(void) {
  if (gcolor_equal(s_color_fg, s_color_bg)) {
    s_color_fg = gcolor_legible_over(s_color_bg);
  }
}

// Clay-Werte kommen je nach Element als String oder als Zahl an, Zahlen
// zudem in 1, 2 oder 4 Byte (der Toggle etwa als einzelnes Byte). Alles
// annehmen -- ein falsch geratener Typ wuerde stumm Muell liefern.
static int tuple_int(Tuple *t) {
  if (t->type == TUPLE_CSTRING) return atoi(t->value->cstring);
  bool is_signed = (t->type == TUPLE_INT);
  switch (t->length) {
    case 1:  return is_signed ? t->value->int8  : t->value->uint8;
    case 2:  return is_signed ? t->value->int16 : t->value->uint16;
    default: return is_signed ? t->value->int32 : (int)t->value->uint32;
  }
}

static bool valid_wrist(int v) { return v == WRIST_LEFT || v == WRIST_RIGHT; }
static bool valid_tilt(int v)  { return v >= 0 && v <= TILT_MAX; }

static void load_settings(void) {
  s_color_bg = COLOR_BG_DEFAULT;
  s_color_fg = COLOR_FG_DEFAULT;

  if (persist_exists(PKEY_COLOR_BG)) {
    s_color_bg = GColorFromHEX(persist_read_int(PKEY_COLOR_BG));
  }
  if (persist_exists(PKEY_COLOR_FG)) {
    s_color_fg = GColorFromHEX(persist_read_int(PKEY_COLOR_FG));
  }
  if (persist_exists(PKEY_WRIST)) {
    int v = persist_read_int(PKEY_WRIST);
    if (valid_wrist(v)) s_wrist = v;
  }
  if (persist_exists(PKEY_TILT)) {
    int v = persist_read_int(PKEY_TILT);
    if (valid_tilt(v)) s_tilt = v;
  }
  if (persist_exists(PKEY_FLIP)) {
    s_flip = persist_read_bool(PKEY_FLIP);
  }
  enforce_contrast();
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;

  if ((t = dict_find(iter, MESSAGE_KEY_COLOR_BG)) != NULL) {
    int v = (int)t->value->int32;
    s_color_bg = GColorFromHEX(v);
    persist_write_int(PKEY_COLOR_BG, v);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_COLOR_FG)) != NULL) {
    int v = (int)t->value->int32;
    s_color_fg = GColorFromHEX(v);
    persist_write_int(PKEY_COLOR_FG, v);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_WRIST)) != NULL) {
    int v = tuple_int(t);
    if (valid_wrist(v)) {
      s_wrist = v;
      persist_write_int(PKEY_WRIST, v);
    }
  }
  if ((t = dict_find(iter, MESSAGE_KEY_TILT)) != NULL) {
    int v = tuple_int(t);
    if (valid_tilt(v)) {
      s_tilt = v;
      persist_write_int(PKEY_TILT, v);
    }
  }
  if ((t = dict_find(iter, MESSAGE_KEY_FLIP)) != NULL) {
    s_flip = tuple_int(t) != 0;
    persist_write_bool(PKEY_FLIP, s_flip);
  }

  enforce_contrast();
  window_set_background_color(s_window, s_color_bg);
  layer_mark_dirty(s_canvas);
}

// --------------------------------------------------------------- Fenster

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
}

// --------------------------------------------------------------- Start

static void init(void) {
  load_settings();
  s_digit_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_LECO_104));
  s_ref_w = graphics_text_layout_get_content_size(
      "00", s_digit_font, GRect(0, 0, FB_MAX_W, FB_MAX_H),
      GTextOverflowModeWordWrap, GTextAlignmentLeft).w;

  update_time_text();

  s_window = window_create();
  window_set_background_color(s_window, s_color_bg);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, false);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  app_message_register_inbox_received(inbox_received);
  app_message_open(256, 64);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
  fonts_unload_custom_font(s_digit_font);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
