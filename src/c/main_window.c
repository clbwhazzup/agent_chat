#include "app.h"

// ---------------------------------------------------------------------------
// main_window.c -- the talk screen.
//
//   Select / tap         start or stop listening
//   Up, Down / swipe     previous / next session (when the agent has sessions)
//   Back / side swipe    settings
//   Hold Select / hold   switch agent
// ---------------------------------------------------------------------------

static Window *s_window;
static Layer *s_canvas;

#if HAS_TOUCH
static bool s_touch_down;
static int16_t s_touch_x0, s_touch_y0, s_touch_x, s_touch_y;
static AppTimer *s_hold_timer;     // fires when a touch is held still
static bool s_hold_fired;          // ...so its liftoff isn't also a tap
#define HOLD_MS 500
#endif

GColor accent_color(void) {
#ifdef PBL_COLOR
  if (g_state.accent) {
    return (GColor) { .argb = g_state.accent };
  }
  return GColorCobaltBlue;
#else
  return GColorBlack;
#endif
}

bool main_window_is_top(void) {
  return s_window && window_stack_get_top_window() == s_window;
}

void main_window_refresh(void) {
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void prv_draw_triangle(GContext *ctx, GPoint tip, int half, bool up) {
  int dy = up ? half : -half;
  GPoint points[3] = {
    tip,
    GPoint(tip.x - half, tip.y + dy),
    GPoint(tip.x + half, tip.y + dy),
  };
  GPathInfo info = { .num_points = 3, .points = points };
  GPath *path = gpath_create(&info);
  gpath_draw_filled(ctx, path);
  gpath_destroy(path);
}

static void prv_draw_mic(GContext *ctx, GPoint c, int r, bool active, GColor accent, GColor fg,
                         GColor bg) {
  GColor glyph;
  if (active) {
    graphics_context_set_fill_color(ctx, accent);
    graphics_fill_circle(ctx, c, r);
    glyph = PBL_IF_COLOR_ELSE(gcolor_legible_over(accent), bg);
  } else {
    graphics_context_set_stroke_color(ctx, fg);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_circle(ctx, c, r);
    glyph = fg;
  }

  int cw = r * 2 / 5;
  int ch = r * 4 / 5;
  GRect capsule = GRect(c.x - cw / 2, c.y - ch * 2 / 3, cw, ch);
  graphics_context_set_fill_color(ctx, glyph);
  graphics_fill_rect(ctx, capsule, cw / 2, GCornersAll);

  graphics_context_set_stroke_color(ctx, glyph);
  graphics_context_set_stroke_width(ctx, 2);
  int cup = cw / 2 + 3;
  GRect cup_rect = GRect(c.x - cup, capsule.origin.y + ch / 2 - cup, cup * 2, cup * 2);
  graphics_draw_arc(ctx, cup_rect, GOvalScaleModeFitCircle, DEG_TO_TRIGANGLE(90),
                    DEG_TO_TRIGANGLE(270));
  int stem_top = cup_rect.origin.y + cup * 2;
  int stem_bottom = stem_top + r / 5 + 1;
  graphics_draw_line(ctx, GPoint(c.x, stem_top), GPoint(c.x, stem_bottom));
  graphics_draw_line(ctx, GPoint(c.x - cw / 2, stem_bottom), GPoint(c.x + cw / 2, stem_bottom));
}

static const char *prv_idle_hint(void) {
  if (g_state.agent_count == 0 && g_state.state_known) {
    return "No agent";
  }
  bool touch = app_touch_active();
  return touch ? "Tap to talk" : "Select to talk";
}

// Height `text` needs at `width`, capped at `max_h`.
static int prv_text_height(const char *text, GFont font, int width, int max_h) {
  GSize size = graphics_text_layout_get_content_size(text, font, GRect(0, 0, width, max_h),
                                                     GTextOverflowModeTrailingEllipsis,
                                                     GTextAlignmentCenter);
  int h = size.h + 6;
  return h > max_h ? max_h : h;
}

static void prv_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  const bool big = b.size.w >= 200;
  const bool round = PBL_IF_ROUND_ELSE(true, false);
  const GColor fg = GColorBlack;
  const GColor bg = GColorWhite;
  const GColor accent = accent_color();
  const GColor on_accent = PBL_IF_COLOR_ELSE(gcolor_legible_over(accent), GColorWhite);

  GFont title_font = fonts_get_system_font(big ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD);
  GFont body_bold = fonts_get_system_font(big ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_14_BOLD);
  GFont body = fonts_get_system_font(big ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_14);
  GFont hint_font = fonts_get_system_font(big ? FONT_KEY_GOTHIC_24 : FONT_KEY_GOTHIC_18);
  const int title_h = big ? 30 : 22;
  const int line_h = big ? 22 : 16;

  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Header: the active agent.
  const int inset = round ? b.size.w / 8 : 4;
  const int header_h = title_h + (round ? b.size.h / 12 : 2);
  graphics_context_set_fill_color(ctx, accent);
  graphics_fill_rect(ctx, GRect(0, 0, b.size.w, header_h), 0, GCornerNone);
  const char *agent = "Agent Chat";
  if (g_state.agent_active >= 0 && g_state.agent_active < g_state.agent_count) {
    agent = g_state.agent_names[g_state.agent_active];
  }
  graphics_context_set_text_color(ctx, on_accent);
  graphics_draw_text(ctx, agent, title_font,
                     GRect(inset, header_h - title_h - 2, b.size.w - inset * 2, title_h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  int y = header_h + 2;

  // Session, with arrows when up/down can change it.
  const bool can_switch = (g_state.caps & CAP_SESSIONS) && g_state.session_count > 1;
  const int arrow = big ? 5 : 4;
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(accent, fg));
  if (can_switch) {
    prv_draw_triangle(ctx, GPoint(b.size.w / 2, y + 1), arrow, true);
  }
  y += arrow + 2;
  if (g_state.session_count > 0) {
    graphics_context_set_text_color(ctx, fg);
    graphics_draw_text(ctx, g_state.session_names[g_state.session_active], body_bold,
                       GRect(inset, y - 2, b.size.w - inset * 2, line_h + 2),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
  y += line_h + 2;
  if (can_switch) {
    prv_draw_triangle(ctx, GPoint(b.size.w / 2, y + arrow), arrow, false);
  }
  y += arrow + 2;

  // Bottom: a setup problem, else the last message and what happened to it.
  // Measured first so the microphone can be centred in whatever is left.
  const int bottom_inset = round ? b.size.w / 5 : 6;
  const int bottom_pad = round ? b.size.h / 9 : 4;
  const int text_w = b.size.w - bottom_inset * 2;
  const char *problem = NULL;
  if (!g_state.phone_connected) {
    problem = "Phone not connected";
  } else if (g_state.status[0]) {
    problem = g_state.status;
  }
  const char *message = NULL;
  const char *status = NULL;
  GFont status_font = body_bold;
  GColor status_color = fg;
  int status_lines = 1;
  if (problem && g_state.delivery == DELIVERY_NONE) {
    status = problem;
    status_font = body;
    status_lines = 3;
  } else {
    message = g_state.last_message[0] ? g_state.last_message : NULL;
    status = g_state.delivery_text[0] ? g_state.delivery_text : NULL;
    if (g_state.delivery == DELIVERY_REPLIED && g_state.reply[0]) {
      status = g_state.reply;
      status_lines = 2;
    } else if (g_state.delivery == DELIVERY_FAILED) {
      status_lines = 2;
      status_color = PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, fg);
    }
  }
  const int status_h = status ? prv_text_height(status, status_font, text_w,
                                                line_h * status_lines) : 0;

  // Microphone and hint, centred between the session and the bottom text.
  // On small screens (Time Round) everything doesn't always fit; give way in
  // order: the message to one line, then the hint, then the message.
  int hint_h = title_h;
  const int min_r = big ? 18 : 13;
  int msg_h = message ? prv_text_height(message, body, text_w, line_h * 2) : 0;
  #define PRV_SPACE() (b.size.h - bottom_pad - msg_h - status_h - y)
  if (msg_h && min_r * 2 + 4 + hint_h > PRV_SPACE()) {
    msg_h = prv_text_height(message, body, text_w, line_h);
  }
  if (min_r * 2 + 4 + hint_h > PRV_SPACE()) {
    hint_h = 0;
  }
  if (msg_h && min_r * 2 + 4 > PRV_SPACE()) {
    message = NULL;
    msg_h = 0;
  }
  const int bottom_top = b.size.h - bottom_pad - msg_h - status_h;
  const int space = PRV_SPACE();
  #undef PRV_SPACE
  int r = big ? 30 : 22;
  while (r > min_r && r * 2 + 4 + hint_h > space) {
    r--;
  }
  int top = y + (space - (r * 2 + 4 + hint_h)) / 2;
  if (top < y) {
    top = y;
  }
  prv_draw_mic(ctx, GPoint(b.size.w / 2, top + r), r, g_state.listening, accent, fg, bg);
  if (hint_h) {
    const char *hint = g_state.listening ? "Listening..." : prv_idle_hint();
    graphics_context_set_text_color(ctx, fg);
    graphics_draw_text(ctx, hint, hint_font,
                       GRect(inset, top + r * 2 + 4 - (big ? 6 : 4), b.size.w - inset * 2, hint_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }

  if (message) {
    graphics_context_set_text_color(ctx, fg);
    graphics_draw_text(ctx, message, body, GRect(bottom_inset, bottom_top, text_w, msg_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
  if (status) {
    graphics_context_set_text_color(ctx, status_color);
    graphics_draw_text(ctx, status, status_font,
                       GRect(bottom_inset, bottom_top + msg_h, text_w, status_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

static void prv_select(ClickRecognizerRef r, void *ctx) { app_toggle_listening(); }
static void prv_select_long(ClickRecognizerRef r, void *ctx) { agent_picker_push(); }
static void prv_up(ClickRecognizerRef r, void *ctx) { app_select_session_delta(-1); }
static void prv_down(ClickRecognizerRef r, void *ctx) { app_select_session_delta(1); }
static void prv_back(ClickRecognizerRef r, void *ctx) { settings_menu_push(); }

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, prv_select_long, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back);
}

// ---------------------------------------------------------------------------
// Touch: the same four gestures, recognised from raw touch events.
// ---------------------------------------------------------------------------

#if HAS_TOUCH
static int prv_swipe_threshold(void) {
  return layer_get_bounds(window_get_root_layer(s_window)).size.w / 5;
}

static void prv_cancel_hold(void) {
  if (s_hold_timer) {
    app_timer_cancel(s_hold_timer);
    s_hold_timer = NULL;
  }
}

// Touch analogue of holding Select.
static void prv_hold_timer(void *context) {
  s_hold_timer = NULL;
  if (!s_touch_down || !main_window_is_top()) {
    return;
  }
  s_hold_fired = true;
  vibes_short_pulse();
  agent_picker_push();
}

static void prv_touch(const TouchEvent *event, void *context) {
  if (event->non_navigational) {
    return;
  }
  switch (event->type) {
    case TouchEvent_Touchdown:
      s_touch_down = true;
      s_hold_fired = false;
      s_touch_x0 = s_touch_x = event->x;
      s_touch_y0 = s_touch_y = event->y;
      prv_cancel_hold();
      s_hold_timer = app_timer_register(HOLD_MS, prv_hold_timer, NULL);
      return;
    case TouchEvent_PositionUpdate: {
      s_touch_x = event->x;
      s_touch_y = event->y;
      int dx = s_touch_x - s_touch_x0;
      int dy = s_touch_y - s_touch_y0;
      int slop = prv_swipe_threshold() / 2;
      if (dx > slop || dx < -slop || dy > slop || dy < -slop) {
        prv_cancel_hold();  // it's a swipe, not a hold
      }
      return;
    }
    case TouchEvent_Liftoff:
      break;
  }
  prv_cancel_hold();
  if (!s_touch_down || s_hold_fired) {
    s_touch_down = false;
    return;
  }
  s_touch_down = false;
  s_touch_x = event->x;
  s_touch_y = event->y;

  int threshold = prv_swipe_threshold();
  int dx = s_touch_x - s_touch_x0;
  int dy = s_touch_y - s_touch_y0;
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;

  int swipe = touch_horizontal_swipe(dx, dy, threshold * 5);

  if (ady >= threshold && ady > adx) {
    // Content follows the finger: swiping up reveals the next session.
    app_select_session_delta(dy < 0 ? 1 : -1);
  } else if (swipe != 0) {
    // Back swipe opens settings, like the Back button; the select swipe has no
    // job here (a tap already starts listening).
    if (swipe < 0) {
      settings_menu_push();
    }
  } else if (adx < threshold / 2 && ady < threshold / 2) {
    app_toggle_listening();
  }
}

#endif

static void prv_update_touch_subscription(bool want) {
#if HAS_TOUCH
  s_touch_down = false;
  prv_cancel_hold();
  if (want) {
    app_touch_claim(prv_touch);
  } else {
    app_touch_release(prv_touch);
  }
#else
  (void)want;
#endif
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

static void prv_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, prv_draw);
  layer_add_child(root, s_canvas);
}

static void prv_unload(Window *window) {
  layer_destroy(s_canvas);
  s_canvas = NULL;
}

static void prv_appear(Window *window) {
  prv_update_touch_subscription(true);
  // A reply that arrived while dictating or in settings is waiting.
  main_window_refresh();
}

static void prv_disappear(Window *window) {
  prv_update_touch_subscription(false);
}

void main_window_push(void) {
  if (!s_window) {
    s_window = window_create();
    window_set_click_config_provider(s_window, prv_click_config);
    window_set_window_handlers(s_window, (WindowHandlers) {
      .load = prv_load,
      .unload = prv_unload,
      .appear = prv_appear,
      .disappear = prv_disappear,
    });
#if HAS_TOUCH
    // This window reads raw touches itself; don't also turn taps into presses.
    window_set_touch_bridge_disabled(s_window, true);
#endif
  }
  window_stack_push(s_window, true);
}
