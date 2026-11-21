// Runs src/pkjs/index.js against a fake Pebble app with fake agent plugins.
var test = require('node:test');
var assert = require('node:assert');

var CHATS = { category: 'agent', items: ['chats'] };
var CONFIGURED = { category: 'agent', items: ['configured'] };
var SEND = { name: 'send_to_agent', parameters: { properties: { message: {}, chat_id: {} } } };
var CLAUDE = { uuid: '0d33014e-5d56-42ea-9665-3ebf182b4cfb', name: 'Claude', sources: [CHATS, CONFIGURED], actions: [SEND] };
var GROK = {
  uuid: '80ee6780-856f-411f-954b-4553ba7f2af8', name: 'Grok Bot', sources: [CHATS, CONFIGURED],
  actions: [SEND, { name: 'read_grok_bot', parameters: { properties: { limit: {}, chat_id: {} } } }],
};
// Universal AI's real manifest (ericlmccormick/Pebble-Universal-AI), trimmed.
var UNIVERSAL = {
  uuid: '2c5edc42-b096-4f76-b284-1f27907e6501', name: 'Universal AI', sources: [CHATS, CONFIGURED],
  actions: [SEND, { name: 'send_message' }, { name: 'conversation' }, { name: 'get_config' },
            { name: 'list_models' }, { name: 'clear_conversation' }, { name: 'clear_ai_conversation' },
            { name: 'get_message', parameters: { required: ['request_id'] } }, { name: 'get_capabilities' }],
};
var MUSE = { uuid: 'b8346592-cd59-4b83-9185-87d94e0734b0', name: 'Muse', sources: [], actions: [SEND] };

function chats(list) {
  return { instances: list.map(function (c) {
    return { instanceId: c[0], properties: { name: { shortText: { text: c[1] } } } };
  }) };
}

function configured(ok, name) {
  return { instances: [{ properties: { configured: { boolean: { value: ok } }, name: { shortText: { text: name } } } }] };
}

function harness(plugins, onAction) {
  var h = { sent: [], calls: [], subs: [], listeners: {}, store: {} };
  global.localStorage = {
    getItem: function (k) { return k in h.store ? h.store[k] : null; },
    setItem: function (k, v) { h.store[k] = String(v); },
    removeItem: function (k) { delete h.store[k]; },
  };
  global.Pebble = {
    addEventListener: function (name, fn) { h.listeners[name] = fn; },
    sendAppMessage: function (dict, ok) { h.sent.push(JSON.parse(JSON.stringify(dict))); if (ok) ok(); },
    enumeratePlugins: function () { return plugins; },
    subscribeToSource: function (opts) {
      var sub = { opts: opts, active: true, unsubscribe: function () { sub.active = false; } };
      h.subs.push(sub);
      return sub;
    },
    invokeAction: function (req) {
      h.calls.push(req);
      return Promise.resolve(onAction(req));
    },
  };
  delete require.cache[require.resolve('../src/pkjs/index.js')];
  require('../src/pkjs/index.js');
  h.watch = function (payload) { h.listeners.appmessage({ payload: payload }); };
  h.feed = function (plugin, item, envelope) {
    h.subs.filter(function (s) {
      return s.active && s.opts.plugin === plugin.uuid && s.opts.item === item;
    }).forEach(function (s) { s.opts.onData(envelope); });
  };
  h.last = function (key) {
    for (var i = h.sent.length - 1; i >= 0; i--) if (key in h.sent[i]) return h.sent[i][key];
    return undefined;
  };
  return h;
}

/** Node 18 takes an array, newer Node an options object. */
function fakeTimers(t) {
  try {
    t.mock.timers.enable({ apis: ['setTimeout'] });
  } catch (e) {
    t.mock.timers.enable(['setTimeout']);
  }
}

function flush() {
  return new Promise(function (resolve) { setImmediate(resolve); });
}

test('no plugin API: tells the user to update the Pebble app', function () {
  var h = harness([], function () {});
  delete global.Pebble.enumeratePlugins;
  h.listeners.ready();
  assert.match(h.last('STATUS'), /latest Pebble app/);
  assert.strictEqual(h.last('AGENT_ACTIVE'), -1);
});

test('lists agents, caps follow the active plugin, sessions come from agent/chats', function () {
  var h = harness([MUSE, GROK, CLAUDE], function () {});
  h.listeners.ready();
  assert.strictEqual(h.last('AGENTS'), 'Claude\nGrok Bot\nMuse');
  assert.strictEqual(h.last('AGENT_ACTIVE'), 0);
  assert.strictEqual(h.last('CAPS'), 1, 'Claude: sessions, no read');
  assert.ok(h.last('ACCENT') > 0);

  h.feed(CLAUDE, 'configured', configured(true, 'New chat'));
  h.feed(CLAUDE, 'chats', chats([['chat:1', 'Groceries'], ['code:2', 'Build fix']]));
  assert.strictEqual(h.last('SESSIONS'), 'New chat\nGroceries\nBuild fix');

  h.watch({ CMD: 3, ARG: 1 });  // Grok Bot
  assert.strictEqual(h.last('CAPS'), 3, 'Grok: sessions and read');
  assert.strictEqual(h.store.agent, GROK.uuid);

  h.watch({ CMD: 3, ARG: 2 });  // Muse: no sources at all
  assert.strictEqual(h.last('CAPS'), 0);
  assert.strictEqual(h.last('SESSIONS'), 'Default');
});

test('not signed in shows a status line', function () {
  var h = harness([CLAUDE], function () {});
  h.listeners.ready();
  h.feed(CLAUDE, 'configured', configured(false, ''));
  assert.match(h.last('STATUS'), /Set up Claude: open its settings/);
  assert.strictEqual(h.last('CONFIGURED'), 0);
});

test('send goes to the selected session and reports the receipt', async function () {
  var h = harness([CLAUDE], function () { return { ok: true, text: 'Sent.' }; });
  h.listeners.ready();
  h.feed(CLAUDE, 'configured', configured(true, 'Groceries'));
  h.feed(CLAUDE, 'chats', chats([['chat:1', 'Groceries'], ['chat:2', 'Work']]));
  // Default, New chat, Work (Groceries is the default, so not repeated)
  assert.strictEqual(h.last('SESSIONS'), 'Groceries\nNew chat\nWork');

  h.watch({ CMD: 4, ARG: 2 });
  assert.strictEqual(h.store['session:' + CLAUDE.uuid], 'chat:2');
  h.watch({ CMD: 2, ARG: 1, TEXT: 'buy milk' });
  await flush();
  assert.deepStrictEqual(h.calls[0], {
    plugin: CLAUDE.uuid, action: 'send_to_agent', args: { message: 'buy milk', chat_id: 'chat:2' },
  });
  assert.strictEqual(h.last('DELIVERY'), 2);
  assert.strictEqual(h.last('DELIVERY_TEXT'), 'Sent to Work');
  assert.strictEqual(h.calls.length, 1, 'no read action, so no polling');
});

test('failed send is reported and never retried', async function () {
  var h = harness([CLAUDE], function () { return { ok: false, code: 'TIMEOUT', message: '' }; });
  h.listeners.ready();
  h.watch({ CMD: 2, ARG: 1, TEXT: 'hello' });
  await flush();
  assert.strictEqual(h.calls.length, 1);
  assert.strictEqual(h.last('DELIVERY'), 3);
  assert.match(h.last('DELIVERY_TEXT'), /may have sent/);
});

test('two-way: Grok Bot reply is polled and forwarded', async function (t) {
  fakeTimers(t);
  var reads = 0;
  var h = harness([GROK], function (req) {
    if (req.action === 'send_to_agent') return { ok: true, text: 'Message sent.' };
    reads++;
    // baseline, then one poll with nothing new, then the reply
    return { ok: true, text: reads < 3 ? 'Work: earlier answer' : 'Work: It is 4pm.' };
  });
  h.listeners.ready();
  h.feed(GROK, 'chats', chats([['bot1', 'Work']]));
  h.watch({ CMD: 4, ARG: 1 });
  h.watch({ CMD: 2, ARG: 1, TEXT: 'what time is it' });
  await flush();
  assert.deepStrictEqual(h.calls[0].args, { limit: 1, chat_id: 'bot1' }, 'baseline read first');
  assert.strictEqual(h.calls[1].action, 'send_to_agent');
  assert.strictEqual(h.last('DELIVERY'), 4, 'waiting');

  t.mock.timers.tick(3000);
  await flush();
  assert.strictEqual(h.last('DELIVERY'), 4, 'unchanged reply keeps waiting');
  t.mock.timers.tick(3000);
  await flush();
  assert.strictEqual(h.last('DELIVERY'), 5);
  assert.strictEqual(h.last('REPLY'), 'Work: It is 4pm.');
});

test('switching session stops polling for the old one', async function (t) {
  fakeTimers(t);
  var h = harness([GROK], function (req) {
    return req.action === 'send_to_agent' ? { ok: true } : { ok: true, text: 'same' };
  });
  h.listeners.ready();
  h.feed(GROK, 'chats', chats([['bot1', 'Work'], ['bot2', 'Home']]));
  h.watch({ CMD: 2, ARG: 1, TEXT: 'hi' });
  await flush();
  var before = h.calls.length;
  h.watch({ CMD: 4, ARG: 2 });
  t.mock.timers.tick(60000);
  await flush();
  assert.strictEqual(h.calls.length, before);
});

test('sending to New chat follows the conversation it created', async function (t) {
  fakeTimers(t);
  var h = harness([CLAUDE], function () { return { ok: true }; });
  h.listeners.ready();
  h.feed(CLAUDE, 'configured', configured(true, 'Work'));
  h.feed(CLAUDE, 'chats', chats([['chat:1', 'Work']]));
  h.watch({ CMD: 4, ARG: 1 });  // New chat
  h.watch({ CMD: 2, ARG: 0, TEXT: 'plan a trip' });
  await flush();
  assert.strictEqual(h.calls[0].args.chat_id, 'new');

  t.mock.timers.tick(4000);  // resubscribes for a fresh list
  h.feed(CLAUDE, 'chats', chats([['chat:9', 'Trip planning'], ['chat:1', 'Work']]));
  assert.strictEqual(h.store['session:' + CLAUDE.uuid], 'chat:9');
  assert.strictEqual(h.last('SESSIONS'), 'Work\nNew chat\nTrip planning');
  assert.strictEqual(h.last('SESSION_ACTIVE'), 2);
});

test('sessions keep arriving after the agent list is refreshed', function () {
  var h = harness([CLAUDE], function () {});
  h.listeners.ready();
  h.watch({ CMD: 1 });  // the watch's hello refreshes the agent list again
  h.feed(CLAUDE, 'configured', configured(true, 'New chat'));
  h.feed(CLAUDE, 'chats', chats([['chat:1', 'Groceries']]));
  assert.strictEqual(h.last('SESSIONS'), 'New chat\nGroceries');
});

test('opening settings refetches the chat list', function () {
  var h = harness([CLAUDE], function () {});
  h.listeners.ready();
  var before = h.subs.filter(function (s) { return s.opts.item === 'chats'; }).length;
  h.watch({ CMD: 6 });
  var chatSubs = h.subs.filter(function (s) { return s.opts.item === 'chats'; });
  assert.strictEqual(chatSubs.length, before + 1);
  assert.strictEqual(chatSubs.filter(function (s) { return s.active; }).length, 1, 'old one unsubscribed');
  h.feed(CLAUDE, 'chats', chats([['code:rc', 'Pebble agent app']]));
  assert.match(h.last('SESSIONS'), /Pebble agent app/);
});

test('Universal AI: the answer comes back from send_to_agent and is shown', async function () {
  var h = harness([UNIVERSAL], function (req) {
    assert.strictEqual(req.action, 'send_to_agent');
    return { ok: true, text: 'Paris is the capital of France.', reply_type: 'assistant', chat_id: 'u_s1' };
  });
  h.listeners.ready();
  assert.strictEqual(h.last('CAPS'), 1, 'sessions, but no read action to poll');
  h.feed(UNIVERSAL, 'configured', configured(true, 'OpenAI Conversation'));
  h.feed(UNIVERSAL, 'chats', chats([['default', 'Default conversation'], ['u_s0', 'Conversation abc123']]));
  assert.strictEqual(h.last('SESSIONS'), 'OpenAI Conversation\nNew chat\nConversation abc123');

  h.watch({ CMD: 2, ARG: 1, TEXT: 'capital of France' });
  assert.strictEqual(h.last('DELIVERY'), 4, 'waiting while the model answers');
  await flush();
  assert.deepStrictEqual(h.calls[0].args, { message: 'capital of France' }, 'default: no chat_id');
  assert.strictEqual(h.calls.length, 1, 'no polling, no extra calls');
  assert.strictEqual(h.last('DELIVERY'), 5);
  assert.strictEqual(h.last('REPLY'), 'Paris is the capital of France.');
});

test('Universal AI: answer shown even if the host drops reply_type', async function () {
  var h = harness([UNIVERSAL], function () { return { ok: true, text: 'Four.' }; });
  h.listeners.ready();
  h.watch({ CMD: 2, ARG: 0, TEXT: '2+2' });
  await flush();
  assert.strictEqual(h.last('REPLY'), 'Four.');
});

test('a receipt from a send-only agent is not mistaken for a reply', async function () {
  var h = harness([CLAUDE], function () { return { ok: true, text: 'Message submitted to Claude: Work.' }; });
  h.listeners.ready();
  h.watch({ CMD: 2, ARG: 1, TEXT: 'hi' });
  await flush();
  assert.strictEqual(h.last('DELIVERY'), 2);
  assert.strictEqual(h.last('REPLY'), undefined);
});

test('Universal AI: a new chat is followed by the id the plugin returned', async function (t) {
  fakeTimers(t);
  var h = harness([UNIVERSAL], function () {
    return { ok: true, text: 'Sure.', reply_type: 'assistant', chat_id: 'u_new' };
  });
  h.listeners.ready();
  h.feed(UNIVERSAL, 'configured', configured(true, 'OpenAI Conversation'));
  h.feed(UNIVERSAL, 'chats', chats([['default', 'Default conversation']]));
  h.watch({ CMD: 4, ARG: 1 });  // New chat
  h.watch({ CMD: 2, ARG: 0, TEXT: 'plan a trip' });
  await flush();
  assert.strictEqual(h.calls[0].args.chat_id, 'new');
  t.mock.timers.tick(4000);
  h.feed(UNIVERSAL, 'chats', chats([['default', 'Default conversation'], ['u_other', 'Conversation other'],
                                    ['u_new', 'Conversation u_new']]));
  assert.strictEqual(h.store['session:' + UNIVERSAL.uuid], 'u_new');
});
