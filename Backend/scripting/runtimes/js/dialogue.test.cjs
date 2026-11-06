'use strict';

// Dialogue: pages, picks and the close of a conversation the server drives on a player.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-dialogue-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const sent = (op) => output.filter((message) => message.op === op);
  const events = [];
  host.on('dialogueChoice', (session, player, option) => {
    events.push(['choice', session, player.id, option]);
  });
  host.on('dialogueClosed', (session, player, reason) => {
    events.push(['closed', session, player.id, reason]);
  });
  return { host, Dialogue: host.makeAPI().Dialogue, sent, output, events };
}

function join(host, id, position = { x: 0, y: 0, z: 0 }) {
  return host.updatePlayer({ id, ready: true, position, rotation: { w: 1, x: 0, y: 0, z: 0 } });
}

const PAGE = {
  line: 'What do you want?',
  options: [
    { id: 'buy', text: 'Show me your swords.' },
    { id: 'steal', text: 'Hand me the purse.', enabled: false },
    { id: 'bye', text: 'Farewell.' },
  ],
};

test('open shows the page in the player game and the pick comes back as the row id', async () => {
  const { host, Dialogue, sent, events } = await startedHost();
  const player = join(host, 1);
  const session = Dialogue.open(1, PAGE);
  assert.ok(session > 0);
  assert.equal(Dialogue.activeSession(1), session);
  assert.equal(Dialogue.activeSession(player), session, 'a handle works as well');
  const [page] = sent('dialoguePage');
  assert.deepEqual(page, {
    op: 'dialoguePage',
    id: 1,
    session,
    generation: 1,
    onRight: false,
    line: 'What do you want?',
    speaker: '',
    options: [
      { text: 'Show me your swords.', enabled: true },
      { text: 'Hand me the purse.', enabled: false },
      { text: 'Farewell.', enabled: true },
    ],
  });
  // A disabled row, a row out of range, another player's pick: dropped.
  assert.equal(host.dialogues.choice(player, session, 1, 1), false);
  assert.equal(host.dialogues.choice(player, session, 1, 5), false);
  const other = join(host, 2);
  assert.equal(host.dialogues.choice(other, session, 1, 0), false);
  assert.equal(host.dialogues.choice(player, session, 1, 0), true);
  // One pick per page.
  assert.equal(host.dialogues.choice(player, session, 1, 2), false);
  assert.deepEqual(events, [['choice', session, 1, 'buy']]);
});

test('update replaces the page; a pick still in flight against the old one is dropped', async () => {
  const { host, Dialogue, sent, events } = await startedHost();
  const player = join(host, 1);
  const session = Dialogue.open(1, PAGE);
  assert.equal(
    Dialogue.update(session, { onRight: true, options: [{ id: 'more', text: 'Tell me more.' }] }),
    true,
  );
  const pages = sent('dialoguePage');
  assert.equal(pages.length, 2);
  assert.equal(pages[1].generation, 2);
  assert.equal(pages[1].onRight, true);
  assert.equal(pages[1].line, '');
  assert.equal(host.dialogues.choice(player, session, 1, 0), false, 'the old page');
  assert.equal(host.dialogues.choice(player, session, 2, 0), true);
  assert.deepEqual(events, [['choice', session, 1, 'more']]);
  assert.equal(Dialogue.update(session, { options: [] }), false, 'a page without rows');
  assert.equal(Dialogue.update(999, PAGE), false, 'no such session');
});

test('unusable pages and players open nothing', async () => {
  const { host, Dialogue, sent } = await startedHost();
  join(host, 1);
  host.updatePlayer({ id: 3, ready: false, position: { x: 0, y: 0, z: 0 } });
  const nine = Array.from({ length: 9 }, (_, i) => ({ id: `r${i}`, text: 'row' }));
  assert.equal(Dialogue.open(1, { options: nine }), 0);
  assert.equal(Dialogue.open(1, { options: [] }), 0);
  assert.equal(Dialogue.open(1, { options: [{ id: 'a', text: '' }] }), 0);
  assert.equal(
    Dialogue.open(1, {
      options: [
        { id: 'a', text: 'x' },
        { id: 'a', text: 'y' },
      ],
    }),
    0,
  );
  assert.equal(Dialogue.open(1, { options: [{ id: 'a', text: 'x', enabled: 'no' }] }), 0);
  assert.equal(Dialogue.open(1, { line: 7, options: [{ id: 'a', text: 'x' }] }), 0);
  assert.equal(Dialogue.open(2, PAGE), 0, 'nobody');
  assert.equal(Dialogue.open(3, PAGE), 0, 'still loading');
  assert.equal(sent('dialoguePage').length, 0);
  assert.equal(Dialogue.activeSession(1), 0);
});

test('close, cancel and replace end the session with their reasons', async () => {
  const { host, Dialogue, sent, events } = await startedHost();
  const player = join(host, 1);
  const first = Dialogue.open(1, PAGE);
  const second = Dialogue.open(1, PAGE);
  assert.notEqual(first, second);
  assert.equal(Dialogue.activeSession(1), second);
  assert.equal(Dialogue.close(second), true);
  assert.equal(Dialogue.close(second), false);
  assert.equal(Dialogue.activeSession(1), 0);
  assert.deepEqual(
    sent('dialogueClose').map((m) => m.session),
    [first, second],
  );
  const third = Dialogue.open(1, PAGE);
  assert.equal(host.dialogues.cancelled(player, third, 1), true);
  assert.equal(host.dialogues.choice(player, third, 1, 0), false, 'ended');
  const fourth = Dialogue.open(1, PAGE);
  assert.equal(host.dialogues.cancelled(player, fourth, 3), true);
  assert.equal(sent('dialogueClose').length, 2, 'the game ended those itself');
  assert.deepEqual(events, [
    ['closed', first, 1, 2],
    ['closed', second, 1, 0],
    ['closed', third, 1, 1],
    ['closed', fourth, 1, 3],
  ]);
});

test('a disconnect, a knock-out and a new world interrupt the conversation', async () => {
  const { host, Dialogue, events } = await startedHost();
  join(host, 1);
  const down = Dialogue.open(1, PAGE);
  host.playerDied(1);
  const reloaded = Dialogue.open(1, PAGE);
  host.dialogues.arrived(host.players.get(1));
  const gone = Dialogue.open(1, PAGE);
  host.disconnect(1);
  assert.deepEqual(
    events.filter((event) => event[0] === 'closed'),
    [
      ['closed', down, 1, 3],
      ['closed', reloaded, 1, 3],
      ['closed', gone, 1, 3],
    ],
  );
  assert.equal(Dialogue.activeSession(1), 0);
});

test('a conversation tied to an NPC names it, ends with it and ends out of reach', async () => {
  const { host, Dialogue, sent, events } = await startedHost();
  const player = join(host, 1, { x: 1, y: 0, z: 0 });
  const npc = host.npcs.Npc.create({ name: 'Smith', position: { x: 0, y: 0, z: 0 } });
  const killed = Dialogue.open(1, PAGE, { npc });
  assert.equal(sent('dialoguePage')[0].speaker, 'Smith');
  npc.kill();
  assert.deepEqual(events.at(-1), ['closed', killed, 1, 3]);
  assert.equal(Dialogue.open(1, PAGE, { npc }), 0, 'a dead NPC does not talk');
  assert.throws(() => Dialogue.open(1, PAGE, { npc: 99999 }), TypeError);
  assert.throws(() => Dialogue.open(1, PAGE, { radius: 3 }), RangeError);
  const placed = Dialogue.open(1, PAGE, { position: { x: 0, y: 0, z: 0 }, radius: 2 });
  host.dialogues.checkReach();
  assert.equal(Dialogue.activeSession(1), placed, 'within reach');
  host.updatePlayer({ id: 1, ready: true, position: { x: 5, y: 0, z: 0 } });
  assert.equal(player.position.x, 5);
  host.dialogues.checkReach();
  assert.deepEqual(events.at(-1), ['closed', placed, 1, 3]);
  assert.equal(host.dialogues.reachTimer, null, 'no check runs without a tied conversation');
});

test('a page without a pick ends after its timeout; a pick stops the clock', async () => {
  const { host, Dialogue, events } = await startedHost();
  const player = join(host, 1);
  const timed = Dialogue.open(1, PAGE, { timeout: 0.05 });
  await new Promise((resolve) => setTimeout(resolve, 120));
  assert.deepEqual(events.at(-1), ['closed', timed, 1, 3]);
  const picked = Dialogue.open(1, PAGE, { timeout: 0.05 });
  assert.equal(host.dialogues.choice(player, picked, 1, 0), true);
  await new Promise((resolve) => setTimeout(resolve, 120));
  assert.equal(Dialogue.activeSession(1), picked);
  assert.throws(() => Dialogue.open(1, PAGE, { timeout: -1 }), RangeError);
});

test('a stopped resource takes its conversations with it', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-dialogue-'));
  const dir = path.join(root, 'talk');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'talk', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    `kcds.core.Events.on('playerReady', (p) => kcds.characters.Dialogue.open(p.id, { options: [{ id: 'a', text: 'Hi' }] }));`,
  );
  const output = [];
  const host = new ResourceHost(root, { send: (m) => output.push(m), log: () => {} });
  await host.startAll();
  join(host, 1);
  host.emit('playerReady', host.players.get(1));
  const session = host.dialogues.activeSession(1);
  assert.ok(session > 0);
  await host.stop('talk');
  assert.equal(host.dialogues.activeSession(1), 0);
  assert.deepEqual(
    output.filter((m) => m.op === 'dialogueClose').map((m) => m.session),
    [session],
  );
});
