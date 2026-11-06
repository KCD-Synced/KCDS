'use strict';

// The level's own items: the server decides who takes one up, once.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { classBytesHex, lookupItemClass } = require('./inventory/catalog.cjs');
const { parseKey } = require('./ground/world_pickables.cjs');

async function fixture(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-world-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'rules');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'rules', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  await host.startAll();
  const join = (id, position) => {
    host.updatePlayer({ id, ready: true, position, rotation: { w: 1, x: 0, y: 0, z: 0 } });
    return host.players.get(id);
  };
  const sent = (op) => output.filter((e) => e.op === op);
  return { host, join, sent, output };
}

const TANKARD = classBytesHex(lookupItemClass('apple'));
const KEY = `${TANKARD}@100,200,300`;

test('the first player within reach takes a level item up; everyone else loses it', async () => {
  const { host, join, sent, output } = await fixture(`
    kcds.core.Events.on('worldItemPickup', (player, item) => {
      kcds.core.Chat.sendToAll(['pickup', player.id, item.name, item.position.x, item.position.z].join(' '));
    });
  `);
  const near = join(7, { x: 1, y: 2, z: 3 });
  const other = join(8, { x: 1.5, y: 2, z: 3 });
  const far = join(9, { x: 50, y: 2, z: 3 });
  assert.equal(host.worldPickables.pickup(far, KEY), false, 'out of reach');
  assert.equal(host.worldPickables.pickup(near, KEY), true);
  assert.equal(host.worldPickables.pickup(other, KEY), false, 'already taken');
  assert.equal(host.worldPickables.pickup(near, 'not a key'), false);
  assert.deepEqual(
    sent('worldPickupVerdict').map((e) => [e.id, e.granted]),
    [
      [9, false],
      [7, true],
      [8, false],
    ],
  );
  assert.deepEqual(
    sent('worldTaken').filter((e) => e.id === 0),
    [{ op: 'worldTaken', id: 0, full: false, keys: [KEY] }],
  );
  assert.deepEqual(
    sent('itemNotice').map((e) => [e.id, e.item, e.amount, e.gained]),
    [[7, lookupItemClass('apple').id, 1, true]],
    "only the taker's game shows its notice",
  );
  assert.deepEqual(
    output.filter((e) => e.op === 'chat').map((e) => e.text),
    ['pickup 7 apple 1 3'],
  );
  // A player who comes later gets the whole list.
  join(10, { x: 0, y: 0, z: 0 });
  assert.deepEqual(sent('worldTaken').at(-1), {
    op: 'worldTaken',
    id: 10,
    full: true,
    keys: [KEY],
  });
});

test('worldItemPickup may refuse; keys are checked', async () => {
  const { host, join, sent } = await fixture(
    `kcds.core.Events.on('worldItemPickup', () => false);`,
  );
  const p = join(7, { x: 1, y: 2, z: 3 });
  assert.equal(host.worldPickables.pickup(p, KEY), false);
  assert.deepEqual(sent('worldPickupVerdict').at(-1).granted, false);
  assert.equal(sent('worldTaken').filter((e) => e.id === 0).length, 0);
  assert.deepEqual(parseKey(KEY).position, { x: 1, y: 2, z: 3 });
  assert.equal(parseKey(`${TANKARD}@1,2`), null);
  assert.equal(parseKey(`${TANKARD.toUpperCase()}@1,2,3`), null);
});

test('the level items can be turned off: every game is told and no pick-up is granted', async () => {
  const { host, join, sent } = await fixture('');
  const World = host.api.World;
  assert.equal(World.levelItemsEnabled, true);
  const near = join(7, { x: 1, y: 2, z: 3 });
  assert.equal(World.setLevelItemsEnabled(false), true);
  assert.equal(World.levelItemsEnabled, false);
  assert.deepEqual(sent('worldItemsEnabled').at(-1), {
    op: 'worldItemsEnabled',
    id: 0,
    enabled: false,
  });
  assert.equal(host.worldPickables.pickup(near, KEY), false);
  assert.equal(sent('worldPickupVerdict').at(-1).granted, false);
  // A player whose world gets ready later hears it too.
  host.worldPickables.arrived(join(8, { x: 0, y: 0, z: 0 }));
  assert.deepEqual(sent('worldItemsEnabled').at(-1), {
    op: 'worldItemsEnabled',
    id: 8,
    enabled: false,
  });
  const before = sent('worldItemsEnabled').length;
  World.setLevelItemsEnabled(false);
  assert.equal(sent('worldItemsEnabled').length, before, 'no change, no message');
  World.setLevelItemsEnabled(true);
  assert.equal(host.worldPickables.pickup(near, KEY), true);
  assert.throws(() => World.setLevelItemsEnabled('no'), TypeError);
});

test('level-items.json turns them off from the start', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-world-off-'));
  fs.writeFileSync(path.join(root, 'level-items.json'), JSON.stringify({ enabled: false }));
  const host = new ResourceHost(root, { send: () => {}, log: () => {} });
  await host.startAll();
  assert.equal(host.api.World.levelItemsEnabled, false);
});

// some of the level's items gone for everyone, by place and class.
test('removeLevelItems sends the places to every game and to late comers until restore', async () => {
  const { host, join, sent } = await fixture('');
  const World = host.api.World;
  const rack = World.removeLevelItems({
    position: { x: 10, y: 20, z: 30 },
    radius: 1,
    item: 'apple',
  });
  const any = World.removeLevelItems({ position: { x: 1, y: 2, z: 3 } });
  assert.deepEqual(sent('worldItemsRemoved').at(-1), {
    op: 'worldItemsRemoved',
    id: 0,
    areas: [
      { center: { x: 10, y: 20, z: 30 }, radius: 1, item: TANKARD },
      { center: { x: 1, y: 2, z: 3 }, radius: 0.5, item: '' },
    ],
  });
  host.worldPickables.arrived(join(8, { x: 0, y: 0, z: 0 }));
  assert.equal(sent('worldItemsRemoved').at(-1).id, 8);
  rack.restore();
  assert.deepEqual(
    sent('worldItemsRemoved')
      .at(-1)
      .areas.map((a) => a.radius),
    [0.5],
  );
  any.restore();
  assert.deepEqual(sent('worldItemsRemoved').at(-1).areas, []);
  assert.throws(() => World.removeLevelItems({ position: { x: 1, y: 2, z: 3 }, radius: 0 }));
  assert.throws(() => World.removeLevelItems({ position: { x: 1 } }), TypeError);
  assert.throws(() => World.removeLevelItems({ position: { x: 1, y: 2, z: 3 }, item: 'nope' }));
});
