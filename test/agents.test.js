// node --test test/
var test = require('node:test');
var assert = require('node:assert');
var A = require('../src/pkjs/agents');

// Trimmed from the real manifests in coredevices/pebble-plugins, in the shape
// Pebble.enumeratePlugins() returns them.
var CHATS = { category: 'agent', items: ['chats'], properties: { name: ['shortText'] } };
var CONFIGURED = { category: 'agent', items: ['configured'], properties: { configured: ['boolean'] } };
var SEND = {
  name: 'send_to_agent',
  parameters: { type: 'object', properties: { message: {}, chat_id: {} }, required: ['message'] },
};

var claude = { uuid: '0d33014e-5d56-42ea-9665-3ebf182b4cfb', name: 'Claude', sources: [CHATS, CONFIGURED], actions: [SEND] };
var chatgpt = {
  uuid: '3f97d5c5-bc28-48ba-b4af-e7ad7f6000f0', name: 'ChatGPT', sources: [CHATS, CONFIGURED],
  actions: [SEND, { name: 'start_codex_cloud_task', parameters: { properties: { prompt: {} }, required: ['prompt'] } }],
};
var grok = {
  uuid: '80ee6780-856f-411f-954b-4553ba7f2af8', name: 'Grok Bot', sources: [CHATS, CONFIGURED],
  actions: [SEND, { name: 'read_grok_bot', parameters: { properties: { limit: {}, chat_id: {} } } }],
};
var weather = { uuid: 'x', name: 'Weather', sources: [{ category: 'weather', items: ['location'] }], actions: [] };

test('findAgents keeps only send_to_agent plugins, sorted, with capabilities', function () {
  var agents = A.findAgents([grok, weather, claude, chatgpt]);
  assert.deepStrictEqual(agents.map(function (a) { return a.name; }), ['ChatGPT', 'Claude', 'Grok Bot']);
  var byName = {};
  agents.forEach(function (a) { byName[a.name] = a; });
  assert.strictEqual(byName.Claude.readAction, null);
  assert.strictEqual(byName.ChatGPT.readAction, null, 'start_codex_cloud_task is not a read');
  assert.strictEqual(byName['Grok Bot'].readAction.name, 'read_grok_bot');
  assert.ok(byName.Claude.hasChats && byName.Claude.hasConfigured);
  assert.strictEqual(byName.Claude.newChat, 'new');
});

test('findAgents tolerates missing fields', function () {
  assert.deepStrictEqual(A.findAgents(undefined), []);
  var bare = A.findAgents([{ uuid: 'u', actions: [{ name: 'send_to_agent' }] }]);
  assert.strictEqual(bare.length, 1);
  assert.strictEqual(bare[0].hasChats, false);
  assert.strictEqual(bare[0].name, 'Agent');
});

test('read actions needing unknown arguments are not used', function () {
  var plugin = { actions: [{ name: 'read_thing', parameters: { required: ['query'] } }] };
  assert.strictEqual(A.readActionOf(plugin), null);
  var preferred = { actions: [{ name: 'read_other' }, { name: 'read_agent' }] };
  assert.strictEqual(A.readActionOf(preferred).name, 'read_agent');
});

test('accentByte maps to the nearest Pebble colour', function () {
  assert.strictEqual(A.accentByte('#10A37F'), 0xC9);  // JaegerGreen 00AA55
  assert.strictEqual(A.accentByte('#FFFFFF'), 0xFF);
  assert.strictEqual(A.accentByte(undefined), 0);
});

test('buildSessions: default first, Claude gets New chat, no duplicate names', function () {
  var agent = A.findAgents([claude])[0];
  var chats = A.parseChats({ instances: [
    { instanceId: 'chat:1', properties: { name: { shortText: { text: 'Groceries' } } } },
    { instanceId: 'code:2', properties: { title: { shortText: { text: 'Fix  the\nbuild' } } } },
  ] });
  var configured = A.parseConfigured({ instances: [{ properties: {
    configured: { boolean: { value: true } }, name: { shortText: { text: 'New chat' } } } }] });
  var sessions = A.buildSessions(agent, chats, configured);
  // Default is already "New chat", so no second New chat entry.
  assert.deepStrictEqual(sessions, [
    { id: null, name: 'New chat' },
    { id: 'chat:1', name: 'Groceries' },
    { id: 'code:2', name: 'Fix the build' },
  ]);

  var withDefault = A.buildSessions(agent, chats, { configured: true, name: 'Groceries' });
  assert.deepStrictEqual(withDefault.map(function (s) { return s.id; }), [null, 'new', 'code:2']);
});

test('buildSessions: ChatGPT lists its own new-chat instance', function () {
  var agent = A.findAgents([chatgpt])[0];
  var chats = [{ id: 'dot', name: 'Dot' }, { id: 'new-chat', name: 'New ChatGPT chat' }, { id: 'c1', name: 'Trip' }];
  var sessions = A.buildSessions(agent, chats, { configured: true, name: 'Dot' });
  assert.deepStrictEqual(sessions.map(function (s) { return s.id; }), [null, 'new-chat', 'c1']);
});

test('buildSessions caps the list', function () {
  var chats = [];
  for (var i = 0; i < 50; i++) chats.push({ id: 'c' + i, name: 'Chat ' + i });
  assert.strictEqual(A.buildSessions({}, chats, null, 24).length, 24);
});

test('startsNewChat', function () {
  assert.ok(A.startsNewChat({ id: 'new', name: 'New chat' }));
  assert.ok(A.startsNewChat({ id: 'new-chat', name: 'x' }));
  assert.ok(A.startsNewChat({ id: null, name: 'New ChatGPT chat' }));
  assert.ok(!A.startsNewChat({ id: null, name: 'Dot' }));
  assert.ok(!A.startsNewChat({ id: 'chat:1', name: 'New chat ideas' }));
});

test('send and read args', function () {
  assert.deepStrictEqual(A.sendArgs('hi', { id: null }), { message: 'hi' });
  assert.deepStrictEqual(A.sendArgs('hi', { id: 'c1' }), { message: 'hi', chat_id: 'c1' });
  var read = A.findAgents([grok])[0].readAction;
  assert.deepStrictEqual(A.readArgs(read, { id: null }), { limit: 1 });
  assert.deepStrictEqual(A.readArgs(read, { id: 'b1' }), { limit: 1, chat_id: 'b1' });
  assert.deepStrictEqual(A.readArgs({ name: 'read_x', parameters: { properties: { chat_id: {} } } }, { id: 'b' }),
    { chat_id: 'b' });
});

test('errorText is short and actionable', function () {
  assert.match(A.errorText('Claude', { ok: false, code: 'AUTH_REQUIRED' }), /Set up Claude/);
  assert.match(A.errorText('Claude', { ok: false, code: 'TIMEOUT' }), /may have sent/);
  assert.strictEqual(A.errorText('X', { ok: false, code: 'PLUGIN_UNAVAILABLE', message: 'Down' }), 'Down');
  assert.ok(A.errorText('X', { ok: false, code: 'UNKNOWN', message: new Array(300).join('a') }).length <= A.STATUS_MAX);
});

test('isNewReply', function () {
  assert.ok(!A.isNewReply('Bot: hi', 'Bot: hi '));
  assert.ok(A.isNewReply('Bot has no replies yet.', 'Bot: hello'));
  assert.ok(A.isNewReply(null, 'Bot: hello'));
  assert.ok(!A.isNewReply('x', ''));
});

test('plugin errors are cut to what a person needs', function () {
  var claude409 = 'Claude reported a session conflict (HTTP 409). Read the session to check its current state before sending again; do not automatically retry.';
  assert.strictEqual(A.errorText('Claude', { ok: false, code: 'PLUGIN_UNAVAILABLE', message: claude409 }),
    'Claude reported a session conflict (HTTP 409). Check the chat before resending.');
  assert.strictEqual(A.errorText('Claude', { ok: false, code: 'INVALID_ARGS', message: 'Wait for Claude to finish the current reply before sending.' }),
    'Wait for Claude to finish the current reply before sending.');
  assert.strictEqual(A.errorText('X', { ok: false, code: 'UNKNOWN', message: 'v1.2 broke. Try later.' }), 'v1.2 broke.');
});

test('buildSessions keeps different chats that share a title', function () {
  var agent = A.findAgents([claude])[0];
  var chats = [{ id: 'chat:1', name: 'Untitled' }, { id: 'chat:2', name: 'Untitled' },
               { id: 'chat:2', name: 'Untitled' }, { id: 'chat:3', name: 'Work' }];
  var sessions = A.buildSessions(agent, chats, { configured: true, name: 'Work' });
  assert.deepStrictEqual(sessions.map(function (s) { return s.id; }), [null, 'new', 'chat:1', 'chat:2']);
});
