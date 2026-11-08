'use strict';

// The game's own travel between maps: a fast travel that ends at a level's travel point.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { ResourceHost } = require('../host.cjs');
const LEVELS = require('./level_travel.json');

const TO_KUTNOHORSKO = LEVELS.trosecko.marks.find((mark) => mark.to === 'kutnohorsko').position;

async function hostOn(maps) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-travel-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  host.setMaps(maps);
  await host.startAll();
  host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    gameplayReady: true,
    position: { x: 100, y: 200, z: 30 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const trips = () => output.filter((e) => e.op === 'playerMap');
  return { host, output, trips, player: host.players.get(7) };
}

test('the travel tables name both levels, each with a point and an arrival', () => {
  for (const level of ['trosecko', 'kutnohorsko']) {
    assert.ok(LEVELS[level].marks.length >= 1);
    assert.ok(LEVELS[level].arrival?.position);
  }
});

test('a fast travel that ends at the travel point takes the player to the other map', async () => {
  const { host, trips, player } = await hostOn(['trosecko', 'kutnohorsko']);
  const seen = [];
  host.on('playerTravelling', (who, trip) => {
    seen.push(trip);
  });
  // Elsewhere on the map: an ordinary fast travel.
  assert.equal(host.levelTravel.travelEnded(player, { x: 1000, y: 1000, z: 50 }), false);
  assert.equal(trips().length, 0);
  // On the road beside the point.
  const end = { x: TO_KUTNOHORSKO.x + 20, y: TO_KUTNOHORSKO.y - 15, z: TO_KUTNOHORSKO.z };
  assert.equal(host.levelTravel.travelEnded(player, end), true);
  assert.equal(seen.length, 1);
  assert.equal(seen[0].from, 'trosecko');
  assert.equal(seen[0].to, 'kutnohorsko');
  assert.equal(seen[0].available, true);
  assert.deepEqual(seen[0].position, LEVELS.kutnohorsko.arrival.position);
  assert.equal(trips().length, 1);
  assert.equal(trips()[0].map, 1);
  assert.equal(player.map, 'kutnohorsko');
});

test('a handler keeps the player, or sends them elsewhere', async () => {
  const { host, output, trips, player } = await hostOn(['trosecko', 'kutnohorsko']);
  let answer = false;
  host.on('playerTravelling', () => answer);
  assert.equal(host.levelTravel.travelEnded(player, TO_KUTNOHORSKO), false);
  assert.equal(trips().length, 0);
  // Somewhere else on the same map: placed there, no trip.
  answer = { map: 'trosecko', position: { x: 2425, y: 2470, z: 170 } };
  const before = output.length;
  assert.equal(host.levelTravel.travelEnded(player, TO_KUTNOHORSKO), true);
  assert.equal(trips().length, 0);
  const placed = output.slice(before).find((e) => e.op === 'entityPosition' && e.id === 7);
  assert.deepEqual([placed.position.x, placed.position.y], [2425, 2470]);
});

test('a server on one map, or with the travel off, keeps everyone at the point', async () => {
  const one = await hostOn(['trosecko']);
  let available = null;
  one.host.on('playerTravelling', (who, trip) => {
    available = trip.available;
  });
  assert.equal(one.host.levelTravel.travelEnded(one.player, TO_KUTNOHORSKO), false);
  assert.equal(available, false);
  assert.equal(one.trips().length, 0);
  assert.equal(one.player.map, 'trosecko');

  const two = await hostOn(['trosecko', 'kutnohorsko']);
  assert.equal(two.host.api.World.levelTravel, true);
  two.host.api.World.setLevelTravel(false);
  assert.equal(two.host.api.World.levelTravel, false);
  assert.equal(two.host.levelTravel.travelEnded(two.player, TO_KUTNOHORSKO), false);
  assert.equal(two.trips().length, 0);
  assert.throws(() => two.host.api.World.setLevelTravel('no'), TypeError);
});
