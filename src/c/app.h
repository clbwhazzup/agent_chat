#pragma once
#include <pebble.h>

// ---------------------------------------------------------------------------
// Shared state for Agent Chat.
//
// The phone (src/pkjs) is the source of truth for which agent plugin is active
// and which session is selected; the watch mirrors it here and caches the bits
// it needs to draw something useful the instant the app launches.
// ---------------------------------------------------------------------------

// Everything here starts from dictation, so a watch without a microphone has
// nothing to run. package.json leaves those platforms (aplite) out; this stops
// one being added back by accident.
#ifndef PBL_MICROPHONE
#error "Agent Chat needs a microphone; remove this platform from targetPlatforms."
#endif

// Watches with a touchscreen (the SDK defines PBL_TOUCH for them). Touch code
// is compiled only for these; at runtime app_touch_active() also checks the
// user's setting and whether the watch has touch turned on.
#ifdef PBL_TOUCH
#define HAS_TOUCH 1
#else
#define HAS_TOUCH 0
#endif

#define MAX_AGENTS    8
#define MAX_SESSIONS  24
#define NAME_LEN      32
#define STATUS_LEN    120
#define MESSAGE_LEN   512
#define REPLY_LEN     1024

// Capability bits, sent by the phone for the active agent plugin.
enum {
  CAP_SESSIONS = 1 << 0,  // plugin serves agent/chats: up/down switch sessions
  CAP_READ     = 1 << 1,  // plugin has a read_* action: replies come back
};

// Watch -> phone commands (MESSAGE_KEY_CMD).
enum {
  CMD_HELLO          = 1,
  CMD_SEND           = 2,  // TEXT = message, ARG = 1 to wait for a reply
  CMD_SELECT_AGENT   = 3,  // ARG = agent index
  CMD_SELECT_SESSION = 4,  // ARG = session index
  CMD_READ           = 5,  // fetch the latest reply now
  CMD_REFRESH        = 6,  // re-enumerate plugins and resend state
};

// Delivery state of the last message (MESSAGE_KEY_DELIVERY).
typedef enum {
  DELIVERY_NONE = 0,
  DELIVERY_SENDING,
  DELIVERY_SENT,
  DELIVERY_FAILED,
  DELIVERY_WAITING,   // sent; polling the plugin for a reply
  DELIVERY_REPLIED,
} Delivery;

typedef struct {
  // Mirrored from the phone.
  char agent_names[MAX_AGENTS][NAME_LEN];
  uint8_t agent_count;
  int8_t agent_active;               // -1 when none
  uint8_t caps;
  uint8_t accent;                    // GColor8 argb; 0 = default
  char session_names[MAX_SESSIONS][NAME_LEN];
  uint8_t session_count;
  uint8_t session_active;
  bool configured;
  char status[STATUS_LEN];           // setup problem to show, "" when fine

  // Conversation.
  Delivery delivery;
  char delivery_text[STATUS_LEN];
  char last_message[MESSAGE_LEN];
  char reply[REPLY_LEN];

  // Local.
  bool js_ready;
  bool phone_connected;
  bool listening;
  bool state_known;                  // phone has answered at least once this run
} AppState;

typedef struct {
  bool touch_enabled;
  bool confirm;                      // review transcription before sending
  bool listen_on_launch;
  bool wait_for_reply;
} Prefs;

extern AppState g_state;
extern Prefs g_prefs;

// main.c
void app_send_command(uint8_t cmd, int32_t arg, const char *text);
void app_start_listening(void);
void app_stop_listening(void);
void app_toggle_listening(void);
void app_select_session_delta(int delta);
void app_apply_touch_pref(void);
bool app_touch_active(void);  // touchscreen present, enabled on the watch, and on here
#if HAS_TOUCH
// Horizontal swipes mirror the buttons: the Back button is on the left, so a
// finger moving left-to-right is "back" and right-to-left is "select".
// Returns -1 for back, 1 for select, 0 when it isn't a horizontal swipe.
int touch_horizontal_swipe(int dx, int dy, int width);

// One touch subscription for the app, handed to whichever window is showing.
void app_touch_claim(TouchServiceHandler handler);
void app_touch_release(TouchServiceHandler handler);
#endif

// prefs.c
void prefs_load(void);
void prefs_save(void);
bool cache_load(void);  // false on first run
void cache_save(void);

// main_window.c
void main_window_push(void);
void main_window_refresh(void);
bool main_window_is_top(void);
GColor accent_color(void);

// settings_menu.c
void settings_menu_push(void);
void settings_menu_refresh(void);
void agent_picker_push(void);    // hold Select: switch agent

// reply_window.c
void reply_window_push(const char *text);
