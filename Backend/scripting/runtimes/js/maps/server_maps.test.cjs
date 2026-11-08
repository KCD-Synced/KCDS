'use strict';

// The maps one server runs at once: the dimension values the native server tells them apart by,
// and a player's trip from one map to another.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { ResourceHost } = require('../host.cjs');
const {
  GLOBAL_WORLD,
  mapOf,
  mapsFromArguments,
  onMap,
  worldFitsMaps,
  worldOf,
} = require('./server_maps.cjs');

test('the first map keeps the one-map values; others carry their map', () => {
  assert.equal(onMap(0, GLOBAL_WORLD), GLOBAL_WORLD);
  assert.equal(onMap(0, 5), 5);
  assert.equal(onMap(1, GLOBAL_WORLD), 0x01ffffff);
  assert.equal(onMap(1, 5), 0x01000005);
  assert.equal(onMap(2, 7), 0x02000007);
  for (const map of [0, 1, 2]) {
    for (const world of [0, 5, GLOBAL_WORLD]) {
      assert.equal(mapOf(onMap(map, world)), map);
      assert.equal(worldOf(onMap(map, world)), world);
    }
  }
  assert.equal(worldFitsMaps(0x02000000, 1), true, 'one map: any world as before');
  assert.equal(worldFitsMaps(0x02000000, 2), false);
  assert.equal(worldFitsMaps(GLOBAL_WORLD, 2), true);
  assert.deepEqual(mapsFromArguments('trosecko', ''), ['trosecko']);
  assert.deepEqual(mapsFromArguments('trosecko', 'trosecko, kutnohorsko'), [
    'trosecko',
    'kutnohorsko',
  ]);
});

async function twoMapHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-maps-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  host.setMaps(['trosecko', 'kutnohorsko']);
  await host.startAll();
  const report = (extra) =>
    host.updatePlayer({
      id: 7,
      nickname: 'P7',
      ready: true,
      gameplayReady: true,
      position: { x: 100, y: 200, z: 30 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      ...extra,
    });
  report();
  return { host, output, report, player: host.players.get(7) };
}

test('a player sent to the other map gets its world and stands where the script said', async () => {
  const { host, output, report, player } = await twoMapHost();
  const { World } = host.api;
  assert.deepEqual(World.maps, ['trosecko', 'kutnohorsko']);
  assert.equal(player.map, 'trosecko');
  const changed = [];
  host.on('playerMapChanged', (who, to, from) => changed.push([who.id, to, from]));

  assert.equal(player.setMap('kutnohorsko', { x: 1500, y: 1600, z: 80 }), true);
  assert.equal(player.map, 'kutnohorsko');
  assert.deepEqual(
    output.filter((e) => e.op === 'playerMap'),
    [{ op: 'playerMap', id: 7, map: 1, dimension: 0x01ffffff }],
  );
  // A numbered world on the new map carries it.
  player.setDimension(5);
  assert.equal(output.filter((e) => e.op === 'entityWorld').at(-1).dimension, 0x01000005);
  host.confirmPlayerWorld({ id: 7, request: host.nextWorldRequest, dimension: 0x01000005 });
  assert.equal(player.dimension, 5, 'scripts see their own world');
  assert.deepEqual(changed, [], 'not before they stand in the new world');

  // Their game loads the new level: not ready, then ready again.
  report({ ready: false, dimension: 0x01000005 });
  const before = output.length;
  report({ ready: true, dimension: 0x01000005 });
  assert.deepEqual(changed, [[7, 'kutnohorsko', 'trosecko']]);
  const placed = output.slice(before).find((e) => e.op === 'entityPosition' && e.id === 7);
  assert.ok(placed, 'placed where the script said');
  assert.deepEqual([placed.position.x, placed.position.y], [1500, 1600]);
});

test('a server on one map stays on it: no trips, worlds as before', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-one-map-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  host.setMaps(['trosecko']);
  await host.startAll();
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 100, y: 200, z: 30 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const player = host.players.get(7);
  assert.deepEqual(host.api.World.maps, ['trosecko']);
  assert.equal(player.map, 'trosecko');
  assert.throws(
    () => player.setMap('kutnohorsko'),
    /Unknown map kutnohorsko: this server runs trosecko/,
  );
  // Any numbered world as a one-map server always took.
  player.setDimension(0x02000000);
  assert.equal(output.filter((e) => e.op === 'entityWorld').at(-1).dimension, 0x02000000);
  assert.equal(output.filter((e) => e.op === 'playerMap').length, 0);
  host.playerInventories.flush();
});

test('an unknown map throws; the map the player is on only places them', async () => {
  const { host, output, player } = await twoMapHost();
  assert.throws(() => player.setMap('rataje'), /Unknown map rataje/);
  assert.throws(() => player.setMap('kutnohorsko', { x: Number.NaN, y: 0, z: 0 }), TypeError);
  assert.equal(player.setMap('trosecko', { x: 10, y: 20, z: 30 }), true);
  assert.equal(output.filter((e) => e.op === 'playerMap').length, 0);
  assert.throws(() => player.setDimension(0x02000000), /Invalid dimension/);
  host.playerInventories.flush();
});
