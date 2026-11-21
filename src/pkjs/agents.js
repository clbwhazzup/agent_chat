// Pure helpers for talking to Index agent plugins (category `agent`).
//
// No Pebble globals in here, so test/agents.test.js can run it under Node.
//
// The agent contract (see coredevices/pebble-plugins):
//   action  send_to_agent({ message, chat_id? })  -> receipt, never the answer
//   source  agent/chats       one instance per chat; instanceId is the chat_id
//   source  agent/configured  { configured, name }: would a send with no chat_id work
// A plugin that can also return the agent's answer exposes a read action next to
// send_to_agent (Grok Bot's `read_grok_bot`); those get two-way messaging.

var SEND_ACTION = 'send_to_agent';
var NAME_MAX = 30;
var STATUS_MAX = 118;
var REPLY_MAX = 1000;

// Per-plugin touches that the manifest can't express.
var KNOWN = {
  // Claude: agent/chats has no "new chat" entry, but chat_id "new" starts one.
  '0d33014e-5d56-42ea-9665-3ebf182b4cfb': { accent: '#FFAA55', newChat: 'new' },
  // ChatGPT: serves its own "new-chat" instance in agent/chats.
  '3f97d5c5-bc28-48ba-b4af-e7ad7f6000f0': { accent: '#10A37F', newChat: 'new-chat' },
  // Universal AI (Eric McCormick): calls provider APIs with the user's own keys
  // and returns the whole answer from send_to_agent instead of a receipt.
  // "new" starts a conversation; agent/chats lists a "default" entry.
  '2c5edc42-b096-4f76-b284-1f27907e6501': { newChat: 'new', inlineReplies: true },
};

// agent/chats instanceIds that mean "the default conversation", which the
// session list already has as its first entry (send with no chat_id).
var DEFAULT_IDS = ['default'];

// chat_ids that start a fresh conversation on every send.
var NEW_CHAT_IDS = ['new', 'new chat', 'new-chat'];

// Preferred names for a generic read action, before falling back to any read_*.
var READ_NAMES = ['read_agent', 'read_from_agent', 'read_replies', 'read_reply'];
var READ_ARGS = ['chat_id', 'limit'];

function actionsOf(plugin) {
  return (plugin && plugin.actions) || [];
}

function sourcesOf(plugin) {
  return (plugin && plugin.sources) || [];
}

function servesItem(plugin, item) {
  return sourcesOf(plugin).some(function (source) {
    return source.category === 'agent' && (source.items || []).indexOf(item) !== -1;
  });
}

/** A read action we know how to call: only optional chat_id / limit arguments. */
function usableReadAction(action) {
  if (!action || typeof action.name !== 'string' || action.name.indexOf('read_') !== 0) {
    return false;
  }
  var params = action.parameters || {};
  var required = params.required || [];
  return required.every(function (name) { return READ_ARGS.indexOf(name) !== -1 && name !== 'limit'; });
}

function readActionOf(plugin) {
  var actions = actionsOf(plugin).filter(usableReadAction);
  for (var i = 0; i < READ_NAMES.length; i++) {
    for (var j = 0; j < actions.length; j++) {
      if (actions[j].name === READ_NAMES[i]) return actions[j];
    }
  }
  return actions[0] || null;
}

/** "#D97757" -> the nearest Pebble GColor8 argb byte. */
function accentByte(hex) {
  var m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex || '');
  if (!m) return 0;
  function level(h) { return Math.round(parseInt(h, 16) / 85); }
  return 0xC0 | (level(m[1]) << 4) | (level(m[2]) << 2) | level(m[3]);
}

/** Installed plugins that accept send_to_agent, sorted by name. */
function findAgents(plugins) {
  return (plugins || []).filter(function (plugin) {
    return actionsOf(plugin).some(function (action) { return action.name === SEND_ACTION; });
  }).map(function (plugin) {
    var known = KNOWN[String(plugin.uuid).toLowerCase()] || {};
    return {
      uuid: plugin.uuid,
      name: cleanName(plugin.name || 'Agent'),
      hasChats: servesItem(plugin, 'chats'),
      hasConfigured: servesItem(plugin, 'configured'),
      readAction: readActionOf(plugin),
      accent: accentByte(known.accent),
      newChat: known.newChat || null,
      inlineReplies: !!known.inlineReplies,
    };
  }).sort(function (a, b) {
    return a.name.toLowerCase() < b.name.toLowerCase() ? -1 : 1;
  });
}

function textOf(property) {
  if (!property) return '';
  if (property.shortText && property.shortText.text) return String(property.shortText.text);
  if (property.longText && property.longText.text) return String(property.longText.text);
  return '';
}

/** agent/configured envelope -> { configured, name } (configured is null when unknown). */
function parseConfigured(envelope) {
  var instance = envelope && envelope.instances && envelope.instances[0];
  if (!instance) return { configured: null, name: '' };
  var p = instance.properties || {};
  var configured = null;
  if (p.configured && p.configured.boolean) configured = !!p.configured.boolean.value;
  return { configured: configured, name: textOf(p.name) };
}

/** agent/chats instances -> [{ id, name }]. */
function parseChats(envelope) {
  return ((envelope && envelope.instances) || []).map(function (instance) {
    var p = instance.properties || {};
    return { id: String(instance.instanceId), name: cleanName(textOf(p.name) || textOf(p.title) || 'Chat') };
  });
}

function cleanName(name) {
  var s = String(name || '').replace(/\s+/g, ' ').trim();
  return s.length > NAME_MAX ? s.slice(0, NAME_MAX - 1) + '…' : s;
}

/**
 * The list up/down walks through. First the plugin's own default (send with no
 * chat_id), then a "new chat" entry if the plugin has one that agent/chats
 * doesn't list, then the recent chats.
 *
 * Chats are distinct by id, so two chats with the same title are both kept.
 * Names are only used to drop the one chat that *is* the default (the plugin
 * names its default after it), so it isn't listed twice.
 */
function buildSessions(agent, chats, configured, max) {
  chats = chats || [];
  var defaultName = cleanName((configured && configured.name) || 'Default');
  var sessions = [{ id: null, name: defaultName }];
  var sameName = function (name) { return name.toLowerCase() === defaultName.toLowerCase(); };
  if (agent && agent.newChat && !sameName('New chat') &&
      !chats.some(function (c) { return c.id === agent.newChat; })) {
    sessions.push({ id: agent.newChat, name: 'New chat' });
  }
  var seenIds = {};
  var defaultDropped = false;
  chats.forEach(function (chat) {
    var id = String(chat.id);
    if (DEFAULT_IDS.indexOf(id.toLowerCase()) !== -1 || seenIds[id]) return;
    seenIds[id] = true;
    if (!defaultDropped && sameName(chat.name)) {
      defaultDropped = true;
      return;
    }
    sessions.push({ id: chat.id, name: chat.name });
  });
  return sessions.slice(0, max || 24);
}

/** Does sending to this session start a new conversation each time? */
function startsNewChat(session) {
  if (!session) return false;
  if (session.id !== null && session.id !== undefined) {
    return NEW_CHAT_IDS.indexOf(String(session.id).toLowerCase()) !== -1;
  }
  return /^new\b.*\bchat$/i.test(session.name);
}

/** Only the arguments the action declares, so plugins that reject extras accept the call. */
function argsFor(action, args) {
  var declared = action && action.parameters && action.parameters.properties;
  if (!declared) return args;
  var out = {};
  Object.keys(args).forEach(function (key) {
    if (args[key] !== undefined && args[key] !== null && declared[key]) out[key] = args[key];
  });
  return out;
}

function sendArgs(message, session) {
  var args = { message: message };
  if (session && session.id) args.chat_id = session.id;
  return args;
}

function readArgs(action, session) {
  return argsFor(action, { limit: 1, chat_id: session && session.id ? session.id : undefined });
}

function clip(text, max) {
  var s = String(text || '').trim();
  return s.length > max ? s.slice(0, max - 1) + '…' : s;
}

/** A short, actionable line for an invokeAction failure. */
function errorText(agentName, result) {
  var code = (result && result.code) || 'UNKNOWN';
  var message = result && result.message;
  var text;
  switch (code) {
    case 'AUTH_REQUIRED':
      text = 'Set up ' + agentName + ': open its settings in the Pebble app.';
      break;
    case 'PERMISSION_DENIED':
      text = 'Allow Agent Chat to use ' + agentName + ' in the Pebble app, then try again.';
      break;
    case 'RATE_LIMITED':
      text = agentName + ' is rate limiting. Try again later.';
      break;
    case 'TIMEOUT':
      text = 'No receipt from ' + agentName + '. It may have sent; check before resending.';
      break;
    case 'PLUGIN_UNAVAILABLE':
      text = forWatch(message) || agentName + ' is unavailable right now.';
      break;
    default:
      text = forWatch(message) || agentName + ' failed (' + code + ').';
  }
  return clip(text, STATUS_MAX);
}

/**
 * Plugin errors are written for the Index agent ("... do not automatically
 * retry."). On a watch only the first sentence fits, plus the one instruction
 * that matters to a person: look before sending again.
 */
function forWatch(message) {
  if (!message) return '';
  var text = String(message).trim();
  var first = /^.*?[.!?](?=\s|$)/.exec(text);
  var short = first ? first[0] : text;
  if (short !== text && /retry|before sending again|uncertain|may have/i.test(text)) {
    short += ' Check the chat before resending.';
  }
  return short;
}

/**
 * Did send_to_agent hand back the agent's answer rather than a receipt?
 * Plugins that do say so with reply_type "assistant" (if the host passes it
 * through); known ones are trusted even when it doesn't.
 */
function isInlineReply(agent, result) {
  if (!result || !result.ok || !result.text || !String(result.text).trim()) return false;
  return result.reply_type === 'assistant' || !!(agent && agent.inlineReplies);
}

/** Has a read returned something new since the baseline? */
function isNewReply(baseline, text) {
  if (!text) return false;
  return String(text).trim() !== String(baseline || '').trim();
}

module.exports = {
  SEND_ACTION: SEND_ACTION,
  STATUS_MAX: STATUS_MAX,
  REPLY_MAX: REPLY_MAX,
  accentByte: accentByte,
  findAgents: findAgents,
  readActionOf: readActionOf,
  parseConfigured: parseConfigured,
  parseChats: parseChats,
  buildSessions: buildSessions,
  startsNewChat: startsNewChat,
  sendArgs: sendArgs,
  readArgs: readArgs,
  errorText: errorText,
  isNewReply: isNewReply,
  isInlineReply: isInlineReply,
  clip: clip,
};
