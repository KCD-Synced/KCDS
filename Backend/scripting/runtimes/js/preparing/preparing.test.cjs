'use strict';

// playerPreparing allows or refuses cooking, smoking and drying.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { ResourceHost } = require('../host.cjs');

test('playerPreparing: a resource refuses smoking, allows cooking; nothing to say allows', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-preparing-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const dir = path.join(root, 'kitchen');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'kitchen', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    [
      "kcds.core.Events.on('playerPreparing', (player, preparing) => {",
      "  console.log(preparing.kind + ' ' + Math.round(preparing.position.x) + ' ' + Object.isFrozen(preparing));",
      "  return preparing.kind !== 'smoking';",
      '});',
    ].join('\n'),
  );
  const output = [];
  const logs = [];
  const host = new ResourceHost(root, {
    send: (e) => output.push(e),
    log: (resource, text) => logs.push(text),
  });
  t.after(() => host.shutdown());
  await host.startAll();
  const player = host.updatePlayer({
    id: 9,
    ready: true,
    position: { x: 2038, y: 2490, z: 115 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const station = { x: 2038.4, y: 2489.7, z: 114.9 };
  host.preparing.ask(player, { request: 0x40000001, reason: 'smoking', station });
  host.preparing.ask(player, { request: 0x40000002, reason: 'cooking', station });
  host.preparing.ask(player, { request: 0x40000003, reason: 'baking', station });
  assert.deepEqual(
    output.filter((e) => e.op === 'craftVerdict'),
    [
      { op: 'craftVerdict', id: 9, request: 0x40000001, allowed: false },
      { op: 'craftVerdict', id: 9, request: 0x40000002, allowed: true },
      { op: 'craftVerdict', id: 9, request: 0x40000003, allowed: false },
    ],
  );
  assert.deepEqual(
    logs.filter((line) => line !== 'started'),
    ['smoking 2038 true', 'cooking 2038 true'],
  );
  await host.stop('kitchen');
  host.preparing.ask(player, { request: 0x40000004, reason: 'drying', station });
  assert.deepEqual(output.filter((e) => e.op === 'craftVerdict').at(-1), {
    op: 'craftVerdict',
    id: 9,
    request: 0x40000004,
    allowed: true,
  });
});

test('Preparation: station on the ask; playerPrepared pairs what the game converted', (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const { FoodPreparing } = require('./preparing.cjs');
  const raised = [];
  const preparing = new FoodPreparing({
    emit: (name, player, payload) => {
      raised.push([name, player.id, payload]);
      return true;
    },
    send: () => {},
    vector: (xyz) => ({ ...xyz }),
    stationAt: (kind, position) => (kind === 'drying' && position.x > 0 ? 'trosecko:abc' : null),
  });
  const player = { id: 9, dimension: 0 };
  preparing.ask(player, { request: 1, reason: 'drying', station: { x: 5, y: 6, z: 7 } });
  assert.equal(raised[0][0], 'playerPreparing');
  assert.equal(raised[0][2].station, 'trosecko:abc');
  assert.equal(raised[0][2].items, undefined, 'the rows are not known at the ask');
  const row = (item, amount) => ({ item, amount, metadata: {} });
  preparing.inventoryChanged(player, {
    reason: 'drop',
    items: [{ id: 'r1', before: row('meat_raw', 3), after: null }],
  });
  preparing.inventoryChanged(player, {
    reason: 'loot',
    items: [{ id: 'r2', before: null, after: row('meat_dried', 3) }],
  });
  preparing.inventoryChanged(player, { reason: 'script', items: [] });
  t.mock.timers.tick(2999);
  assert.equal(raised.length, 1, 'still converting');
  t.mock.timers.tick(1);
  assert.equal(raised[1][0], 'playerPrepared');
  assert.deepEqual(raised[1][2].items, [
    { id: 'r1', item: 'meat_raw', amount: 3, product: 'meat_dried' },
  ]);
  assert.equal(raised[1][2].station, 'trosecko:abc');
  assert.equal(raised[1][2].kind, 'drying');

  // The game reports food it takes as 'use': a smoked cheese comes the same way.
  preparing.ask(player, { request: 3, reason: 'smoking', station: { x: 1, y: 2, z: 3 } });
  preparing.inventoryChanged(player, {
    reason: 'use',
    items: [{ id: 'r2', before: { item: 'cheese', amount: 3 }, after: null }],
  });
  preparing.inventoryChanged(player, {
    reason: 'loot',
    items: [{ id: 'r3', before: null, after: { item: 'cheese_smoked', amount: 3 } }],
  });
  t.mock.timers.tick(3000);
  assert.deepEqual(raised.at(-1)[2].items, [
    { id: 'r2', item: 'cheese', amount: 3, product: 'cheese_smoked' },
  ]);
  raised.length = 2;

  // A preparing at a fire has no station; one that converts nothing raises nothing.
  preparing.ask(player, { request: 2, reason: 'cooking', station: { x: 5, y: 6, z: 7 } });
  assert.equal(raised[2][2].station, null);
  t.mock.timers.tick(30000);
  assert.equal(raised.length, 3);
});
