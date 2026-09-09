// pebble_deck_lite - a three-panel, large-text fork of pebble_deck.
//
// The screen is split into three equal horizontal panels (top / middle / bottom).
// Each panel shows one complication as a small caption plus a large value.
// Battery percentage can optionally sit in any corner, out of the way.
// There is no permanent clock; "Time" is just one of the complications.

#include <pebble.h>

#define SETTINGS_KEY 1
#define NUM_PANELS 3

// --- HRV (unchanged from pebble_deck) --------------------------------------------------
// HRV comes from the sensor's peak-to-peak (RR) intervals. The Goodix algorithm reports
// bursts of up to 4 consecutive intervals, each as its own HealthEventHRVUpdate, so RMSSD
// over a rolling window of them is a real measurement rather than an approximation.
// Holding an HRV sample period keeps the PPG sensor and the HRV algorithm running, so the
// sensor is only opened for a short measurement window every so often rather than left on.
#define HRV_BUF_LEN 16
#define HRV_MIN_DIFFS 4
#define HRV_PPI_MIN_MS 300
#define HRV_PPI_MAX_MS 2000
#define HRV_MAX_DIFF_MS 250
#define HRV_MEASURE_EVERY_SEC 1800
#define HRV_BURST_PERIOD_SEC 5
#define HRV_WINDOW_MAX_SEC 120
#define HRV_RESULT_STALE_SEC 7200

#if defined(PBL_HEALTH) && defined(_PBL_API_EXISTS_health_service_set_hrv_sample_period)
#define DECK_HAS_HRV 1
#endif

// Complication IDs. These are the values used in the Clay <select> options, so keep them
// in sync with src/pkjs/config.json.
typedef enum {
  COMP_NONE = 0,
  COMP_TIME,
  COMP_DATE,
  COMP_BATTERY,
  COMP_MEMORY,
  COMP_COMPASS,
  COMP_HEART_RATE,
  COMP_STEPS,
  COMP_WEATHER,
  COMP_ALT,
  COMP_CUSTOM_API,
  COMP_TZ2,
  COMP_TZ3,
  COMP_MOON_PHASE,
  COMP_HRV,
  COMP_COUNT
} ComplicationType;

typedef enum {
  CORNER_OFF = 0,
  CORNER_TOP_LEFT,
  CORNER_TOP_RIGHT,
  CORNER_BOTTOM_LEFT,
  CORNER_BOTTOM_RIGHT
} BatteryCorner;

typedef struct ClaySettings {
  GColor BackgroundColor;
  GColor TextColor;
  ComplicationType Panels[NUM_PANELS];
  BatteryCorner BatteryCorner;
  bool ShowLabels;
  bool ShowDividers;
  int8_t Tz2Offset;
  int8_t Tz3Offset;
} ClaySettings;

static ClaySettings settings;

static Window *s_main_window;
static Layer *s_window_layer;
static Layer *s_canvas_layer;   // Draws the three panels
static Layer *s_battery_layer;  // Draws the small corner battery on top

// Fonts: three sizes of the value font so long strings can step down, plus the caption font.
static GFont s_font_big;
static GFont s_font_mid;
static GFont s_font_small;
static GFont s_font_caption;

// Per-panel text
static char s_value[NUM_PANELS][32];
static char s_label[NUM_PANELS][16];
static char s_battery_text[8];

// Global data caches
static int s_battery_level = 0;
static int s_compass_degrees = 0;
static const char *s_compass_dir = "---";
static bool s_compass_valid = false;
static int s_hr_value = 0;
static int s_steps_value = 0;
static char s_weather_cache[32] = "SCANNING...";
static char s_api_cache[16] = "NO DATA";
static int s_alt_cache = 0;
static char s_moon_cache[16] = "";
static int s_moon_day = -1;

typedef enum {
  HRV_UNSUPPORTED = 0,
  HRV_NO_DATA,
  HRV_COLLECTING,
  HRV_READY
} HrvState;

static HrvState s_hrv_state = HRV_COLLECTING;
static int s_hrv_rmssd = 0;
static uint16_t s_hrv_ppi[HRV_BUF_LEN];
static int s_hrv_count = 0;
static int s_hrv_head = 0;
static bool s_hrv_measuring = false;
static time_t s_hrv_window_start = 0;
static time_t s_hrv_result_at = 0;

static AppTimer *s_compass_timer = NULL;
static bool s_compass_subscribed = false;
static bool s_health_subscribed = false;

// --- SETTINGS ---------------------------------------------------------------------------
static void prv_default_settings() {
  settings.BackgroundColor = GColorBlack;
  settings.TextColor = GColorCyan;
  settings.Panels[0] = COMP_HEART_RATE;
  settings.Panels[1] = COMP_STEPS;
  settings.Panels[2] = COMP_ALT;
  settings.BatteryCorner = CORNER_BOTTOM_RIGHT;
  settings.ShowLabels = true;
  settings.ShowDividers = true;
  settings.Tz2Offset = 0;
  settings.Tz3Offset = 0;
}

static void prv_save_settings() {
  persist_write_data(SETTINGS_KEY, &settings, sizeof(settings));
}

static void prv_load_settings() {
  prv_default_settings();
  // Only trust stored settings that match this build's struct layout.
  if (persist_exists(SETTINGS_KEY) && persist_get_size(SETTINGS_KEY) == (int)sizeof(settings)) {
    persist_read_data(SETTINGS_KEY, &settings, sizeof(settings));
  }
  for (int i = 0; i < NUM_PANELS; i++) {
    if (settings.Panels[i] >= COMP_COUNT) settings.Panels[i] = COMP_NONE;
  }
  if (settings.BatteryCorner > CORNER_BOTTOM_RIGHT) settings.BatteryCorner = CORNER_BOTTOM_RIGHT;
}

// --- HELPERS ----------------------------------------------------------------------------
static bool prv_panel_active(ComplicationType comp) {
  for (int i = 0; i < NUM_PANELS; i++) {
    if (settings.Panels[i] == comp) return true;
  }
  return false;
}

static uint32_t prv_isqrt(uint32_t n) {
  if (n == 0) return 0;
  uint32_t x = n;
  uint32_t y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + n / x) / 2;
  }
  return x;
}

static void prv_upper(char *s) {
  for (; *s; s++) {
    if (*s >= 'a' && *s <= 'z') *s -= 'a' - 'A';
  }
}

static void prv_tz_label(char *buf, size_t len, int8_t off) {
  if (off == 0) {
    snprintf(buf, len, "UTC");
  } else {
    snprintf(buf, len, "UTC%c%d", off >= 0 ? '+' : '-', off < 0 ? -off : off);
  }
}

// --- MOON PHASE -------------------------------------------------------------------------
static double moon_age(int y, int m, int d) {
  int a = (14 - m) / 12;
  int y2 = y + 4800 - a;
  int m2 = m + 12 * a - 3;
  double jd = d + (double)((153 * m2 + 2) / 5) + 365 * (double)y2
              + (double)(y2 / 4) - (double)(y2 / 100) + (double)(y2 / 400) - 32045.0;
  double age = (jd - 2451550.1) / 29.530588853;
  age = age - (int)age;
  if (age < 0) age += 1.0;
  return age * 29.530588853;
}

static const char *moon_phase_name(double age) {
  if (age < 1.84566)  return "NEW";
  if (age < 5.53699)  return "WAX CRES";
  if (age < 9.22831)  return "1ST QTR";
  if (age < 12.91963) return "WAX GIBB";
  if (age < 16.61096) return "FULL";
  if (age < 20.30228) return "WAN GIBB";
  if (age < 23.99361) return "3RD QTR";
  if (age < 27.68493) return "WAN CRES";
  return "NEW";
}

static void health_handler(HealthEventType event, void *context);

// --- CORE: FILL THE THREE PANELS --------------------------------------------------------
// Each complication produces a short LABEL (caption) and a VALUE (the big text).
static void prv_fill_panel(ComplicationType comp, char *label, size_t llen, char *value, size_t vlen) {
  label[0] = '\0';
  value[0] = '\0';

  switch (comp) {
    case COMP_TIME: {
      time_t now = time(NULL);
      struct tm *t = localtime(&now);
      if (clock_is_24h_style()) {
        strftime(value, vlen, "%H:%M", t);
        snprintf(label, llen, "TIME");
      } else {
        strftime(value, vlen, "%I:%M", t);
        // Drop the leading zero ("09:41" -> "9:41") so the text centres properly.
        if (value[0] == '0') memmove(value, value + 1, strlen(value));
        snprintf(label, llen, t->tm_hour >= 12 ? "PM" : "AM");
      }
      break;
    }
    case COMP_DATE: {
      time_t now = time(NULL);
      struct tm *t = localtime(&now);
      strftime(value, vlen, "%a %d", t);
      strftime(label, llen, "%B %Y", t);
      prv_upper(value);
      prv_upper(label);
      break;
    }
    case COMP_BATTERY:
      snprintf(label, llen, "BATTERY");
      snprintf(value, vlen, "%d%%", s_battery_level);
      break;
    case COMP_MEMORY:
      snprintf(label, llen, "FREE KB");
      snprintf(value, vlen, "%d", ((int)heap_bytes_free()) / 1024);
      break;
    case COMP_COMPASS:
      snprintf(label, llen, "HEADING");
      if (!s_compass_valid) {
        snprintf(value, vlen, "CAL");
      } else {
        snprintf(value, vlen, "%d %s", s_compass_degrees, s_compass_dir);
      }
      break;
    case COMP_HEART_RATE:
      snprintf(label, llen, "BPM");
#if defined(PBL_HEALTH)
      if (s_hr_value > 0) {
        snprintf(value, vlen, "%d", s_hr_value);
      } else if (s_hr_value == -1) {
        snprintf(value, vlen, "...");
      } else {
        snprintf(value, vlen, "OFF");
      }
#else
      snprintf(value, vlen, "N/A");
#endif
      break;
    case COMP_HRV:
      snprintf(label, llen, "HRV MS");
#if defined(DECK_HAS_HRV)
      if (s_hrv_state == HRV_READY) {
        snprintf(value, vlen, "%d", s_hrv_rmssd);
      } else if (s_hrv_state == HRV_COLLECTING) {
        snprintf(value, vlen, "...");
      } else if (s_hrv_state == HRV_NO_DATA) {
        snprintf(value, vlen, "NO DATA");
      } else {
        snprintf(value, vlen, "N/A");
      }
#else
      snprintf(value, vlen, "N/A");
#endif
      break;
    case COMP_STEPS:
      snprintf(label, llen, "STEPS");
#if defined(PBL_HEALTH)
      snprintf(value, vlen, "%d", s_steps_value);
#else
      snprintf(value, vlen, "N/A");
#endif
      break;
    case COMP_WEATHER: {
      // The phone sends "72F CLOUDY"; show the temperature big and the condition as caption.
      const char *sp = strchr(s_weather_cache, ' ');
      if (sp && sp != s_weather_cache) {
        size_t n = (size_t)(sp - s_weather_cache);
        if (n >= vlen) n = vlen - 1;
        memcpy(value, s_weather_cache, n);
        value[n] = '\0';
        snprintf(label, llen, "%s", sp + 1);
      } else {
        snprintf(label, llen, "WEATHER");
        snprintf(value, vlen, "%s", s_weather_cache);
      }
      break;
    }
    case COMP_CUSTOM_API:
      snprintf(label, llen, "API");
      snprintf(value, vlen, "%s", s_api_cache);
      break;
    case COMP_ALT:
      snprintf(label, llen, "ALT FT");
      snprintf(value, vlen, "%d", s_alt_cache);
      break;
    case COMP_TZ2:
    case COMP_TZ3: {
      int8_t off = (comp == COMP_TZ2) ? settings.Tz2Offset : settings.Tz3Offset;
      time_t utc_now = time(NULL);
      time_t tz_time = utc_now + (off * 3600);
      struct tm *tz_tm = gmtime(&tz_time);
      strftime(value, vlen, "%H:%M", tz_tm);
      prv_tz_label(label, llen, off);
      break;
    }
    case COMP_MOON_PHASE: {
      time_t now = time(NULL);
      struct tm *tm_now = gmtime(&now);
      if (tm_now->tm_yday != s_moon_day) {
        double age = moon_age(tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday);
        snprintf(s_moon_cache, sizeof(s_moon_cache), "%s", moon_phase_name(age));
        s_moon_day = tm_now->tm_yday;
      }
      snprintf(label, llen, "MOON");
      snprintf(value, vlen, "%s", s_moon_cache);
      break;
    }
    case COMP_NONE:
    default:
      break;
  }
}

static void render_panels() {
  bool changed = false;
  for (int i = 0; i < NUM_PANELS; i++) {
    char label[16];
    char value[32];
    prv_fill_panel(settings.Panels[i], label, sizeof(label), value, sizeof(value));
    if (strcmp(label, s_label[i]) != 0 || strcmp(value, s_value[i]) != 0) {
      memcpy(s_label[i], label, sizeof(label));
      memcpy(s_value[i], value, sizeof(value));
      changed = true;
    }
  }
  // Repainting costs more than comparing, and most events change nothing on screen.
  if (changed && s_canvas_layer) layer_mark_dirty(s_canvas_layer);
}

static void render_battery() {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", s_battery_level);
  if (strcmp(buf, s_battery_text) != 0) {
    memcpy(s_battery_text, buf, sizeof(buf));
    if (s_battery_layer) layer_mark_dirty(s_battery_layer);
  }
}

static void prv_update_display() {
  window_set_background_color(s_main_window, settings.BackgroundColor);
  render_panels();
  render_battery();
  if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
  if (s_battery_layer) layer_mark_dirty(s_battery_layer);
}

// --- DRAWING ----------------------------------------------------------------------------
// Pick the largest font whose rendered width fits inside `width`.
static GFont prv_pick_font(GContext *ctx, const char *text, int width, int height) {
  GFont candidates[3] = { s_font_big, s_font_mid, s_font_small };
  GFont chosen = s_font_small;
  for (int i = 0; i < 3; i++) {
    GSize sz = graphics_text_layout_get_content_size(text, candidates[i], GRect(0, 0, 1000, height),
                                                     GTextOverflowModeWordWrap, GTextAlignmentCenter);
    if (sz.w <= width) {
      chosen = candidates[i];
      break;
    }
  }
  return chosen;
}

static void prv_draw_centered(GContext *ctx, const char *text, GFont font, GRect box) {
  GSize sz = graphics_text_layout_get_content_size(text, font, GRect(0, 0, box.size.w, 1000),
                                                   GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter);
  int y = box.origin.y + (box.size.h - sz.h) / 2;
  graphics_draw_text(ctx, text, font, GRect(box.origin.x, y, box.size.w, sz.h + 4),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_text_color(ctx, settings.TextColor);
  graphics_context_set_stroke_color(ctx, settings.TextColor);
  graphics_context_set_stroke_width(ctx, 1);

  const int margin_x = 6;
  int panel_h = bounds.size.h / NUM_PANELS;

  for (int i = 0; i < NUM_PANELS; i++) {
    GRect panel = GRect(margin_x, i * panel_h, bounds.size.w - 2 * margin_x, panel_h);

    if (settings.ShowDividers && i > 0) {
      graphics_draw_line(ctx, GPoint(panel.origin.x + 10, panel.origin.y),
                         GPoint(panel.origin.x + panel.size.w - 10, panel.origin.y));
    }

    if (settings.Panels[i] == COMP_NONE) continue;

    bool show_label = settings.ShowLabels && s_label[i][0] != '\0';
    int label_h = show_label ? 16 : 0;

    if (show_label) {
      graphics_draw_text(ctx, s_label[i], s_font_caption,
                         GRect(panel.origin.x, panel.origin.y + 2, panel.size.w, label_h),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    }

    GRect value_box = GRect(panel.origin.x, panel.origin.y + label_h,
                            panel.size.w, panel.size.h - label_h);
    GFont font = prv_pick_font(ctx, s_value[i], value_box.size.w, value_box.size.h);
    prv_draw_centered(ctx, s_value[i], font, value_box);
  }
}

static void battery_update_proc(Layer *layer, GContext *ctx) {
  if (settings.BatteryCorner == CORNER_OFF) return;
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_text_color(ctx, settings.TextColor);

  const int w = 44, h = 16, pad = 3;
  int x = (settings.BatteryCorner == CORNER_TOP_LEFT || settings.BatteryCorner == CORNER_BOTTOM_LEFT)
          ? pad : bounds.size.w - w - pad;
  int y = (settings.BatteryCorner == CORNER_TOP_LEFT || settings.BatteryCorner == CORNER_TOP_RIGHT)
          ? pad : bounds.size.h - h - pad;
  GTextAlignment align = (x == pad) ? GTextAlignmentLeft : GTextAlignmentRight;

  graphics_draw_text(ctx, s_battery_text, s_font_caption, GRect(x, y, w, h),
                     GTextOverflowModeTrailingEllipsis, align, NULL);
}

// --- DATA UPDATERS ----------------------------------------------------------------------
static void compass_handler(CompassHeadingData heading_data) {
  if (heading_data.compass_status == CompassStatusDataInvalid ||
      heading_data.compass_status == CompassStatusCalibrating) {
    s_compass_valid = false;
  } else {
    s_compass_valid = true;
    s_compass_degrees = 360 - TRIGANGLE_TO_DEG(heading_data.magnetic_heading);
    if (s_compass_degrees >= 360) s_compass_degrees -= 360;

    if (s_compass_degrees < 23 || s_compass_degrees > 337) s_compass_dir = "N";
    else if (s_compass_degrees < 68)  s_compass_dir = "NE";
    else if (s_compass_degrees < 113) s_compass_dir = "E";
    else if (s_compass_degrees < 158) s_compass_dir = "SE";
    else if (s_compass_degrees < 203) s_compass_dir = "S";
    else if (s_compass_degrees < 248) s_compass_dir = "SW";
    else if (s_compass_degrees < 293) s_compass_dir = "W";
    else if (s_compass_degrees < 338) s_compass_dir = "NW";
  }
  render_panels();
}

static void compass_timer_callback(void *data) {
  if (!s_compass_valid) {
    s_compass_timer = app_timer_register(10000, compass_timer_callback, NULL);
    return;
  }
  if (s_compass_subscribed) {
    compass_service_unsubscribe();
    s_compass_subscribed = false;
  }
  s_compass_timer = NULL;
}

static void backlight_callback(bool backlight_on) {
  if (backlight_on) {
    if (s_compass_timer) app_timer_cancel(s_compass_timer);
    if (!s_compass_subscribed) {
      compass_service_subscribe(compass_handler);
      compass_service_set_heading_filter(2 * (TRIG_MAX_ANGLE / 360));
      s_compass_subscribed = true;
    }
    s_compass_timer = app_timer_register(5000, compass_timer_callback, NULL);
  }
}

static void update_hr() {
#if defined(PBL_HEALTH)
  HealthMetric metric = HealthMetricHeartRateBPM;
  time_t start = time(NULL);
  time_t end = time(NULL);
  if (health_service_metric_accessible(metric, start, end) & HealthServiceAccessibilityMaskAvailable) {
    HealthValue hr = health_service_peek_current_value(metric);
    s_hr_value = (hr > 0) ? (int)hr : -1;
  } else {
    s_hr_value = 0;
  }
#endif
  render_panels();
}

static void update_steps() {
#if defined(PBL_HEALTH)
  HealthMetric metric = HealthMetricStepCount;
  time_t start = time_start_of_today();
  time_t end = time(NULL);
  if (health_service_metric_accessible(metric, start, end) & HealthServiceAccessibilityMaskAvailable) {
    s_steps_value = (int)health_service_sum_today(metric);
  } else {
    s_steps_value = 0;
  }
#endif
  render_panels();
}

// --- HRV --------------------------------------------------------------------------------
#if defined(DECK_HAS_HRV)
static void prv_hrv_reset() {
  s_hrv_count = 0;
  s_hrv_head = 0;
}

static void prv_hrv_push_ppi(uint16_t ppi) {
  if (ppi < HRV_PPI_MIN_MS || ppi > HRV_PPI_MAX_MS) return;
  s_hrv_ppi[s_hrv_head] = ppi;
  s_hrv_head = (s_hrv_head + 1) % HRV_BUF_LEN;
  if (s_hrv_count < HRV_BUF_LEN) s_hrv_count++;
}

static bool prv_hrv_compute() {
  uint32_t sum_sq = 0;
  int diffs = 0;
  int start = (s_hrv_head - s_hrv_count + HRV_BUF_LEN) % HRV_BUF_LEN;
  for (int i = 1; i < s_hrv_count; i++) {
    int prev = s_hrv_ppi[(start + i - 1) % HRV_BUF_LEN];
    int delta = s_hrv_ppi[(start + i) % HRV_BUF_LEN] - prev;
    if (delta < 0) delta = -delta;
    if (delta > HRV_MAX_DIFF_MS) continue;
    sum_sq += (uint32_t)(delta * delta);
    diffs++;
  }
  if (diffs < HRV_MIN_DIFFS) return false;
  s_hrv_rmssd = (int)prv_isqrt(sum_sq / (uint32_t)diffs);
  return true;
}

static void prv_hrv_stop_window() {
  health_service_set_hrv_sample_period(0);
  s_hrv_measuring = false;
}

static void prv_hrv_start_window() {
  if (!health_service_set_hrv_sample_period(HRV_BURST_PERIOD_SEC)) {
    s_hrv_state = HRV_UNSUPPORTED;
    return;
  }
  prv_hrv_reset();
  s_hrv_measuring = true;
  s_hrv_window_start = time(NULL);
  if (s_hrv_state == HRV_UNSUPPORTED) s_hrv_state = HRV_COLLECTING;
}

static void prv_hrv_finish_window(time_t now) {
  if (prv_hrv_compute()) {
    s_hrv_state = HRV_READY;
    s_hrv_result_at = now;
  } else if (s_hrv_state != HRV_READY) {
    s_hrv_state = (s_hrv_count == 0) ? HRV_NO_DATA : HRV_COLLECTING;
  }
  prv_hrv_stop_window();
}
#endif

static void prv_hrv_tick(time_t now) {
#if defined(DECK_HAS_HRV)
  if (!prv_panel_active(COMP_HRV)) {
    if (s_hrv_measuring) prv_hrv_stop_window();
    return;
  }
  if (s_hrv_measuring) {
    if ((now - s_hrv_window_start) >= HRV_WINDOW_MAX_SEC) {
      prv_hrv_finish_window(now);
      render_panels();
    }
    return;
  }
  if (s_hrv_state == HRV_READY && (now - s_hrv_result_at) > HRV_RESULT_STALE_SEC) {
    s_hrv_state = HRV_NO_DATA;
    render_panels();
  }
  if (s_hrv_result_at == 0 || (now - s_hrv_result_at) >= HRV_MEASURE_EVERY_SEC) {
    prv_hrv_start_window();
  }
#endif
}

static void prv_hrv_on_layout_change() {
#if defined(DECK_HAS_HRV)
  if (!prv_panel_active(COMP_HRV)) {
    if (s_hrv_measuring) prv_hrv_stop_window();
    prv_hrv_reset();
    s_hrv_rmssd = 0;
    s_hrv_state = HRV_COLLECTING;
    s_hrv_result_at = 0;
    return;
  }
  if (!s_health_subscribed) {
    s_health_subscribed = health_service_events_subscribe(health_handler, NULL);
  }
  if (!s_hrv_measuring) prv_hrv_start_window();
#endif
}

// --- SERVICE CALLBACKS ------------------------------------------------------------------
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  render_panels();
  prv_hrv_tick(time(NULL));
  if (tick_time->tm_min % 30 == 0) {
    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);
    dict_write_uint8(iter, MESSAGE_KEY_RequestWeather, 1);
    app_message_outbox_send();
  }
}

static void health_handler(HealthEventType event, void *context) {
  if (event == HealthEventMovementUpdate) {
    update_steps();
  } else if (event == HealthEventHeartRateUpdate) {
    update_hr();
  }
#if defined(DECK_HAS_HRV)
  else if (event == HealthEventHRVUpdate && s_hrv_measuring) {
    prv_hrv_push_ppi(health_service_peek_hrv_ppi_ms());
    if (s_hrv_count >= HRV_BUF_LEN) {
      prv_hrv_finish_window(time(NULL));
      render_panels();
    }
  }
#endif
}

static void battery_callback(BatteryChargeState state) {
  s_battery_level = state.charge_percent;
  render_battery();
  render_panels();
}

// Make sure the sensors a freshly chosen layout needs are running. Cheap to call repeatedly.
static void prv_sync_subscriptions() {
  bool needs_health = prv_panel_active(COMP_STEPS) || prv_panel_active(COMP_HEART_RATE)
                      || prv_panel_active(COMP_HRV);
  bool needs_compass = prv_panel_active(COMP_COMPASS);

  if (needs_health && !s_health_subscribed) {
    s_health_subscribed = health_service_events_subscribe(health_handler, NULL);
    update_steps();
    update_hr();
  }
  if (needs_compass && !s_compass_subscribed) {
    compass_service_subscribe(compass_handler);
    compass_service_set_heading_filter(2 * (TRIG_MAX_ANGLE / 360));
    s_compass_subscribed = true;
    if (s_compass_timer) app_timer_cancel(s_compass_timer);
    s_compass_timer = app_timer_register(5000, compass_timer_callback, NULL);
    backlight_service_subscribe(backlight_callback);
  }
}

// --- APPMESSAGE -------------------------------------------------------------------------
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *t;
  bool settings_changed = false;

  if ((t = dict_find(iterator, MESSAGE_KEY_Conditions))) {
    snprintf(s_weather_cache, sizeof(s_weather_cache), "%s", t->value->cstring);
  }
  if ((t = dict_find(iterator, MESSAGE_KEY_CustomApi))) {
    snprintf(s_api_cache, sizeof(s_api_cache), "%s", t->value->cstring);
  }
  if ((t = dict_find(iterator, MESSAGE_KEY_Alt))) {
    s_alt_cache = (int)t->value->int32;
  }

  const uint32_t panel_keys[NUM_PANELS] = { MESSAGE_KEY_Panel1, MESSAGE_KEY_Panel2, MESSAGE_KEY_Panel3 };
  for (int i = 0; i < NUM_PANELS; i++) {
    if ((t = dict_find(iterator, panel_keys[i]))) {
      int v = atoi(t->value->cstring);
      settings.Panels[i] = (v >= 0 && v < COMP_COUNT) ? (ComplicationType)v : COMP_NONE;
      settings_changed = true;
    }
  }

  if ((t = dict_find(iterator, MESSAGE_KEY_BatteryCorner))) {
    int v = atoi(t->value->cstring);
    settings.BatteryCorner = (v >= 0 && v <= CORNER_BOTTOM_RIGHT) ? (BatteryCorner)v : CORNER_OFF;
    settings_changed = true;
  }
  if ((t = dict_find(iterator, MESSAGE_KEY_ShowLabels))) {
    settings.ShowLabels = t->value->int32 != 0;
    settings_changed = true;
  }
  if ((t = dict_find(iterator, MESSAGE_KEY_ShowDividers))) {
    settings.ShowDividers = t->value->int32 != 0;
    settings_changed = true;
  }
  if ((t = dict_find(iterator, MESSAGE_KEY_BackgroundColor))) {
    settings.BackgroundColor = GColorFromHEX(t->value->int32);
    settings_changed = true;
  }
  if ((t = dict_find(iterator, MESSAGE_KEY_TextColor))) {
    settings.TextColor = GColorFromHEX(t->value->int32);
    settings_changed = true;
  }
  if ((t = dict_find(iterator, MESSAGE_KEY_Tz2Offset))) {
    settings.Tz2Offset = (int8_t)t->value->int32;
    settings_changed = true;
  }
  if ((t = dict_find(iterator, MESSAGE_KEY_Tz3Offset))) {
    settings.Tz3Offset = (int8_t)t->value->int32;
    settings_changed = true;
  }

  if (settings_changed) {
    prv_save_settings();
    prv_sync_subscriptions();
    prv_hrv_on_layout_change();
    prv_update_display();
  }
  render_panels();
}

// --- QUICK VIEW (unobstructed area) -----------------------------------------------------
// When a timeline peek covers the bottom of the screen, shrink the canvas so all three
// panels stay visible in the remaining space.
static void prv_unobstructed_change(AnimationProgress progress, void *context) {
  GRect bounds = layer_get_unobstructed_bounds(s_window_layer);
  layer_set_frame(s_canvas_layer, bounds);
  layer_set_frame(s_battery_layer, bounds);
}

// --- WINDOW -----------------------------------------------------------------------------
static void main_window_load(Window *window) {
  s_window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_unobstructed_bounds(s_window_layer);

  s_font_big     = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_CHAKRA_48));
  s_font_mid     = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_CHAKRA_32));
  s_font_small   = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_CHAKRA_22));
  s_font_caption = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_IBM_14));

  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
  layer_add_child(s_window_layer, s_canvas_layer);

  s_battery_layer = layer_create(bounds);
  layer_set_update_proc(s_battery_layer, battery_update_proc);
  layer_add_child(s_window_layer, s_battery_layer);

  UnobstructedAreaHandlers handlers = { .change = prv_unobstructed_change };
  unobstructed_area_service_subscribe(handlers, NULL);

  prv_update_display();
}

static void main_window_unload(Window *window) {
  layer_destroy(s_canvas_layer);
  layer_destroy(s_battery_layer);
  s_canvas_layer = NULL;
  s_battery_layer = NULL;
  fonts_unload_custom_font(s_font_big);
  fonts_unload_custom_font(s_font_mid);
  fonts_unload_custom_font(s_font_small);
  fonts_unload_custom_font(s_font_caption);
}

static void init() {
  prv_load_settings();
  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });
  window_stack_push(s_main_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_callback);

  app_message_register_inbox_received(inbox_received_callback);
  app_message_open(256, 256);

  // Ask the phone for weather / altitude / API data straight away.
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  dict_write_uint8(iter, MESSAGE_KEY_RequestWeather, 1);
  app_message_outbox_send();

  battery_callback(battery_state_service_peek());
  prv_sync_subscriptions();
  prv_hrv_on_layout_change();
  render_panels();
}

static void deinit() {
#if defined(DECK_HAS_HRV)
  health_service_set_hrv_sample_period(0);
#endif
  if (s_compass_timer) app_timer_cancel(s_compass_timer);
  backlight_service_unsubscribe();
  compass_service_unsubscribe();
  health_service_events_unsubscribe();
  battery_state_service_unsubscribe();
  tick_timer_service_unsubscribe();
  unobstructed_area_service_unsubscribe();
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
