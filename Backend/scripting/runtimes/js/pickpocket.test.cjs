'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// Robbing another player: the server's rules and events around the game's own
// pickpocketing.

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-pickpocket-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'gamemode');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'gamemode', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  return { host, output };
}

function player(host, id, nickname) {
  return host.updatePlayer({
    id,
    nickname,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

const rules = (output) => output.filter((e) => e.op === 'pickpocketRule');

test('a request is answered, a caught thief is told to the victim, a theft ends', async () => {
  const { host, output } = hostWith(`
    kcds.core.Events.on('playerPickpocketStart', (t, v) => kcds.core.Chat.sendToAll('start ' + t.id + ' ' + v.id));
    kcds.core.Events.on('playerPickpocketCaught', (t, v) => kcds.core.Chat.sendToAll('caught ' + t.id + ' ' + v.id));
    kcds.core.Events.on('playerPickpocketed', (t, v, items) =>
      kcds.core.Chat.sendToAll('robbed ' + t.id + ' ' + v.id + ' ' + items.length));
  `);
  await host.startAll();
  const thief = player(host, 7, 'Tom');
  player(host, 8, 'Anna');
  host.pickpocketing.settleMs = 0;
  host.pickpocketing.report(thief, { kind: 'request', victim: 8 });
  assert.deepEqual(rules(output).at(-1), {
    op: 'pickpocketRule',
    id: 7,
    kind: 'answer',
    flag: true,
    player: 8,
  });
  host.pickpocketing.report(thief, { kind: 'caught', victim: 8 });
  assert.deepEqual(rules(output).at(-1), {
    op: 'pickpocketRule',
    id: 8,
    kind: 'noticed',
    flag: true,
    player: 7,
    notice: 'Tom tried to pick your pocket',
  });
  // A result without an allowed request is ignored.
  host.pickpocketing.report(thief, { kind: 'finished', victim: 8 });
  host.pickpocketing.report(thief, { kind: 'request', victim: 8 });
  host.pickpocketing.report(thief, { kind: 'finished', victim: 8 });
  const chat = output.filter((e) => e.op === 'chat').map((e) => e.text);
  assert.deepEqual(
    chat.filter((t) => /^(start|caught|robbed)/.test(t)),
    ['start 7 8', 'caught 7 8', 'start 7 8', 'robbed 7 8 0'],
  );
});

test('a gamemode refuses, turns Rob off and changes the victim notice', async () => {
  const { host, output } = hostWith(`
    kcds.core.Events.on('playerPickpocketStart', () => false);
  `);
  await host.startAll();
  const thief = player(host, 7, 'Tom');
  player(host, 8, 'Anna');
  host.pickpocketing.report(thief, { kind: 'request', victim: 8 });
  assert.equal(rules(output).at(-1).flag, false);
  const api = host.api.Pickpocketing;
  api.setEnabled(false);
  assert.equal(api.enabled, false);
  assert.deepEqual(
    rules(output)
      .filter((e) => e.kind === 'enabled')
      .slice(-2)
      .map((e) => [e.id, e.flag]),
    [
      [7, false],
      [8, false],
    ],
  );
  api.setVictimNotice(false, '{thief} ti chtěl vybrat kapsu');
  assert.equal(api.victimNotice, false);
  assert.equal(api.victimNoticeText, '{thief} ti chtěl vybrat kapsu');
  assert.throws(() => api.setEnabled('no'));
  assert.throws(() => api.setVictimNotice(true, 3));
});

test("the victim's notice comes in the victim's language", async () => {
  const { host, output } = hostWith(`
    kcds.core.Locales.set('czech', { 'pickpocket.victimNotice': '{thief} ti chtěl vybrat kapsu' });
  `);
  await host.startAll();
  const thief = player(host, 7, 'Tom');
  host.updatePlayer({
    id: 8,
    nickname: 'Anna',
    ready: true,
    language: 'czech',
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.equal(host.players.get(8).language, 'czech');
  host.pickpocketing.report(thief, { kind: 'request', victim: 8 });
  host.pickpocketing.report(thief, { kind: 'caught', victim: 8 });
  assert.equal(rules(output).at(-1).notice, 'Tom ti chtěl vybrat kapsu');
});

test('a gamemode turns stolen marks off for every player, later arrivals too', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  player(host, 7, 'Tom');
  const inventory = host.api.Inventory;
  assert.equal(inventory.stolenMarks, true);
  inventory.setStolenMarks(false);
  assert.equal(inventory.stolenMarks, false);
  player(host, 8, 'Anna');
  assert.deepEqual(
    rules(output)
      .filter((e) => e.kind === 'stolenMarks')
      .map((e) => [e.id, e.flag]),
    [
      [7, false],
      [8, false],
    ],
  );
  assert.throws(() => inventory.setStolenMarks('no'));
});

test('what the thief takes comes out of the victim, at most what they carry', async () => {
  const { host, output } = hostWith(`
    kcds.core.Events.on('playerPickpocketed', (t, v, items) =>
      kcds.core.Chat.sendToAll('robbed ' + JSON.stringify(items.map((l) => [l.name, l.amount]))));
  `);
  await host.startAll();
  const thief = player(host, 7, 'Tom');
  const victim = player(host, 8, 'Anna');
  const Inventory = host.api.Inventory;
  const pockets = [
    { id: 'm', item: 'money', amount: 10 },
    { id: 'k', item: 'keyring', amount: 1 },
  ];
  assert.equal(Inventory.set(victim, { items: pockets }).ok, true);
  assert.equal(Inventory.set(thief, { items: [] }).ok, true);
  host.pickpocketing.settleMs = 0;
  host.pickpocketing.report(thief, { kind: 'request', victim: 8 });
  const money = Inventory.get(victim).items[0].item;
  // The victim's keyring is not in the thief's loot screen.
  const shown = output.find((e) => e.op === 'pickpocketPockets');
  assert.deepEqual(
    shown.rows.map((row) => row[0]),
    [money],
  );
  host.playerInventories.native(thief, [{ kind: 'gained', item: money, amount: 15 }]);
  host.pickpocketing.report(thief, { kind: 'finished', victim: 8 });
  assert.deepEqual(
    Inventory.get(victim).items.map((row) => row.name),
    ['keyring'],
  );
  assert.deepEqual(
    Inventory.get(thief).items.map((row) => row.amount),
    [10],
  );
  const chat = output.filter((e) => e.op === 'chat').map((e) => e.text);
  assert.deepEqual(
    chat.filter((t) => t.startsWith('robbed')),
    ['robbed [["money",10]]'],
  );
  // The victim's game is never told what was taken (no notice of the loss).
  assert.equal(
    output.some((e) => e.op === 'itemNotice' && e.id === 8),
    false,
  );
});
