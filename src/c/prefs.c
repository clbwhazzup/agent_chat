#include "app.h"

// ---------------------------------------------------------------------------
// Persistence: user preferences, plus a small cache of the last state the
// phone reported so the first frame after launch already names the agent and
// session instead of waiting a second or two for PebbleKit JS to start.
// ---------------------------------------------------------------------------

#define PERSIST_PREFS 1
#define PERSIST_CACHE 2
#define PREFS_VERSION 1
#define CACHE_VERSION 1

typedef struct {
  uint8_t version;
  uint8_t touch_enabled;
  uint8_t confirm;
  uint8_t listen_on_launch;
  uint8_t wait_for_reply;
} StoredPrefs;

typedef struct {
  uint8_t version;
  uint8_t has_agent;
  uint8_t caps;
  uint8_t accent;
  uint8_t configured;
  char agent[NAME_LEN];
  char session[NAME_LEN];
} StoredCache;

void prefs_load(void) {
  g_prefs = (Prefs) {
    .touch_enabled = true,
    .confirm = false,
    .listen_on_launch = true,
    .wait_for_reply = true,
  };
  StoredPrefs stored;
  if (persist_read_data(PERSIST_PREFS, &stored, sizeof(stored)) == (int)sizeof(stored) &&
      stored.version == PREFS_VERSION) {
    g_prefs.touch_enabled = stored.touch_enabled;
    g_prefs.confirm = stored.confirm;
    g_prefs.listen_on_launch = stored.listen_on_launch;
    g_prefs.wait_for_reply = stored.wait_for_reply;
  }
}

void prefs_save(void) {
  StoredPrefs stored = {
    .version = PREFS_VERSION,
    .touch_enabled = g_prefs.touch_enabled,
    .confirm = g_prefs.confirm,
    .listen_on_launch = g_prefs.listen_on_launch,
    .wait_for_reply = g_prefs.wait_for_reply,
  };
  persist_write_data(PERSIST_PREFS, &stored, sizeof(stored));
}

bool cache_load(void) {
  StoredCache stored;
  if (persist_read_data(PERSIST_CACHE, &stored, sizeof(stored)) != (int)sizeof(stored) ||
      stored.version != CACHE_VERSION) {
    return false;
  }
  stored.agent[NAME_LEN - 1] = '\0';
  stored.session[NAME_LEN - 1] = '\0';
  g_state.caps = stored.caps;
  g_state.accent = stored.accent;
  g_state.configured = stored.configured;
  if (stored.has_agent) {
    strncpy(g_state.agent_names[0], stored.agent, NAME_LEN);
    g_state.agent_count = 1;
    g_state.agent_active = 0;
    strncpy(g_state.session_names[0], stored.session, NAME_LEN);
    g_state.session_count = stored.session[0] ? 1 : 0;
    g_state.session_active = 0;
  }
  return true;
}

void cache_save(void) {
  StoredCache stored = {
    .version = CACHE_VERSION,
    .caps = g_state.caps,
    .accent = g_state.accent,
    .configured = g_state.configured,
  };
  if (g_state.agent_active >= 0 && g_state.agent_active < g_state.agent_count) {
    stored.has_agent = 1;
    strncpy(stored.agent, g_state.agent_names[g_state.agent_active], NAME_LEN - 1);
  }
  if (g_state.session_active < g_state.session_count) {
    strncpy(stored.session, g_state.session_names[g_state.session_active], NAME_LEN - 1);
  }
  persist_write_data(PERSIST_CACHE, &stored, sizeof(stored));
}
