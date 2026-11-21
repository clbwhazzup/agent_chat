#include "app.h"

// ---------------------------------------------------------------------------
// reply_window.c -- an agent's reply, for plugins that can read replies back.
//
//   Up / Down (or swipe)   scroll
//   Select (or tap)        close and start listening to answer
//   Back                   close
// ---------------------------------------------------------------------------

static Window *s_window;
static ScrollLayer *s_scroll;
static TextLayer *s_text;
static const char *s_body;

static void prv_reply_click(ClickRecognizerRef r, void *ctx) {
  window_stack_remove(s_window, true);
  app_start_listening();
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_reply_click);
}

static void prv_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  const bool big = bounds.size.w >= 200;
  const int pad = PBL_IF_ROUND_ELSE(bounds.size.w / 9, 4);

  s_scroll = scroll_layer_create(bounds);
  scroll_layer_set_click_config_onto_window(s_scroll, window);
  scroll_layer_set_callbacks(s_scroll, (ScrollLayerCallbacks) {
    .click_config_provider = prv_click_config,
  });
  scroll_layer_set_shadow_hidden(s_scroll, true);

  GFont font = fonts_get_system_font(big ? FONT_KEY_GOTHIC_24 : FONT_KEY_GOTHIC_18);
  GRect text_frame = GRect(pad, PBL_IF_ROUND_ELSE(bounds.size.h / 8, 2),
                           bounds.size.w - pad * 2, 4000);
  s_text = text_layer_create(text_frame);
  text_layer_set_font(s_text, font);
  text_layer_set_text(s_text, s_body);
  text_layer_set_overflow_mode(s_text, GTextOverflowModeWordWrap);
  text_layer_set_text_alignment(s_text, PBL_IF_ROUND_ELSE(GTextAlignmentCenter,
                                                          GTextAlignmentLeft));
  GSize size = text_layer_get_content_size(s_text);
  size.h += 8;
  text_layer_set_size(s_text, GSize(text_frame.size.w, size.h));
  scroll_layer_add_child(s_scroll, text_layer_get_layer(s_text));
  scroll_layer_set_content_size(s_scroll,
      GSize(bounds.size.w, size.h + text_frame.origin.y + PBL_IF_ROUND_ELSE(bounds.size.h / 6, 4)));
#ifdef PBL_ROUND
  text_layer_enable_screen_text_flow_and_paging(s_text, 4);
  scroll_layer_set_paging(s_scroll, true);
#endif
  layer_add_child(root, scroll_layer_get_layer(s_scroll));
}

static void prv_unload(Window *window) {
  text_layer_destroy(s_text);
  scroll_layer_destroy(s_scroll);
  window_destroy(s_window);
  s_window = NULL;
}

void reply_window_push(const char *text) {
  if (s_window || !text || !text[0]) {
    return;
  }
  s_body = text;
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_load,
    .unload = prv_unload,
  });
  window_stack_push(s_window, true);
}
