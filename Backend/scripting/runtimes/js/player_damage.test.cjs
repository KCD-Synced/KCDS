'use strict';

// playerDamage: health a player's own game took off them.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

test('a damage report raises playerDamage with the attacker, part and reason', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-damage-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'referee');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'referee', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    `kcds.core.Events.on('playerDamage', (p, by, amount, part, reason) =>
      kcds.core.Chat.sendToAll([p.id, by ? by.id : 'none', amount, part, reason].join(' ')));
    kcds.core.Events.on('playerAttack', (p, other) => kcds.core.Chat.sendToAll('attack ' + p.id + ' ' + other.id));
    kcds.core.Events.on('playerCombatCancelled', (a, b) => kcds.core.Chat.sendToAll('cancelled ' + a.id + ' ' + b.id));`,
  );
  await host.startAll();
  for (const id of [7, 8]) {
    host.updatePlayer({
      id,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  }
  assert.equal(
    host.playerDamage({ id: 7, attacker: 8, amount: 12.5, reason: 1, subpart: 24 }),
    true,
  );
  assert.equal(host.playerDamage({ id: 7, attacker: 0, amount: 10, reason: 5, subpart: -1 }), true);
  assert.equal(
    host.playerDamage({ id: 7, attacker: 999, amount: 3, reason: 7, subpart: 55 }),
    true,
  );
  assert.equal(host.playerDamage({ id: 7, attacker: 0, amount: 0, reason: 7, subpart: -1 }), false);
  assert.equal(
    host.playerDamage({ id: 42, attacker: 0, amount: 1, reason: 7, subpart: -1 }),
    false,
  );
  assert.equal(host.playerAttack({ id: 8, opponent: 7 }), true);
  assert.equal(host.playerAttack({ id: 8, opponent: 99 }), false);
  const [p7, p8] = [7, 8].map((id) => host.players.get(id));
  assert.equal(p7.stopFight(p8), true);
  assert.equal(p7.stopFight(p7), false);
  assert.deepEqual(
    output.filter((e) => e.op === 'combatCancel').map((e) => [e.id, e.opponent]),
    [
      [7, 8],
      [8, 7],
    ],
  );
  assert.deepEqual(
    output.filter((e) => e.op === 'chat').map((e) => e.text),
    [
      '7 8 12.5 head combat',
      '7 none 10  scripted',
      '7 none 3 rightLeg fall',
      'attack 8 7',
      'cancelled 7 8',
    ],
  );
});

// who a player fights, and makePeace ending every fight.
test('player.opponents remembers swings either way and makePeace ends those fights', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-peace-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  await host.startAll();
  for (const id of [7, 8, 9]) {
    host.updatePlayer({
      id,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  }
  const [p7, p8, p9] = [7, 8, 9].map((id) => host.players.get(id));
  host.playerAttack({ id: 8, opponent: 7 });
  host.playerAttack({ id: 7, opponent: 9 });
  assert.deepEqual(p7.opponents.map((p) => p.id).sort(), [8, 9]);
  assert.deepEqual(p8.opponents, [p7]);
  assert.equal(p7.makePeace(), 2);
  assert.deepEqual(p7.opponents, []);
  assert.deepEqual(p9.opponents, []);
  assert.equal(output.filter((e) => e.op === 'combatCancel').length, 4);
  assert.equal(p8.makePeace(), 0);
});

test('startFight locks both players on each other until stopFight, a knock-out or a disconnect', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-fights-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'duels');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'duels', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    `kcds.core.Events.on('playerFightStarted', (a, b) => kcds.core.Chat.sendToAll('started ' + a.id + ' ' + b.id));
    kcds.core.Events.on('playerFightEnded', (a, b, reason) =>
      kcds.core.Chat.sendToAll('ended ' + a.id + ' ' + b.id + ' ' + reason));`,
  );
  await host.startAll();
  const at = { 7: { x: 0, y: 0, z: 0 }, 8: { x: 3, y: 0, z: 0 }, 9: { x: 40, y: 0, z: 0 } };
  for (const id of [7, 8, 9]) {
    host.updatePlayer({ id, ready: true, position: at[id], rotation: { w: 1, x: 0, y: 0, z: 0 } });
  }
  const [p7, p8, p9] = [7, 8, 9].map((id) => host.players.get(id));
  const chat = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  // The same player, one too far away: no fight.
  assert.equal(p7.startFight(p7), false);
  assert.equal(p7.startFight(p9), false);
  assert.equal(p7.startFight(p8), true);
  assert.equal(p8.startFight(p7), false, 'the pair fights already');
  assert.deepEqual(
    output.filter((e) => e.op === 'combatStart').map((e) => [e.id, e.opponent]),
    [
      [7, 8],
      [8, 7],
    ],
  );
  assert.deepEqual(p7.fights, [p8]);
  assert.deepEqual(p8.fights, [p7]);
  // stopFight ends it.
  assert.equal(p8.stopFight(p7), true);
  assert.deepEqual(p7.fights, []);
  // A knock-out ends the next one, and a knocked-out player cannot fight.
  assert.equal(p7.startFight(p8), true);
  host.playerDied(8);
  assert.equal(p7.startFight(p8), false);
  p8.revive();
  // A disconnect ends the last one.
  assert.equal(p8.startFight(p7), true);
  host.disconnect(8);
  assert.deepEqual(chat(), [
    'started 7 8',
    'ended 7 8 stopped',
    'started 7 8',
    'ended 7 8 knockedOut',
    'started 8 7',
    'ended 8 7 left',
  ]);
  assert.deepEqual(p7.fights, []);
});
