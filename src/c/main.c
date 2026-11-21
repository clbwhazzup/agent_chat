#include "app.h"

// ---------------------------------------------------------------------------
// main.c -- entry point, phone messaging and dictation.
//
// The watch never talks to an agent directly. It dictates, hands the text to
// PebbleKit JS (src/pkjs/index.js), and that calls the active agent plugin's
// send_to_agent action through the Pebble app. Everything about which plugins
// exist and what they can do arrives from the phone as AppMessages.
// ---------------------------------------------------------------------------

AppState g_state;
Prefs g_prefs;

static DictationSession *s_dictation;
static bool s_send_pending;      // dictated before PebbleKit JS was up

// ---------------------------------------------------------------------------
// Outgoing commands
//
// A tiny queue so a session switch made while a send is in flight is not lost.
// CMD_SEND carries g_state.last_message rather than a copy.
// ---------------------------------------------------------------------------

#define QUEUE_LEN 4
#define MAX_RETRIES 2

typedef struct {
  uint8_t cmd;
  int32_t arg;
  uint8_t retries;
} Command;

static Command s_queue[QUEUE_LEN];
static uint8_t s_queue_count;
static bool s_in_flight;
static AppTimer *s_retry_timer;

static void prv_pump(void);

static void prv_retry_timer(void *context) {
  s_retry_timer = NULL;
  prv_pump();
}

static void prv_pump(void) {
  if (s_in_flight || s_queue_count == 0) {
    return;
  }
  const Command *c = &s_queue[0];
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) {
    if (!s_retry_timer) {
      s_retry_timer = app_timer_register(250, prv_retry_timer, NULL);
    }
    return;
  }
  dict_write_uint8(out, MESSAGE_KEY_CMD, c->cmd);
  dict_write_int32(out, MESSAGE_KEY_ARG, c->arg);
  if (c->cmd == CMD_SEND) {
    dict_write_cstring(out, MESSAGE_KEY_TEXT, g_state.last_message);
  }
  if (app_message_outbox_send() == APP_MSG_OK) {
    s_in_flight = true;
  } else if (!s_retry_timer) {
    s_retry_timer = app_timer_register(250, prv_retry_timer, NULL);
  }
}

static void prv_dequeue(void) {
  if (s_queue_count == 0) {
    return;
  }
  memmove(&s_queue[0], &s_queue[1], sizeof(Command) * (s_queue_count - 1));
  s_queue_count--;
}

void app_send_command(uint8_t cmd, int32_t arg, const char *text) {
  (void)text;  // CMD_SEND always sends g_state.last_message
  // A newer selection supersedes a queued one of the same kind.
  for (uint8_t i = s_in_flight ? 1 : 0; i < s_queue_count; i++) {
    if (s_queue[i].cmd == cmd && cmd != CMD_SEND) {
      s_queue[i].arg = arg;
      return;
    }
  }
  if (s_queue_count == QUEUE_LEN) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "command queue full, dropping %d", cmd);
    return;
  }
  s_queue[s_queue_count++] = (Command) { .cmd = cmd, .arg = arg };
  prv_pump();
}

static void prv_outbox_sent(DictionaryIterator *iter, void *context) {
  s_in_flight = false;
  prv_dequeue();
  prv_pump();
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  s_in_flight = false;
  APP_LOG(APP_LOG_LEVEL_WARNING, "outbox failed: %d", (int)reason);
  if (s_queue_count == 0) {
    return;
  }
  Command *c = &s_queue[0];
  // Nothing reached the phone, so resending cannot double-deliver to the agent.
  if (c->retries < MAX_RETRIES) {
    c->retries++;
    if (!s_retry_timer) {
      s_retry_timer = app_timer_register(500, prv_retry_timer, NULL);
    }
    return;
  }
  if (c->cmd == CMD_SEND) {
    g_state.delivery = DELIVERY_FAILED;
    strncpy(g_state.delivery_text, "Couldn't reach the phone. Not sent.", STATUS_LEN - 1);
    main_window_refresh();
  }
  prv_dequeue();
  prv_pump();
}

// ---------------------------------------------------------------------------
// Incoming state
// ---------------------------------------------------------------------------

// Copies at most `size - 1` bytes without cutting a UTF-8 sequence in half.
static void prv_copy_utf8(char *dst, const char *src, size_t len, size_t size) {
  if (len > size - 1) {
    len = size - 1;
    while (len > 0 && ((uint8_t)src[len] & 0xC0) == 0x80) {
      len--;
    }
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

// Splits a '\n'-joined list into fixed-size name slots.
static uint8_t prv_split(const char *joined, char names[][NAME_LEN], uint8_t max) {
  uint8_t count = 0;
  const char *p = joined;
  while (*p && count < max) {
    const char *end = strchr(p, '\n');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    prv_copy_utf8(names[count++], p, len, NAME_LEN);
    if (!end) {
      break;
    }
    p = end + 1;
  }
  return count;
}

static void prv_copy_tuple(char *dst, const Tuple *t, size_t size) {
  const char *s = t->value->cstring;
  size_t len = 0;
  while (len < t->length && s[len]) {
    len++;
  }
  prv_copy_utf8(dst, s, len, size);
}

static void prv_flush_pending_send(void);

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  bool was_ready = g_state.js_ready;
  g_state.js_ready = true;
  g_state.state_known = true;

  Tuple *t;
  if ((t = dict_find(iter, MESSAGE_KEY_AGENTS))) {
    g_state.agent_count = prv_split(t->value->cstring, g_state.agent_names, MAX_AGENTS);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_AGENT_ACTIVE))) {
    g_state.agent_active = (int8_t)t->value->int32;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_CAPS))) {
    g_state.caps = (uint8_t)t->value->int32;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ACCENT))) {
    g_state.accent = (uint8_t)t->value->int32;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_SESSIONS))) {
    g_state.session_count = prv_split(t->value->cstring, g_state.session_names, MAX_SESSIONS);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_SESSION_ACTIVE))) {
    g_state.session_active = (uint8_t)t->value->int32;
  }
  if (g_state.session_active >= g_state.session_count) {
    g_state.session_active = 0;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_CONFIGURED))) {
    g_state.configured = t->value->int32 != 0;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_STATUS))) {
    prv_copy_tuple(g_state.status, t, STATUS_LEN);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_DELIVERY))) {
    g_state.delivery = (Delivery)t->value->int32;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_DELIVERY_TEXT))) {
    prv_copy_tuple(g_state.delivery_text, t, STATUS_LEN);
  }

  bool new_reply = false;
  if ((t = dict_find(iter, MESSAGE_KEY_REPLY))) {
    prv_copy_tuple(g_state.reply, t, REPLY_LEN);
    new_reply = g_state.reply[0] != '\0';
  }

  if (dict_find(iter, MESSAGE_KEY_AGENTS) || dict_find(iter, MESSAGE_KEY_SESSIONS) ||
      dict_find(iter, MESSAGE_KEY_SESSION_ACTIVE) || dict_find(iter, MESSAGE_KEY_CONFIGURED)) {
    cache_save();
  }

  main_window_refresh();
  settings_menu_refresh();

  if (new_reply) {
    vibes_short_pulse();
    if (main_window_is_top() && !g_state.listening) {
      reply_window_push(g_state.reply);
    }
  }

  if (!was_ready) {
    prv_flush_pending_send();
  }
}

static void prv_inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "inbox dropped: %d", (int)reason);
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

static void prv_send_message(void) {
  g_state.reply[0] = '\0';
  g_state.delivery = DELIVERY_SENDING;
  strncpy(g_state.delivery_text, "Sending...", STATUS_LEN - 1);
  if (!g_state.js_ready) {
    // PebbleKit JS starts a moment after the app; hold the message for it.
    s_send_pending = true;
    main_window_refresh();
    return;
  }
  app_send_command(CMD_SEND, g_prefs.wait_for_reply ? 1 : 0, NULL);
  main_window_refresh();
}

static void prv_flush_pending_send(void) {
  if (s_send_pending) {
    s_send_pending = false;
    app_send_command(CMD_SEND, g_prefs.wait_for_reply ? 1 : 0, NULL);
  }
}

// ---------------------------------------------------------------------------
// Dictation
// ---------------------------------------------------------------------------

static void prv_dictation_status(DictationSession *session, DictationSessionStatus status,
                                 char *transcription, void *context) {
  g_state.listening = false;
  switch (status) {
    case DictationSessionStatusSuccess:
      if (transcription && transcription[0]) {
        prv_copy_utf8(g_state.last_message, transcription, strlen(transcription), MESSAGE_LEN);
        prv_send_message();
        return;
      }
      break;
    case DictationSessionStatusFailureConnectivityError:
      g_state.delivery = DELIVERY_FAILED;
      strncpy(g_state.delivery_text, "Voice needs the phone and internet.", STATUS_LEN - 1);
      break;
    case DictationSessionStatusFailureDisabled:
      g_state.delivery = DELIVERY_FAILED;
      strncpy(g_state.delivery_text, "Voice is turned off in the Pebble app.", STATUS_LEN - 1);
      break;
    default:
      // Cancelled, nothing heard, or rejected: just stop listening.
      break;
  }
  main_window_refresh();
}

void app_start_listening(void) {
  if (g_state.listening) {
    return;
  }
  if (!s_dictation) {
    s_dictation = dictation_session_create(MESSAGE_LEN, prv_dictation_status, NULL);
  }
  if (!s_dictation) {
    g_state.delivery = DELIVERY_FAILED;
    strncpy(g_state.delivery_text, "Voice isn't available. Is the phone connected?",
            STATUS_LEN - 1);
    main_window_refresh();
    return;
  }
  dictation_session_enable_confirmation(s_dictation, g_prefs.confirm);
  if (dictation_session_start(s_dictation) == DictationSessionStatusSuccess) {
    g_state.listening = true;
  }
  main_window_refresh();
}

void app_stop_listening(void) {
  if (s_dictation && g_state.listening) {
    dictation_session_stop(s_dictation);
  }
  g_state.listening = false;
  main_window_refresh();
}

void app_toggle_listening(void) {
  if (g_state.listening) {
    app_stop_listening();
  } else {
    app_start_listening();
  }
}

// ---------------------------------------------------------------------------
// Sessions and preferences
// ---------------------------------------------------------------------------

void app_select_session_delta(int delta) {
  if (!(g_state.caps & CAP_SESSIONS) || g_state.session_count < 2) {
    return;
  }
  int count = g_state.session_count;
  g_state.session_active = (uint8_t)(((int)g_state.session_active + delta + count) % count);
  // Messages and replies belong to the session they were sent in.
  g_state.last_message[0] = '\0';
  g_state.reply[0] = '\0';
  g_state.delivery = DELIVERY_NONE;
  g_state.delivery_text[0] = '\0';
  app_send_command(CMD_SELECT_SESSION, g_state.session_active, NULL);
  cache_save();
  main_window_refresh();
}

#if HAS_TOUCH
static TouchServiceHandler s_touch_owner;

int touch_horizontal_swipe(int dx, int dy, int width) {
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;
  if (adx < width / 5 || adx <= ady) {
    return 0;
  }
  int kind = dx > 0 ? -1 : 1;
  APP_LOG(APP_LOG_LEVEL_DEBUG, "swipe dx=%d dy=%d -> %s", dx, dy, kind < 0 ? "back" : "select");
  return kind;
}

void app_touch_claim(TouchServiceHandler handler) {
  // Never hold the sensor on when touch is off here or on the watch.
  if (!handler || !app_touch_active()) {
    app_touch_release(s_touch_owner);
    return;
  }
  touch_service_subscribe(handler, NULL);  // replaces the previous owner
  s_touch_owner = handler;
}

// Only the current owner can let go, so a window disappearing after the next
// one appeared doesn't cut that one off.
void app_touch_release(TouchServiceHandler handler) {
  if (handler && s_touch_owner == handler) {
    touch_service_unsubscribe();
    s_touch_owner = NULL;
  }
}
#endif

bool app_touch_active(void) {
#if HAS_TOUCH
  return g_prefs.touch_enabled && touch_service_is_enabled();
#else
  return false;
#endif
}

void app_apply_touch_pref(void) {
  // Lets the system map taps and swipes onto the menus and scroll views.
#if HAS_TOUCH
  app_touch_navigation_enable(g_prefs.touch_enabled);
#endif
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void prv_connection(bool connected) {
  g_state.phone_connected = connected;
  if (connected) {
    app_send_command(CMD_HELLO, 0, NULL);
  }
  main_window_refresh();
}

static void prv_init(void) {
  memset(&g_state, 0, sizeof(g_state));
  g_state.agent_active = -1;
  prefs_load();
  bool cached = cache_load();
  g_state.phone_connected = connection_service_peek_pebble_app_connection();

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_register_outbox_failed(prv_outbox_failed);
  uint32_t inbox = app_message_inbox_size_maximum();
  if (inbox > 2048) {
    inbox = 2048;
  }
  app_message_open(inbox, MESSAGE_LEN + 64);

  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = prv_connection,
  });

  app_apply_touch_pref();

  // Listen right away unless we already know there is nothing to send to.
  bool listen = g_prefs.listen_on_launch && (!cached || g_state.agent_count > 0);
  main_window_push();
  if (listen) {
    app_start_listening();
  }

  app_send_command(CMD_HELLO, 0, NULL);
}

static void prv_deinit(void) {
  if (s_dictation) {
    dictation_session_destroy(s_dictation);
  }
  connection_service_unsubscribe();
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
