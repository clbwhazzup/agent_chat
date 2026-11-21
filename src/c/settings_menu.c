#include "app.h"

// ---------------------------------------------------------------------------
// settings_menu.c -- opened with Back (or a side swipe) from the talk screen.
//
// Rows appear only when the active agent plugin supports them: Session needs
// agent/chats, the reply rows need a read action, Touch controls needs a
// touchscreen. Touch navigation in here comes from the system bridge
// (app_touch_navigation_enable), so it follows the Touch controls setting.
// ---------------------------------------------------------------------------

typedef enum {
  ROW_CHAT,
  ROW_AGENT,
  ROW_SESSION,
  ROW_LAST_REPLY,
  ROW_CHECK_REPLIES,
  ROW_WAIT_REPLY,
  ROW_CONFIRM,
  ROW_LISTEN_LAUNCH,
  ROW_TOUCH,
  ROW_EXIT,
  ROW_KIND_COUNT,
} RowKind;

static Window *s_window;
static MenuLayer *s_menu;
static RowKind s_rows[ROW_KIND_COUNT];
static uint8_t s_row_count;

// Picker: one generic list window for agents and sessions.
typedef enum { PICK_AGENT, PICK_SESSION } PickKind;
static Window *s_pick_window;
static MenuLayer *s_pick_menu;
static PickKind s_pick_kind;

static bool prv_has_touchscreen(void) {
  return HAS_TOUCH;
}

static void prv_build_rows(void) {
  s_row_count = 0;
  s_rows[s_row_count++] = ROW_CHAT;
  s_rows[s_row_count++] = ROW_AGENT;
  if ((g_state.caps & CAP_SESSIONS) && g_state.session_count > 0) {
    s_rows[s_row_count++] = ROW_SESSION;
  }
  if (g_state.reply[0]) {
    s_rows[s_row_count++] = ROW_LAST_REPLY;
  }
  if (g_state.caps & CAP_READ) {
    s_rows[s_row_count++] = ROW_CHECK_REPLIES;
    s_rows[s_row_count++] = ROW_WAIT_REPLY;
  }
  s_rows[s_row_count++] = ROW_CONFIRM;
  s_rows[s_row_count++] = ROW_LISTEN_LAUNCH;
  if (prv_has_touchscreen()) {
    s_rows[s_row_count++] = ROW_TOUCH;
  }
  s_rows[s_row_count++] = ROW_EXIT;
}

static const char *prv_on_off(bool on) {
  return on ? "On" : "Off";
}

// ---------------------------------------------------------------------------
// Picker
// ---------------------------------------------------------------------------

static uint16_t prv_pick_rows(MenuLayer *m, uint16_t section, void *ctx) {
  return s_pick_kind == PICK_AGENT ? g_state.agent_count : g_state.session_count;
}

static void prv_pick_draw(GContext *ctx, const Layer *cell, MenuIndex *index, void *c) {
  bool agent = s_pick_kind == PICK_AGENT;
  const char *name = agent ? g_state.agent_names[index->row] : g_state.session_names[index->row];
  bool active = agent ? index->row == g_state.agent_active : index->row == g_state.session_active;
  menu_cell_basic_draw(ctx, cell, name, active ? "Current" : NULL, NULL);
}

static void prv_pick_select(MenuLayer *m, MenuIndex *index, void *ctx) {
  if (s_pick_kind == PICK_AGENT) {
    if (index->row != g_state.agent_active) {
      g_state.agent_active = (int8_t)index->row;
      // The phone answers with the new agent's capabilities and sessions.
      g_state.session_count = 0;
      g_state.session_active = 0;
      g_state.caps = 0;
      g_state.status[0] = '\0';
      g_state.last_message[0] = '\0';
      g_state.reply[0] = '\0';
      g_state.delivery = DELIVERY_NONE;
      g_state.delivery_text[0] = '\0';
      app_send_command(CMD_SELECT_AGENT, index->row, NULL);
    }
  } else if (index->row != g_state.session_active) {
    app_select_session_delta((int)index->row - (int)g_state.session_active);
  }
  cache_save();
  window_stack_remove(s_pick_window, true);
  settings_menu_refresh();
}

static void prv_pick_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_pick_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_pick_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_pick_rows,
    .draw_row = prv_pick_draw,
    .select_click = prv_pick_select,
  });
#ifdef PBL_COLOR
  menu_layer_set_highlight_colors(s_pick_menu, accent_color(), gcolor_legible_over(accent_color()));
#endif
  menu_layer_set_click_config_onto_window(s_pick_menu, window);
  int active = s_pick_kind == PICK_AGENT ? g_state.agent_active : g_state.session_active;
  if (active > 0) {
    menu_layer_set_selected_index(s_pick_menu, MenuIndex(0, active), MenuRowAlignCenter, false);
  }
  layer_add_child(root, menu_layer_get_layer(s_pick_menu));
}

static void prv_pick_unload(Window *window) {
  menu_layer_destroy(s_pick_menu);
  s_pick_menu = NULL;
  window_destroy(s_pick_window);
  s_pick_window = NULL;
}

static void prv_pick_push(PickKind kind) {
  if (s_pick_window) {
    return;
  }
  s_pick_kind = kind;
  s_pick_window = window_create();
  window_set_window_handlers(s_pick_window, (WindowHandlers) {
    .load = prv_pick_load,
    .unload = prv_pick_unload,
  });
  window_stack_push(s_pick_window, true);
}

// ---------------------------------------------------------------------------
// Settings menu
// ---------------------------------------------------------------------------

static uint16_t prv_rows(MenuLayer *m, uint16_t section, void *ctx) {
  return s_row_count;
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *c) {
  const char *title = "";
  const char *subtitle = NULL;
  switch (s_rows[index->row]) {
    case ROW_CHAT:
      title = "Continue Chatting";
      break;
    case ROW_AGENT:
      title = "Agent";
      if (g_state.agent_active >= 0 && g_state.agent_active < g_state.agent_count) {
        subtitle = g_state.agent_names[g_state.agent_active];
      } else {
        subtitle = g_state.state_known ? "None installed" : "Loading...";
      }
      break;
    case ROW_SESSION:
      title = "Session";
      subtitle = g_state.session_names[g_state.session_active];
      break;
    case ROW_LAST_REPLY:
      title = "Last reply";
      subtitle = g_state.reply;
      break;
    case ROW_CHECK_REPLIES:
      title = "Check for replies";
      break;
    case ROW_WAIT_REPLY:
      title = "Wait for replies";
      subtitle = prv_on_off(g_prefs.wait_for_reply);
      break;
    case ROW_CONFIRM:
      title = "Review before send";
      subtitle = prv_on_off(g_prefs.confirm);
      break;
    case ROW_LISTEN_LAUNCH:
      title = "Listen on launch";
      subtitle = prv_on_off(g_prefs.listen_on_launch);
      break;
    case ROW_TOUCH:
      title = "Touch controls";
#if HAS_TOUCH
      if (g_prefs.touch_enabled && !touch_service_is_enabled()) {
        subtitle = "Off in watch settings";
        break;
      }
#endif
      subtitle = prv_on_off(g_prefs.touch_enabled);
      break;
    case ROW_EXIT:
      title = "Exit";
      break;
    case ROW_KIND_COUNT:
      break;
  }
  menu_cell_basic_draw(ctx, cell, title, subtitle, NULL);
}

static void prv_touch_follow_pref(void);

static void prv_activate(uint16_t row) {
  MenuLayer *m = s_menu;
  switch (s_rows[row]) {
    case ROW_CHAT:
      window_stack_remove(s_window, true);
      return;
    case ROW_AGENT:
      if (g_state.agent_count > 0) {
        prv_pick_push(PICK_AGENT);
      } else {
        app_send_command(CMD_REFRESH, 0, NULL);
      }
      return;
    case ROW_SESSION:
      prv_pick_push(PICK_SESSION);
      return;
    case ROW_LAST_REPLY:
      reply_window_push(g_state.reply);
      return;
    case ROW_CHECK_REPLIES:
      g_state.delivery = DELIVERY_WAITING;
      strncpy(g_state.delivery_text, "Checking for replies...", STATUS_LEN - 1);
      app_send_command(CMD_READ, 0, NULL);
      window_stack_remove(s_window, true);
      return;
    case ROW_WAIT_REPLY:
      g_prefs.wait_for_reply = !g_prefs.wait_for_reply;
      break;
    case ROW_CONFIRM:
      g_prefs.confirm = !g_prefs.confirm;
      break;
    case ROW_LISTEN_LAUNCH:
      g_prefs.listen_on_launch = !g_prefs.listen_on_launch;
      break;
    case ROW_TOUCH:
      g_prefs.touch_enabled = !g_prefs.touch_enabled;
      app_apply_touch_pref();
      prv_touch_follow_pref();
      break;
    case ROW_EXIT:
      window_stack_pop_all(true);
      return;
    case ROW_KIND_COUNT:
      return;
  }
  prefs_save();
  menu_layer_reload_data(m);
}

// The touch bridge turns a horizontal "select" swipe into a Select press, the
// same press a tap or the button makes. The raw touch stream says which it
// was, so a Select is held for a moment: after a swipe it means "back to the
// chat"; otherwise it activates the highlighted row.
#if HAS_TOUCH
#define SWIPE_SELECT_WINDOW_MS 400
#define SELECT_DEFER_MS 80

static uint64_t s_swipe_at;
static int16_t s_touch_x0, s_touch_y0;
static uint16_t s_pending_row;
static AppTimer *s_select_timer;

static uint64_t prv_now_ms(void) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  return (uint64_t)s * 1000 + ms;
}

static void prv_menu_touch(const TouchEvent *event, void *context) {
  if (event->non_navigational) {
    return;
  }
  if (event->type == TouchEvent_Touchdown) {
    s_touch_x0 = event->x;
    s_touch_y0 = event->y;
  } else if (event->type == TouchEvent_Liftoff && s_window) {
    int width = layer_get_bounds(window_get_root_layer(s_window)).size.w;
    int swipe = touch_horizontal_swipe(event->x - s_touch_x0, event->y - s_touch_y0, width);
    if (swipe != 0) {
      // Any Select the bridge makes of this swipe is swallowed below.
      s_swipe_at = prv_now_ms();
    }
    if (swipe > 0) {
      window_stack_remove(s_window, true);  // select swipe: back to the chat
    }
  }
}

static void prv_deferred_select(void *context) {
  s_select_timer = NULL;
  if (!s_window) {
    return;
  }
  if (prv_now_ms() - s_swipe_at < SWIPE_SELECT_WINDOW_MS) {
    s_swipe_at = 0;  // a swipe's Select, already handled on liftoff
    return;
  }
  prv_activate(s_pending_row);
}
#endif

// Start or stop reading touches here as soon as Touch controls is flipped.
static void prv_touch_follow_pref(void) {
#if HAS_TOUCH
  if (app_touch_active()) {
    app_touch_claim(prv_menu_touch);
  } else {
    app_touch_release(prv_menu_touch);
  }
#endif
}

static void prv_select(MenuLayer *m, MenuIndex *index, void *ctx) {
#if HAS_TOUCH
  if (app_touch_active()) {
    s_pending_row = index->row;
    if (!s_select_timer) {
      s_select_timer = app_timer_register(SELECT_DEFER_MS, prv_deferred_select, NULL);
    }
    return;
  }
#endif
  prv_activate(index->row);
}

static int16_t prv_cell_height(MenuLayer *m, MenuIndex *index, void *ctx) {
#ifdef PBL_ROUND
  return menu_layer_is_index_selected(m, index) ? 60 : 44;
#else
  RowKind kind = s_rows[index->row];
  return (kind == ROW_CHAT || kind == ROW_CHECK_REPLIES || kind == ROW_EXIT) ? 36 : 48;
#endif
}

// MenuLayer installs its own click handling; wrap it so Back closes the app
// instead of returning to the talk screen (Continue Chatting does that).
static ClickConfigProvider s_menu_click_config;

static void prv_back_exits(ClickRecognizerRef recognizer, void *context) {
  window_stack_pop_all(true);
}

static void prv_click_config(void *context) {
  s_menu_click_config(context);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_exits);
}

static void prv_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  prv_build_rows();
  s_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_rows,
    .draw_row = prv_draw_row,
    .select_click = prv_select,
    .get_cell_height = prv_cell_height,
  });
#ifdef PBL_COLOR
  menu_layer_set_highlight_colors(s_menu, accent_color(), gcolor_legible_over(accent_color()));
#endif
  menu_layer_set_click_config_onto_window(s_menu, window);
  s_menu_click_config = window_get_click_config_provider(window);
  window_set_click_config_provider_with_context(window, prv_click_config,
                                                window_get_click_config_context(window));
  layer_add_child(root, menu_layer_get_layer(s_menu));
}

static void prv_appear(Window *window) {
#if HAS_TOUCH
  app_touch_claim(prv_menu_touch);
#endif
}

static void prv_disappear(Window *window) {
#if HAS_TOUCH
  app_touch_release(prv_menu_touch);
  if (s_select_timer) {
    app_timer_cancel(s_select_timer);
    s_select_timer = NULL;
  }
#endif
}

static void prv_unload(Window *window) {
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void settings_menu_refresh(void) {
  if (!s_menu) {
    return;
  }
  prv_build_rows();
  MenuIndex sel = menu_layer_get_selected_index(s_menu);
  menu_layer_reload_data(s_menu);
  if (sel.row >= s_row_count) {
    menu_layer_set_selected_index(s_menu, MenuIndex(0, s_row_count - 1), MenuRowAlignCenter,
                                  false);
  }
#ifdef PBL_COLOR
  menu_layer_set_highlight_colors(s_menu, accent_color(), gcolor_legible_over(accent_color()));
#endif
  if (s_pick_menu) {
    menu_layer_reload_data(s_pick_menu);
  }
}

void agent_picker_push(void) {
  if (g_state.agent_count == 0) {
    settings_menu_push();  // nothing to pick; settings explains why
    return;
  }
  // Pick up agent plugins installed or signed in since launch.
  app_send_command(CMD_REFRESH, 0, NULL);
  prv_pick_push(PICK_AGENT);
}

void settings_menu_push(void) {
  if (s_window) {
    return;
  }
  // Pick up plugins installed or signed in since launch.
  app_send_command(CMD_REFRESH, 0, NULL);
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_load,
    .appear = prv_appear,
    .disappear = prv_disappear,
    .unload = prv_unload,
  });
  window_stack_push(s_window, true);
}
