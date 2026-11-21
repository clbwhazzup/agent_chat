// Agent Chat -- phone side.
//
// Finds installed agent plugins (anything with a send_to_agent action), keeps the
// watch told which one is active, what it can do and which sessions it has, and
// relays dictated messages to it with Pebble.invokeAction. For plugins with a
// read action it also polls for the agent's reply and forwards that.
//
// Plugins never get retried sends: they return a receipt, and a timeout may
// still have delivered (the plugins' own contracts forbid automatic retries).

var A = require('./agents');

var CMD = { HELLO: 1, SEND: 2, SELECT_AGENT: 3, SELECT_SESSION: 4, READ: 5, REFRESH: 6 };
var CAP = { SESSIONS: 1, READ: 2 };
var DELIVERY = { NONE: 0, SENDING: 1, SENT: 2, FAILED: 3, WAITING: 4, REPLIED: 5 };

var ACTION_TIMEOUT_MS = 25000;
var POLL_SCHEDULE_MS = [3000, 3000, 4000, 4000, 5000, 5000, 6000, 8000, 10000, 10000,
                        15000, 15000, 20000, 20000, 30000, 30000];  // about 3 minutes
var FOLLOW_WINDOW_MS = 2 * 60 * 1000;

var state = {
  supported: true,
  agents: [],
  active: null,
  chats: [],
  configured: { configured: null, name: '' },
  sessions: [],
  sessionIndex: 0,
  chatsSub: null,
  configuredSub: null,
  pollToken: 0,
  follow: null,       // { known: {id: true}, until } after sending to a "new chat"
};

function log(msg) {
  console.log('[agent_chat] ' + msg);
}

// ---------------------------------------------------------------- watch messaging

var outbox = [];
var outboxBusy = false;
var outboxRetries = 0;

function post(dict) {
  // Merge into a queued (not yet in flight) message; later values win.
  var last = outbox.length > (outboxBusy ? 1 : 0) ? outbox[outbox.length - 1] : null;
  if (last) {
    Object.keys(dict).forEach(function (k) { last[k] = dict[k]; });
  } else {
    outbox.push(dict);
  }
  pump();
}

function pump() {
  if (outboxBusy || !outbox.length) return;
  outboxBusy = true;
  Pebble.sendAppMessage(outbox[0], function () {
    outbox.shift();
    outboxBusy = false;
    outboxRetries = 0;
    pump();
  }, function (e) {
    outboxBusy = false;
    if (++outboxRetries > 3) {
      log('dropping message after retries');
      outbox.shift();
      outboxRetries = 0;
    }
    setTimeout(pump, 600);
  });
}

function deliver(delivery, text, reply) {
  var dict = { DELIVERY: delivery, DELIVERY_TEXT: A.clip(text || '', A.STATUS_MAX) };
  if (reply !== undefined) dict.REPLY = A.clip(reply, A.REPLY_MAX);
  post(dict);
}

function statusText() {
  if (!state.supported) {
    return 'Agent plugins need the latest Pebble app with Settings > Plugins on.';
  }
  if (!state.agents.length) {
    return 'No agent plugins. Install Claude or ChatGPT in the Pebble app and turn on Settings > Plugins.';
  }
  if (state.configured.configured === false) {
    return 'Set up ' + state.active.name + ': open its settings in the Pebble app.';
  }
  return '';
}

function caps() {
  var agent = state.active;
  if (!agent) return 0;
  return (agent.hasChats ? CAP.SESSIONS : 0) | (agent.readAction ? CAP.READ : 0);
}

function pushState() {
  var activeIndex = state.active ? state.agents.indexOf(state.active) : -1;
  post({
    AGENTS: state.agents.map(function (a) { return a.name; }).join('\n'),
    AGENT_ACTIVE: activeIndex,
    CAPS: caps(),
    ACCENT: state.active ? state.active.accent : 0,
    SESSIONS: state.sessions.map(function (s) { return s.name; }).join('\n'),
    SESSION_ACTIVE: state.sessionIndex,
    CONFIGURED: state.configured.configured === false ? 0 : 1,
    STATUS: A.clip(statusText(), A.STATUS_MAX),
  });
}

// ---------------------------------------------------------------- agents and sessions

/**
 * Is this still the active plugin? By uuid: every refresh builds new agent
 * objects, and subscriptions made before it must keep delivering.
 */
function isActive(agent) {
  return !!(state.active && agent && state.active.uuid === agent.uuid);
}

function sessionKey(agent) {
  return 'session:' + agent.uuid;
}

function currentSession() {
  return state.sessions[state.sessionIndex] || { id: null, name: 'Default' };
}

function rebuildSessions() {
  var agent = state.active;
  if (!agent) {
    state.sessions = [];
    state.sessionIndex = 0;
    return;
  }
  state.sessions = A.buildSessions(agent, state.chats, state.configured);
  var saved = localStorage.getItem(sessionKey(agent));
  var index = 0;
  if (saved) {
    state.sessions.forEach(function (s, i) { if (s.id === saved) index = i; });
  }
  state.sessionIndex = index;
}

function selectSession(index) {
  if (!state.active || index < 0 || index >= state.sessions.length) return;
  state.sessionIndex = index;
  state.follow = null;
  state.pollToken++;
  var id = state.sessions[index].id;
  if (id) localStorage.setItem(sessionKey(state.active), id);
  else localStorage.removeItem(sessionKey(state.active));
}

function unsubscribe(sub) {
  try {
    if (sub) sub.unsubscribe();
  } catch (e) { /* already gone */ }
}

function unsubscribeAll() {
  unsubscribe(state.chatsSub);
  unsubscribe(state.configuredSub);
  state.chatsSub = null;
  state.configuredSub = null;
}

/** (Re)subscribes to agent/chats; a fresh subscription reads the list afresh. */
function subscribeChats(agent) {
  unsubscribe(state.chatsSub);
  state.chatsSub = Pebble.subscribeToSource({
    category: 'agent',
    item: 'chats',
    plugin: agent.uuid,
    properties: ['name', 'title'],
    limit: 40,
    onData: function (envelope) {
      if (!isActive(agent)) return;
      state.chats = A.parseChats(envelope);
      followNewChat();
      rebuildSessions();
      pushState();
    },
    onError: function (err) { log(agent.name + ' agent/chats: ' + (err && err.code)); },
  });
}

function subscribe(agent) {
  unsubscribeAll();
  if (agent.hasChats) subscribeChats(agent);
  if (agent.hasConfigured) {
    state.configuredSub = Pebble.subscribeToSource({
      category: 'agent',
      item: 'configured',
      plugin: agent.uuid,
      properties: ['name', 'configured'],
      onData: function (envelope) {
        if (!isActive(agent)) return;
        state.configured = A.parseConfigured(envelope);
        rebuildSessions();
        pushState();
      },
      onError: function (err) { log(agent.name + ' agent/configured: ' + (err && err.code)); },
    });
  }
}

/** After a send that started a new chat, switch to that chat once it is listed. */
function followNewChat() {
  var follow = state.follow;
  if (!follow) return;
  if (Date.now() > follow.until) {
    state.follow = null;
    return;
  }
  // The plugin may say which chat it made; otherwise take the first new one.
  var fresh = state.chats.filter(function (c) {
    return follow.id ? c.id === follow.id : !follow.known[c.id];
  })[0];
  if (!fresh) return;
  state.follow = null;
  localStorage.setItem(sessionKey(state.active), fresh.id);
  log('following new chat ' + fresh.name);
}

function startFollowing(createdId) {
  var known = {};
  state.chats.forEach(function (c) { known[c.id] = true; });
  state.follow = { known: known, until: Date.now() + FOLLOW_WINDOW_MS,
                   id: typeof createdId === 'string' ? createdId : null };
  var agent = state.active;
  // Sources refresh on their own schedule; resubscribe to ask for a fresh list.
  [4000, 15000].forEach(function (delay) {
    setTimeout(function () {
      if (!state.follow || !isActive(agent) || !agent.hasChats) return;
      subscribeChats(agent);
    }, delay);
  });
}

function activate(agent) {
  state.active = agent;
  state.chats = [];
  state.configured = { configured: null, name: '' };
  state.follow = null;
  state.pollToken++;
  if (agent) {
    localStorage.setItem('agent', agent.uuid);
    subscribe(agent);
  } else {
    unsubscribeAll();
  }
  rebuildSessions();
}

/** One line per installed plugin: what agent detection sees. */
function describePlugins(plugins) {
  var lines = [plugins.length + ' plugin(s) enumerated'];
  plugins.forEach(function (p) {
    var actions = (p.actions || []).map(function (a) { return a && a.name; });
    var sources = (p.sources || []).map(function (s) {
      return s && s.category + '/' + (s.items || []).join(',');
    });
    lines.push('  ' + p.name + ' ' + p.uuid + ' actions=[' + actions.join(' ') + '] sources=[' +
               sources.join(' ') + '] keys=' + Object.keys(p).join(','));
  });
  return lines.join('\n');
}

function refreshAgents(refetchChats) {
  log('enumeratePlugins: ' + typeof Pebble.enumeratePlugins);
  if (typeof Pebble.enumeratePlugins !== 'function') {
    state.supported = false;
    state.agents = [];
    activate(null);
    pushState();
    return;
  }
  var plugins = [];
  try {
    plugins = Pebble.enumeratePlugins() || [];
  } catch (e) {
    log('enumeratePlugins failed: ' + e);
  }
  state.supported = true;
  log(describePlugins(plugins));
  state.agents = A.findAgents(plugins);
  var wanted = (state.active && state.active.uuid) || localStorage.getItem('agent');
  var next = state.agents.filter(function (a) { return a.uuid === wanted; })[0] || state.agents[0] || null;
  if (!next || !state.active || next.uuid !== state.active.uuid) {
    activate(next);
  } else {
    // Same plugin, possibly updated: keep its subscriptions and selection.
    state.active = next;
    rebuildSessions();
    // Opening settings asks for chats started since (a new Remote Control session, say).
    if (refetchChats && next.hasChats) subscribeChats(next);
  }
  log(state.agents.length + ' agent plugin(s); active: ' + (state.active ? state.active.name : 'none'));
  pushState();
}

// ---------------------------------------------------------------- actions

function invoke(agent, action, args) {
  var call = Pebble.invokeAction({ plugin: agent.uuid, action: action, args: args });
  var timeout = new Promise(function (resolve) {
    setTimeout(function () {
      resolve({ ok: false, code: 'TIMEOUT', message: '' });
    }, ACTION_TIMEOUT_MS);
  });
  return Promise.race([call, timeout]).then(function (result) {
    return result || { ok: false, code: 'UNKNOWN' };
  }, function (e) {
    return { ok: false, code: (e && e.code) || 'UNKNOWN', message: (e && e.message) || String(e) };
  });
}

function readReply(agent, session) {
  return invoke(agent, agent.readAction.name, A.readArgs(agent.readAction, session));
}

function pollForReply(agent, session, baseline, token) {
  var step = 0;
  function next() {
    if (token !== state.pollToken) return;
    if (step >= POLL_SCHEDULE_MS.length) {
      deliver(DELIVERY.SENT, 'Sent. No reply yet; hold Select to check.');
      return;
    }
    setTimeout(function () {
      if (token !== state.pollToken) return;
      readReply(agent, session).then(function (result) {
        if (token !== state.pollToken) return;
        if (result.ok && A.isNewReply(baseline, result.text)) {
          deliver(DELIVERY.REPLIED, 'Reply from ' + agent.name, result.text);
          return;
        }
        if (!result.ok && result.code === 'PERMISSION_DENIED') {
          deliver(DELIVERY.SENT, 'Sent. ' + agent.name + ' won\'t share replies with this app.');
          return;
        }
        next();
      });
    }, POLL_SCHEDULE_MS[step++]);
  }
  next();
}

function send(message, waitForReply) {
  var agent = state.active;
  if (!agent) {
    deliver(DELIVERY.FAILED, statusText() || 'No agent plugin.');
    return;
  }
  var session = currentSession();
  var token = ++state.pollToken;
  // Agents that answer inline take as long as the model does.
  deliver(agent.inlineReplies ? DELIVERY.WAITING : DELIVERY.SENDING,
          agent.inlineReplies ? 'Waiting for ' + agent.name + '...' : 'Sending to ' + session.name + '...');

  var wantsReply = !!(waitForReply && agent.readAction);
  // Read what is there now, so the poll can tell the new reply apart.
  var baseline = wantsReply
    ? readReply(agent, session).then(function (r) { return r.ok ? r.text : null; })
    : Promise.resolve(null);

  // The message goes to the session it was dictated for, even if the user has
  // switched since; only the reply polling is tied to the current session.
  baseline.then(function (before) {
    return invoke(agent, A.SEND_ACTION, A.sendArgs(message, session)).then(function (result) {
      log('send_to_agent ' + agent.name + ': ' + (result.ok ? 'ok' : result.code));
      if (!result.ok) {
        deliver(DELIVERY.FAILED, A.errorText(agent.name, result));
        return;
      }
      if (A.startsNewChat(session) && agent.hasChats) startFollowing(result.chat_id);
      if (A.isInlineReply(agent, result)) {
        deliver(DELIVERY.REPLIED, 'Reply from ' + agent.name, String(result.text));
      } else if (wantsReply && token === state.pollToken) {
        deliver(DELIVERY.WAITING, 'Sent. Waiting for reply...');
        pollForReply(agent, session, before, token);
      } else {
        deliver(DELIVERY.SENT, 'Sent to ' + (session.id ? session.name : agent.name));
      }
    });
  });
}

function readNow() {
  var agent = state.active;
  if (!agent || !agent.readAction) return;
  var token = ++state.pollToken;
  readReply(agent, currentSession()).then(function (result) {
    if (token !== state.pollToken) return;
    if (result.ok) {
      deliver(DELIVERY.REPLIED, 'Reply from ' + agent.name, result.text || 'No replies yet.');
    } else {
      deliver(DELIVERY.FAILED, A.errorText(agent.name, result));
    }
  });
}

// ---------------------------------------------------------------- events

Pebble.addEventListener('ready', function () {
  log('ready');
  refreshAgents();
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload || {};
  var arg = p.ARG || 0;
  switch (p.CMD) {
    case CMD.HELLO:
      refreshAgents(false);
      break;
    case CMD.REFRESH:
      refreshAgents(true);
      break;
    case CMD.SEND:
      if (p.TEXT) send(String(p.TEXT), arg === 1);
      break;
    case CMD.SELECT_AGENT:
      if (state.agents[arg] && state.agents[arg] !== state.active) {
        activate(state.agents[arg]);
      }
      pushState();
      break;
    case CMD.SELECT_SESSION:
      selectSession(arg);
      pushState();
      break;
    case CMD.READ:
      readNow();
      break;
  }
});
