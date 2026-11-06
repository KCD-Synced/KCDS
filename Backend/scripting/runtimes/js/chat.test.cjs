'use strict';

// Chat and /commands: the server Chat global and playerChat/playerCommand, and the
// client Chat global with the reserved chatSend and chatMessage events
// (Client/resources/chat.cjs).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { Color } = require('./math.cjs');
const { ResourceChat } = require('../../../../Client/resources/chat.cjs');

function server() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-chat-'));
  const output = [];
  const host = new ResourceHost(root, {
    // The time, weather and stew pots every arriving player is told are not
    // what these test.
    send: (message) => {
      if (!['worldTime', 'worldWeather', 'potLooks'].includes(message.op)) {
        output.push(message);
      }
    },
    log: () => {},
  });
  // The needs orders an arriving player gets while the needs are off (their default)
  // are not what these test.
  host.api.Needs.setEnabled(true);
  const player = host.updatePlayer({
    id: 7,
    nickname: 'Henry',
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  return {
    root,
    output,
    host,
    player,
    done: () => fs.rmSync(root, { recursive: true, force: true }),
  };
}

test('server Chat packs colours as 0xRRGGBBAA from numbers and Color objects', () => {
  const f = server();
  try {
    f.host.api.Chat.sendToAll('gold', { color: 0xffd24dff });
    f.host.api.Chat.sendToAll('red', { color: Color.fromRGB(200, 60, 60) });
    f.host.api.Chat.sendToPlayer(f.player, 'plain', { author: 'Referee' });
    assert.deepEqual(
      f.output.map(({ target, text, author, color }) => [target, text, author, color]),
      [
        [0, 'gold', '', 0xffd24dff],
        [0, 'red', '', 0xc83c3cff],
        [7, 'plain', 'Referee', 0],
      ],
    );
    assert.throws(() => f.host.api.Chat.sendToAll('x', { color: 'red' }), TypeError);
    assert.throws(() => f.host.api.Chat.sendToAll(5), TypeError);
  } finally {
    f.done();
  }
});

test('playerCommand keeps the case and splits arguments on whitespace runs', () => {
  const f = server();
  const seen = [];
  try {
    f.host.api.Events.on('playerCommand', (player, command, args) => seen.push([command, args]));
    f.host.receiveChat(7, '/heal');
    f.host.receiveChat(7, '/give apple 5');
    f.host.receiveChat(7, '/Give  apple');
    f.host.receiveChat(7, '/give "Hunting Sword" 2');
    assert.deepEqual(seen, [
      ['heal', []],
      ['give', ['apple', '5']],
      ['Give', ['apple']],
      ['give', ['"Hunting', 'Sword"', '2']],
    ]);
    assert.equal(f.output.length, 0);
    assert.equal(f.host.api.Chat.relaysChat(), true);
  } finally {
    f.done();
  }
});

function client() {
  const sent = [];
  const handlers = new Map();
  const emitted = [];
  const chat = new ResourceChat({
    send: (message) => sent.push(message),
    // The services' emitSync: every handler runs; only a literal false blocks.
    emitSync: (name, args) => {
      let accepted = true;
      for (const handler of handlers.get(name) ?? []) {
        try {
          if (handler(...args) === false) {
            accepted = false;
          }
        } catch {
          // logged and skipped
        }
      }
      return accepted;
    },
    emit: (name, args) => emitted.push([name, ...args]),
  });
  const on = (name, handler) => handlers.set(name, [...(handlers.get(name) ?? []), handler]);
  return { chat, sent, emitted, on, facade: chat.facade() };
}

test('typed lines pass every chatSend handler and leave unless one returns false', () => {
  const { chat, sent, on } = client();
  const seen = [];
  on('chatSend', (text) => {
    seen.push(text);
    return text === '/hidehud' ? false : undefined;
  });
  on('chatSend', (text) => {
    seen.push(`second ${text}`);
    return 0;
  });
  on('chatSend', () => {
    throw new Error('broken');
  });
  chat.receiveTyped({ text: 'hello' });
  chat.receiveTyped({ text: '/hidehud' });
  assert.deepEqual(seen, ['hello', 'second hello', '/hidehud', 'second /hidehud']);
  assert.deepEqual(sent, [{ op: 'chatOut', text: 'hello' }]);
});

test('Chat.send skips chatSend and checks its text', () => {
  const { sent, on, facade } = client();
  on('chatSend', () => false);
  facade.send('/help');
  assert.deepEqual(sent, [{ op: 'chatOut', text: '/help' }]);
  assert.throws(() => facade.send(''), RangeError);
  assert.throws(() => facade.send('a\nb'), RangeError);
  assert.throws(() => facade.send('x'.repeat(1025)), RangeError);
  assert.throws(() => facade.send(3), TypeError);
});

test('incoming lines are raised as chatMessage with author, text and colour', () => {
  const { chat, emitted } = client();
  chat.receiveMessage({ author: 'Henry', text: 'hi', color: 0xffd24dff });
  chat.receiveMessage({ author: '', text: 'notice', color: 0 });
  assert.deepEqual(emitted, [
    ['chatMessage', { author: 'Henry', text: 'hi', color: 0xffd24dff }],
    ['chatMessage', { author: '', text: 'notice', color: 0 }],
  ]);
});

test('overlay visibility, open and close follow the calls and the game reports', () => {
  const { chat, sent, facade } = client();
  assert.equal(facade.isUIVisible(), true);
  facade.open();
  assert.equal(facade.isOpen(), true);
  facade.setUIVisible(false);
  assert.equal(facade.isUIVisible(), false);
  assert.equal(facade.isOpen(), false);
  facade.open();
  assert.equal(facade.isOpen(), false);
  chat.receiveHost({ open: false, visible: true });
  assert.equal(facade.isUIVisible(), true);
  chat.receiveHost({ open: true, visible: true });
  facade.close();
  assert.equal(facade.isOpen(), false);
  assert.throws(() => facade.setUIVisible('no'), TypeError);
  assert.deepEqual(
    sent.map((message) => message.op),
    ['chatOpen', 'chatUI', 'chatOpen', 'chatClose'],
  );
});

test('the chat key pressed while a resource hid the chat is raised as chatKey', () => {
  const { chat, emitted, facade } = client();
  facade.setUIVisible(false);
  chat.receiveKey();
  chat.receiveKey();
  assert.deepEqual(emitted, [['chatKey'], ['chatKey']]);
});
