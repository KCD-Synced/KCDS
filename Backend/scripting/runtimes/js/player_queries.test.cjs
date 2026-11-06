'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// Player.inRadius and Player.nearest (KCDS additions): the players near a position.

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-player-queries-'));
  const host = new ResourceHost(root, { send: () => {}, log: () => {} });
  await host.startAll();
  return host;
}

function player(host, id, x, ready = true) {
  return host.updatePlayer({
    id,
    nickname: 'P' + id,
    ready,
    position: { x, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

const at = (x) => ({ x, y: 0, z: 0 });
const ids = (players) => players.map((p) => p.id);

test('inRadius lists the players within the radius, nearest first', async () => {
  const host = await startedHost();
  const Player = player(host, 1, 10).constructor;
  const second = player(host, 2, 3);
  player(host, 3, -5);
  player(host, 4, 1, false);
  assert.deepEqual(ids(Player.inRadius(at(0), 6)), [2, 3]);
  assert.deepEqual(ids(Player.inRadius(at(0), 100)), [2, 3, 1]);
  assert.deepEqual(ids(Player.inRadius(at(0), 2)), []);
  assert.deepEqual(ids(Player.inRadius(at(0), 100, { except: second })), [3, 1]);
  assert.deepEqual(ids(Player.inRadius(at(0), Infinity)), [2, 3, 1]);
});

test('nearest gives the closest player or null, within a radius when given', async () => {
  const host = await startedHost();
  const Player = player(host, 1, 10).constructor;
  const second = player(host, 2, 3);
  assert.equal(Player.nearest(at(9)).id, 1);
  assert.equal(Player.nearest(at(0)).id, 2);
  assert.equal(Player.nearest(at(0), { radius: 1 }), null);
  assert.equal(Player.nearest(at(3), { except: second }).id, 1);
  assert.equal(Player.nearest(second.position, { except: second }).id, 1);
});

test('the queries keep to a dimension when asked and refuse bad input', async () => {
  const host = await startedHost();
  const first = player(host, 1, 1);
  const Player = first.constructor;
  player(host, 2, 2);
  const world = first.dimension;
  assert.deepEqual(ids(Player.inRadius(at(0), 10, { dimension: world })), [1, 2]);
  assert.deepEqual(ids(Player.inRadius(at(0), 10, { dimension: world + 1 })), []);
  assert.throws(() => Player.inRadius({ x: 0, y: 0 }, 10), TypeError);
  assert.throws(() => Player.inRadius(at(0), 0), RangeError);
  assert.throws(() => Player.inRadius(at(0), Number.NaN), RangeError);
  assert.throws(() => Player.nearest(null), TypeError);
});
