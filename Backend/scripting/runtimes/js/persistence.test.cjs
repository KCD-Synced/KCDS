'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { progressionCatalog } = require('./progression/catalog.cjs');

// A character exported when its player leaves and restored when they come back,
// through the supported API only (guides/Players/restoring-stats.md, progression.md, buffs.md):
// player.progression, the stats, the buffs it carries; loadProgression, setHealth, setStat,
// setHunger, addBuff. The export carries its schema version and survives JSON storage.

const PACIFIST = '2b9f3c56-d5f4-49b0-a259-8a89a67c8f0d';
const ALCOHOLISM = 'cd727cba-0507-4e97-bab9-ae4fe6d55d07';

const RESOURCE = `
const SCHEMA = 1;
const saved = new Map();
const carried = new Map();
kcds.core.Events.on('playerBuffAdded', (player, buff) => {
  const list = carried.get(player.id) ?? new Set();
  list.add(buff);
  carried.set(player.id, list);
});
kcds.core.Events.on('playerBuffRemoved', (player, buff) => carried.get(player.id)?.delete(buff));
kcds.core.Events.on('playerDisconnect', (player) => {
  saved.set(player.nickname, JSON.stringify({
    schema: SCHEMA,
    progression: player.progression,
    stats: { health: player.health, exhaust: player.exhaust, hunger: player.hunger, stamina: player.stamina },
    buffs: [...(carried.get(player.id) ?? [])],
  }));
});
kcds.core.Events.on('playerReady', (player) => {
  const text = saved.get(player.nickname);
  if (!text) return;
  const exported = JSON.parse(text);
  if (exported.schema !== SCHEMA) return;
  player.loadProgression(exported.progression);
  player.setHealth(exported.stats.health);
  player.setStat('exhaust', exported.stats.exhaust);
  player.setHunger(exported.stats.hunger);
  player.setStat('stamina', exported.stats.stamina);
  for (const buff of exported.buffs) player.addBuff(buff);
});
`;

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-persistence-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  // The needs orders every arriving player gets while the needs are off (their default, issue)
  // are not what these test.
  host.api.Needs.setEnabled(true);
  const dir = path.join(root, 'saver');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'saver', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  return { host, output };
}

function join(host, id, ready) {
  return host.updatePlayer({
    id,
    nickname: 'Henry',
    ready,
    gameplayReady: ready,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    vitals: {
      health: 60,
      stamina: 106.68,
      exhaust: 49.97,
      hunger: 39.97,
      bleeding: 0,
      sleeping: 0,
      consciousness: 1,
      drunkenness: 0,
      poisoning: 0,
    },
  });
}

test('a character exported on leaving comes back the same on rejoining', async () => {
  const { host, output } = hostWith(RESOURCE);
  await host.startAll();
  join(host, 7, true);
  const tracks = progressionCatalog
    .tracks()
    .map((track) => (track.name === 'survival' ? [22, 50, 9] : [5, 0, 0]));
  host.updateProgression(7, { tracks, mainPoints: 1, perks: [PACIFIST] });
  host.updateBuffs(7, [[ALCOHOLISM, '42', false]]);
  host.disconnect(7);
  // The same person on a new connection, with a fresh character.
  output.length = 0;
  join(host, 9, false);
  join(host, 9, true);
  const orders = output.filter((e) => e.id === 9);
  const restore = orders.find((e) => e.op === 'progressionOrder' && e.kind === 'restore');
  // The order was built in the resource's own context: copy it into this one to compare.
  const survival = progressionCatalog.tracks().findIndex((track) => track.name === 'survival');
  assert.deepEqual([...restore.snapshot.tracks[survival]], [22, 50, 9]);
  assert.deepEqual([...restore.snapshot.perks], [PACIFIST]);
  assert.equal(restore.snapshot.mainPoints, 1);
  assert.deepEqual(
    orders.filter((e) => e.op === 'setStat').map((e) => [e.stat, e.value]),
    [
      [0, 60],
      [2, 49.97],
      [3, 39.97],
      [1, 106.68],
    ],
  );
  assert.deepEqual(
    orders.filter((e) => e.op === 'buffOrder').map((e) => [e.kind, e.buff]),
    [['add', ALCOHOLISM]],
  );
});
