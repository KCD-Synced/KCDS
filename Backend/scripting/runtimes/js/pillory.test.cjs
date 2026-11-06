'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// Player.putInPillory, Player.releaseFromPillory and playerPilloryReleased on the server (issue).

function hostWith(code = '') {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-pillory-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'gamemode');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'gamemode', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    `kcds.core.Events.on('playerPilloryReleased', (player, reason) =>
       kcds.core.Chat.sendToAll('released ' + player.id + ' ' + reason));
     ${code}`,
  );
  return { host, output };
}

function player(host, id, ready = true) {
  return host.updatePlayer({
    id,
    nickname: 'P' + id,
    ready,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

const orders = (output) => output.filter((e) => e.op === 'pillory').map((e) => ({ ...e }));
const releases = (output) =>
  output
    .filter((e) => e.op === 'chat' && /^released /.test(e.text))
    .map((e) => e.text.slice('released '.length));
const report = (host, id, reason) => host.pillories.report(host.players.get(id), { id, reason });

test('putInPillory sends the order to its owner only and returns true', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  player(host, 2);
  assert.equal(owner.putInPillory('pillory_village'), true);
  assert.deepEqual(orders(output), [{ op: 'pillory', id: 1, kind: 1, holder: 'pillory_village' }]);
});

test('putInPillory takes an entity name with spaces and refuses what the wire cannot carry', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  assert.equal(owner.putInPillory('Pillory 01'), true);
  assert.throws(() => owner.putInPillory(null), TypeError);
  assert.throws(() => owner.putInPillory(7), TypeError);
  assert.throws(() => owner.putInPillory(''), RangeError);
  assert.throws(() => owner.putInPillory('a'.repeat(256)), RangeError);
  assert.throws(() => owner.putInPillory('tab\there'), RangeError);
  assert.throws(() => owner.putInPillory('prangerä'), RangeError);
  assert.equal(owner.putInPillory('a'.repeat(255)), true);
  assert.equal(
    owner.putInPillory(
      'so_pillory[Pillory/pillory1:pillory_prefab[Pillory/pillory1]_2dca5774-55cc-0a7c-1b1c-1432c8cc3f7e]',
    ),
    true,
  );
  assert.equal(orders(output).length, 3);
});

test('putInPillory is false for a player with no body yet and sends nothing', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1, false);
  assert.equal(owner.putInPillory('pillory_village'), false);
  assert.equal(owner.releaseFromPillory(), false);
  assert.deepEqual(orders(output), []);
});

test('releaseFromPillory takes the player out and raises the event with script', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  assert.equal(owner.releaseFromPillory(), false);
  owner.putInPillory('pillory_village');
  assert.equal(owner.releaseFromPillory(), true);
  assert.equal(owner.releaseFromPillory(), false);
  assert.deepEqual(orders(output), [
    { op: 'pillory', id: 1, kind: 1, holder: 'pillory_village' },
    { op: 'pillory', id: 1, kind: 2, holder: '' },
  ]);
  assert.deepEqual(releases(output), ['1 script']);
});

test('A lockpick report raises the event once with lockpicked and ends the stay', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  owner.putInPillory('pillory_village');
  report(host, 1, 1);
  report(host, 1, 1);
  assert.deepEqual(releases(output), ['1 lockpicked']);
  // Nothing is left to release, and nothing is ordered again when the world gets ready.
  assert.equal(owner.releaseFromPillory(), false);
  player(host, 1);
  assert.equal(orders(output).length, 1);
});

test('A report after the script released the player raises nothing', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  owner.putInPillory('pillory_village');
  owner.releaseFromPillory();
  report(host, 1, 1);
  report(host, 1, 2);
  assert.deepEqual(releases(output), ['1 script']);
});

test('A client that left without a lockpick clears the state and raises nothing', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  owner.putInPillory('pillory_village');
  report(host, 1, 2);
  assert.deepEqual(releases(output), []);
  assert.equal(owner.releaseFromPillory(), false);
  // Unknown reasons and unknown players are ignored.
  owner.putInPillory('pillory_village');
  report(host, 1, 0);
  report(host, 1, 9);
  host.pillories.report(undefined, { id: 5, reason: 1 });
  assert.deepEqual(releases(output), []);
  assert.equal(owner.releaseFromPillory(), true);
});

test('A player whose world gets ready again is put into the same pillory again', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  owner.putInPillory('pillory_village');
  player(host, 1, false);
  player(host, 1);
  assert.deepEqual(orders(output), [
    { op: 'pillory', id: 1, kind: 1, holder: 'pillory_village' },
    { op: 'pillory', id: 1, kind: 1, holder: 'pillory_village' },
  ]);
});

test('A second putInPillory moves the player to the new pillory', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  owner.putInPillory('pillory_a');
  owner.putInPillory('pillory_b');
  player(host, 1, false);
  player(host, 1);
  assert.deepEqual(
    orders(output).map((e) => e.holder),
    ['pillory_a', 'pillory_b', 'pillory_b'],
  );
});

const INSTANCE =
  'Pillory/pillory1:pillory_prefab[Pillory/pillory1]_2dca5774-55cc-0a7c-1b1c-1432c8cc3f7e';
const holderOf = (instance) => `so_pillory[${instance}]`;
const lockOf = (instance) => `pillory_lockpickable[${instance}]`;
const lockOpened = (host, id, lock) =>
  host.pillories.report(host.players.get(id), { id, reason: 3, lock });

test('A lock of the same pillory instance, opened in a game, releases the player in it', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  player(host, 2);
  owner.putInPillory(holderOf(INSTANCE));
  lockOpened(host, 1, lockOf(INSTANCE));
  assert.deepEqual(releases(output), ['1 lockpicked']);
  // The client is told to leave as releaseFromPillory does, and the stay is over.
  assert.deepEqual(
    orders(output).map((e) => [e.kind, e.holder]),
    [
      [1, holderOf(INSTANCE)],
      [2, ''],
    ],
  );
  assert.equal(owner.releaseFromPillory(), false);
  player(host, 1, false);
  player(host, 1);
  assert.equal(orders(output).length, 2);
});

test('A lock of another pillory instance releases nobody', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  player(host, 2);
  owner.putInPillory(holderOf(INSTANCE));
  lockOpened(host, 2, lockOf('Pillory/pillory2:pillory_prefab[Pillory/pillory2]_00000000'));
  lockOpened(host, 2, lockOf(INSTANCE.replace('2dca5774', '2dca5775')));
  assert.deepEqual(releases(output), []);
  assert.equal(orders(output).length, 1);
  assert.equal(owner.releaseFromPillory(), true);
});

test('A report from a player who is not in the pillory releases the one who is', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  const picker = player(host, 2);
  owner.putInPillory(holderOf(INSTANCE));
  assert.equal(picker.releaseFromPillory(), false);
  lockOpened(host, 2, lockOf(INSTANCE));
  assert.deepEqual(releases(output), ['1 lockpicked']);
  // A second report finds nobody in it.
  lockOpened(host, 2, lockOf(INSTANCE));
  assert.deepEqual(releases(output), ['1 lockpicked']);
});

test('A lock opened in a game releases every player in that pillory and no other', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const other = 'Pillory/pillory2:pillory_prefab[Pillory/pillory2]_11111111';
  player(host, 1).putInPillory(holderOf(INSTANCE));
  player(host, 2).putInPillory(holderOf(INSTANCE));
  player(host, 3).putInPillory(holderOf(other));
  player(host, 4);
  lockOpened(host, 4, lockOf(INSTANCE));
  assert.deepEqual(releases(output).sort(), ['1 lockpicked', '2 lockpicked']);
  assert.equal(host.players.get(3).releaseFromPillory(), true);
});

test('A lock or a holder with no prefab instance releases nobody', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  player(host, 1).putInPillory('so_pillory');
  player(host, 2).putInPillory(holderOf(INSTANCE));
  player(host, 3);
  lockOpened(host, 3, 'pillory_lockpickable');
  lockOpened(host, 3, undefined);
  lockOpened(host, 3, 7);
  lockOpened(host, 3, '');
  assert.deepEqual(releases(output), []);
  host.pillories.report(undefined, { id: 5, reason: 3, lock: lockOf(INSTANCE) });
  assert.deepEqual(releases(output), []);
});

test('A player who leaves is not released with an event', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  owner.putInPillory('pillory_village');
  host.pillories.left(owner);
  report(host, 1, 1);
  assert.deepEqual(releases(output), []);
  assert.equal(owner.putInPillory('pillory_village'), false);
});

const VILLAGE =
  'so_pillory[Pillory/pillory1:pillory_prefab[Pillory/pillory1]_2dca5774-55cc-0a7c-1b1c-1432c8cc3f7e]';
const entered = (host, id, holder) =>
  host.pillories.report(host.players.get(id), { id, reason: 4, lock: holder });

test('putInPillory without a name asks for the nearest pillory; its game names it', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  assert.equal(owner.putInPillory(), true);
  assert.deepEqual(orders(output), [{ op: 'pillory', id: 1, kind: 3, holder: '' }]);
  assert.equal(owner.pillory, null);
  entered(host, 1, VILLAGE);
  assert.equal(owner.pillory, VILLAGE);
  // Named now: a new world gets the name, and the pillory's lock lets the player out.
  player(host, 1, false);
  player(host, 1);
  assert.deepEqual(orders(output)[1], { op: 'pillory', id: 1, kind: 1, holder: VILLAGE });
  host.pillories.lockOpened(VILLAGE.replace('so_pillory', 'pillory_lockpickable'));
  assert.deepEqual(releases(output), ['1 lockpicked']);
  assert.equal(owner.pillory, null);
});

test('Before its game named the nearest pillory, a new world asks for the nearest again', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  owner.putInPillory();
  player(host, 1, false);
  player(host, 1);
  assert.deepEqual(orders(output), [
    { op: 'pillory', id: 1, kind: 3, holder: '' },
    { op: 'pillory', id: 1, kind: 3, holder: '' },
  ]);
});

test('An entered report counts only for a nearest put-in with a valid name', async () => {
  const { host } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  owner.putInPillory('pillory_village');
  entered(host, 1, VILLAGE);
  assert.equal(owner.pillory, 'pillory_village');
  owner.putInPillory();
  entered(host, 1, 'tab\there');
  assert.equal(owner.pillory, null);
  // A game with no pillory near reports it left: the stay is over, with no event.
  report(host, 1, 2);
  assert.equal(owner.pillory, null);
  assert.equal(owner.releaseFromPillory(), false);
});

test('Player.pillory names the pillory while playerDisconnect runs, for a relog', async () => {
  const { host, output } = hostWith(
    `kcds.core.Events.on('playerDisconnect', (player) => kcds.core.Chat.sendToAll('left in ' + player.pillory));`,
  );
  await host.startAll();
  const owner = player(host, 1);
  assert.equal(owner.pillory, null);
  owner.putInPillory(VILLAGE);
  assert.equal(owner.pillory, VILLAGE);
  host.disconnect(1);
  const said = output.filter((e) => e.op === 'chat' && /^left in /.test(e.text));
  assert.deepEqual(
    said.map((e) => e.text),
    ['left in ' + VILLAGE],
  );
  // Back with a new id: the resource puts them into the same pillory.
  const back = player(host, 2);
  assert.equal(back.putInPillory(VILLAGE), true);
  assert.equal(back.pillory, VILLAGE);
});
