'use strict';

// GroundItem: stacks the server lays in the world.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-ground-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'loot');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'loot', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  const chat = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  return { host, chat, output };
}

test('GroundItem.spawn lays a stack every client builds, and destroy takes it away', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('groundItemSpawn', (g) => kcds.core.Chat.sendToAll('spawn ' + g));
    kcds.core.Events.on('groundItemDestroy', (g) => kcds.core.Chat.sendToAll('destroy ' + g.id + ' ' + g.amount));
    kcds.world.GroundItem.spawn('apple', { x: 1, y: 2, z: 3 }, { x: 0, y: 0, z: 90 }, 1);
    kcds.world.GroundItem.spawn('longSwordDuel', { x: 4, y: 5, z: 6 }, {}, 1, 7, { quality: 3, condition: 0.5 });
    kcds.world.GroundItem.spawn('arrow_crude', { x: 0, y: 0, z: 0 }, {}, 20);
  `);
  await host.startAll();
  const [apples, sword, arrows] = host.makeAPI().GroundItem.all();
  const spawns = output.filter((e) => e.op === 'groundItemSpawn');
  assert.equal(spawns.length, 3);
  assert.deepEqual(
    { ...spawns[0], position: { ...spawns[0].position } },
    {
      op: 'groundItemSpawn',
      id: apples.id,
      itemClass: apples.itemClass,
      amount: 1,
      quality: 0,
      health: -1,
      position: { x: 1, y: 2, z: 3 },
      rotation: spawns[0].rotation,
      dimension: 0xffffffff,
      leftBy: 0,
      resting: false,
    },
  );
  assert.equal(sword.itemClass, '46db8ef200cca9e319099351ec86bf8c');
  assert.equal(sword.quality, 3);
  assert.equal(sword.condition, 0.5);
  assert.ok(sword.health > 0 && sword.health < 1);
  assert.equal(sword.dimension, 7);
  assert.equal(arrows.quality, 1, 'ammunition is made at quality 1');
  assert.equal(arrows.health, 1);
  // Not resting until a game reports where it came to rest.
  assert.equal(apples.resting, false);
  assert.equal(apples.leftBy, null);
  assert.equal(apples.position.x, 1);
  assert.ok(Math.abs(apples.rotation.w - Math.SQRT1_2) < 1e-6);
  assert.match(String(apples), /^GroundItem\(\d+, [0-9a-f]{32} x1, settling\)$/);
  assert.equal(chat()[0], `spawn ${apples}`);
  const api2 = host.makeAPI();
  assert.deepEqual(
    api2.GroundItem.all().map((g) => g.id),
    [apples.id, sword.id, arrows.id],
  );
  assert.equal(api2.GroundItem.getById(sword.id), sword);
  assert.equal(new api2.Entity(sword.id), sword);
  sword.destroy();
  sword.destroy();
  assert.equal(output.filter((e) => e.op === 'groundItemDestroy').length, 1);
  assert.equal(api2.GroundItem.getById(sword.id), null);
  assert.equal(api2.GroundItem.destroyAll(0xffffffff), 2);
  assert.equal(api2.GroundItem.all().length, 0);
  assert.deepEqual(chat().slice(3), [
    `destroy ${sword.id} 1`,
    `destroy ${apples.id} 1`,
    `destroy ${arrows.id} 20`,
  ]);
});

test('GroundItem.spawn refuses what no client could build', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const { GroundItem } = host.makeAPI();
  assert.throws(() => GroundItem.spawn('no such item'), TypeError);
  assert.throws(() => GroundItem.spawn('apple', {}, {}, 0), RangeError);
  assert.throws(() => GroundItem.spawn('apple', {}, {}, 10001), RangeError);
  assert.throws(() => GroundItem.spawn('longSwordDuel', {}, {}, 2), /does not stack/);
  assert.throws(() => GroundItem.spawn('apple', {}, {}, 5), /does not stack/);
  assert.throws(() => GroundItem.spawn('apple', { x: Infinity }));
  assert.throws(() => GroundItem.spawn('apple', {}, {}, 1, -1), TypeError);
  assert.throws(() => GroundItem.spawn('longSwordDuel', {}, {}, 1, 0, { quality: 9 }), RangeError);
  assert.throws(
    () => GroundItem.spawn('apple', {}, {}, 1, 0, { carryable: true }),
    /Unknown carryable item/,
  );
  assert.throws(
    () => GroundItem.spawn('basket_full_wood', {}, {}, 2, 0, { carryable: true }),
    /single item/,
  );
  const basket = GroundItem.spawn('basket_full_wood', {}, {}, 1, 0, { carryable: true });
  assert.equal(basket.carryable, true);
  assert.throws(
    () => GroundItem.spawn('longSwordDuel', {}, {}, 1, 0, { quality: 2, health: 1, condition: 0 }),
    /disagree/,
  );
  // A worn class the game draws nothing for on the ground (a DLC badge that only pins
  // to a cap) is refused; one with a drop model is laid.
  assert.throws(() => GroundItem.spawn('Cap01_mBlacksmith01'), /no model to lie on the ground/);
  assert.ok(GroundItem.spawn('Spectacles01_m01') || true);
});

// Items.model: the mesh a class lies on the ground as, '' when the tables give none.
test('Items.model names the mesh a class lies on the ground as', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const { Items, Inventory } = host.makeAPI();
  assert.equal(
    Items.model('longSwordDuel'),
    'objects/manmade/weapons/swords_long/long_sword_duel.cgf',
  );
  assert.equal(Items.model('Cap01_mBlacksmith01'), '');
  assert.match(Items.model('apple'), /^objects\/.+\.cgf$/);
  assert.throws(() => Items.model('no such item'), TypeError);
  const sword = Inventory.classes().find((itemClass) => itemClass.name === 'longSwordDuel');
  assert.equal(sword.model, Items.model('longSwordDuel'));
});

test('a player in reach takes a stack up: its units join their inventory and it goes', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('groundItemPickup', (g, p) => kcds.core.Chat.sendToAll('pickup ' + g.amount + ' by ' + p.id));
    kcds.core.Events.on('groundItemDestroy', (g) => kcds.core.Chat.sendToAll('destroy ' + g.id));
    kcds.world.GroundItem.spawn('arrow_crude', { x: 1, y: 2, z: 3 }, {}, 20);
    kcds.world.GroundItem.spawn('longSwordDuel', { x: 100, y: 2, z: 3 }, {}, 1, 0xffffffff, { quality: 3 });
  `);
  await host.startAll();
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 2, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const p = host.players.get(7);
  const [arrows, sword] = host.makeAPI().GroundItem.all();
  assert.equal(host.groundItems.pickup(p, sword.id), false, 'out of reach');
  assert.equal(host.groundItems.pickup(p, arrows.id), true);
  assert.equal(host.groundItems.pickup(p, arrows.id), false, 'already taken');
  const rows = p.getInventory().items;
  assert.deepEqual(
    rows.map((row) => [row.name, row.amount]),
    [['arrow_crude', 20]],
  );
  assert.deepEqual(chat(), [`pickup 20 by 7`, `destroy ${arrows.id}`]);
  assert.equal(output.filter((e) => e.op === 'groundItemDestroy').length, 1);
  assert.deepEqual(
    output.filter((e) => e.op === 'itemNotice'),
    [{ op: 'itemNotice', id: 7, item: arrows.klass.id, amount: 20, gained: true }],
    "the taker's game shows its notice",
  );
});

test('units a player drops leave their inventory and lie at their feet, dropped by them', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 10, y: 20, z: 30 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const p = host.players.get(7);
  host.playerInventories.operate(p, (inventory) =>
    inventory.set({
      items: [
        { id: 'arrows', item: 'arrow_crude', amount: 30 },
        { id: 'blades', item: 'longSwordDuel', amount: 2, metadata: { quality: 3 } },
      ],
    }),
  );
  const arrows = host.groundItems.drop(p, 'arrows', 12);
  assert.equal(arrows.length, 1, 'one stack of a class that stacks');
  assert.equal(arrows[0].amount, 12);
  assert.equal(arrows[0].droppedById, 7);
  assert.equal(arrows[0].leftBy, p);
  assert.equal(arrows[0].position.z, 30.05);
  const blades = host.groundItems.drop(p, 'blades', 2);
  assert.equal(blades.length, 2, 'one stack per unit of a class that does not');
  assert.deepEqual(
    blades.map((stack) => [stack.amount, stack.quality]),
    [
      [1, 3],
      [1, 3],
    ],
  );
  assert.deepEqual(host.groundItems.drop(p, 'arrows', 99), [], 'more than the row holds');
  assert.deepEqual(
    p.getInventory().items.map((row) => [row.id, row.amount]),
    [['arrows', 18]],
  );
  const spawns = output.filter((e) => e.op === 'groundItemSpawn');
  assert.equal(spawns.length, 3);
  assert.ok(spawns.every((e) => e.leftBy === 7));
  host.playerInventories.flush();
});

test('a landed shot lies as a stack of one, and taking it up puts it back in the quiver', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 10, y: 20, z: 30 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const p = host.players.get(7);
  host.playerInventories.operate(p, (inventory) =>
    inventory.set({ items: [{ id: 'arrows', item: 'arrow_crude', amount: 29, equipped: 29 }] }),
  );
  const { lookupItemClass } = require('./inventory/catalog.cjs');
  const guid = lookupItemClass('arrow_crude').id;
  const arrow = host.groundItems.landed(p, {
    item: guid,
    position: { x: 12, y: 21, z: 30.2 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
  });
  assert.equal(arrow.amount, 1);
  assert.equal(arrow.leftBy, p);
  assert.equal(arrow.position.x, 12);
  // The shooter's game saw it stop there.
  assert.equal(arrow.resting, true);
  // An arrow stuck in a target stays there for every client, pinned.
  const stuck = host.groundItems.landed(p, {
    item: guid,
    position: { x: 14, y: 21, z: 31.5 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
    stuck: true,
  });
  const laid = output.filter((e) => e.op === 'groundItemSpawn');
  assert.equal(laid.find((e) => e.id === stuck.id).pinned, true);
  assert.equal(laid.find((e) => e.id === arrow.id).pinned, undefined);
  host.groundItems.destroy(stuck);
  assert.equal(host.groundItems.landed(p, { item: 'apple', position: { x: 0, y: 0, z: 0 } }), null);
  assert.equal(host.groundItems.pickup(p, arrow.id), true);
  assert.deepEqual(
    p.getInventory().items.map((row) => [row.id, row.amount, row.equipped]),
    [['arrows', 30, 30]],
  );
  // The oldest landed stack goes once too many lie.
  host.groundItems.landedStacks.length = 0;
  const first = host.groundItems.landed(p, { item: guid, position: { x: 12, y: 21, z: 30 } });
  for (let i = 0; i < 200; i += 1) {
    host.groundItems.landed(p, { item: guid, position: { x: 12, y: 21, z: 30 } });
  }
  assert.equal(host.groundItems.stacks.has(first.id), false);
  assert.equal(host.groundItems.landedStacks.length, 200);
  assert.ok(output.some((e) => e.op === 'groundItemDestroy' && e.id === first.id));
  host.playerInventories.flush();
});

test('the first plausible report of where a stack came to rest moves it there', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('groundItemResting', (g) => {
      kcds.core.Chat.sendToAll('rest ' + g.id + ' ' + g.position.z.toFixed(2) + ' ' + g.resting);
    });
    kcds.world.GroundItem.spawn('apple', { x: 10, y: 20, z: 33 }, {}, 1);
    kcds.world.GroundItem.spawn('apple', { x: 10, y: 20, z: 30 }, {}, 1, 7);
  `);
  await host.startAll();
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 10, y: 21, z: 30 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const p = host.players.get(7);
  const [apple, elsewhere] = host.makeAPI().GroundItem.all();
  const turn = { x: 0, y: 0, z: 0, w: 1.25 };
  const rest = (stack, position, rotation = turn) =>
    host.groundItems.rest(p, { stack: stack.id, position, rotation });
  // Too far from where it was laid, above it, or not a turn: not believed.
  assert.equal(rest(apple, { x: 60, y: 20, z: 30 }), false);
  assert.equal(rest(apple, { x: 10, y: 20, z: 34 }), false);
  assert.equal(rest(apple, { x: 10, y: 20, z: 30 }, { x: 0, y: 0, z: 0, w: 0 }), false);
  assert.equal(rest(apple, { x: 10, y: 20, z: Number.NaN }), false);
  assert.equal(rest(elsewhere, { x: 10, y: 20, z: 30 }), false, 'another world');
  assert.equal(apple.resting, false);
  assert.equal(rest(apple, { x: 11.5, y: 19, z: 30.25 }), true);
  assert.equal(apple.resting, true);
  assert.deepEqual({ ...apple.position }, { x: 11.5, y: 19, z: 30.25 });
  assert.equal(apple.rotation.w, 1, 'the turn is normalised');
  assert.deepEqual(
    output.filter((e) => e.op === 'groundItemRest'),
    [
      {
        op: 'groundItemRest',
        id: apple.id,
        position: { x: 11.5, y: 19, z: 30.25 },
        rotation: { x: 0, y: 0, z: 0, w: 1 },
      },
    ],
  );
  assert.deepEqual(chat(), [`rest ${apple.id} 30.25 true`]);
  // Another game's later report changes nothing.
  assert.equal(rest(apple, { x: 10, y: 20, z: 30 }), false);
  assert.equal(apple.position.x, 11.5);
  // A player in reach of where it rests takes it up.
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 11.5, y: 19, z: 30 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.equal(host.groundItems.pickup(p, apple.id), true);
  host.playerInventories.flush();
});

test('a script names a stack to find it again', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const { GroundItem } = host.makeAPI();
  const named = GroundItem.spawn('apple', { x: 1, y: 2, z: 3 }, {}, 1, undefined, {
    name: 'harvest_table',
  });
  const plain = GroundItem.spawn('apple', { x: 1, y: 2, z: 3 });
  assert.equal(named.name, 'harvest_table');
  assert.equal(plain.name, '');
  assert.equal(
    GroundItem.all().find((stack) => stack.name === 'harvest_table'),
    named,
  );
  assert.throws(
    () => GroundItem.spawn('apple', { x: 0, y: 0, z: 0 }, {}, 1, undefined, { name: 7 }),
    TypeError,
  );
});

// a stack shown to one player only, laid by their game alone and theirs to take up.
test('GroundItem.showOnlyTo shows a stack to one player, who alone takes it up', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  for (const id of [7, 8]) {
    host.updatePlayer({
      id,
      ready: true,
      position: { x: 2, y: 2, z: 3 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  }
  const api = host.makeAPI();
  const viewer = host.players.get(7);
  const other = host.players.get(8);
  const preview = api.GroundItem.spawn('arrow_crude', { x: 1, y: 2, z: 3 }, {}, 5, 0xffffffff, {
    visibleTo: viewer,
  });
  assert.equal(preview.visibleTo, viewer);
  assert.deepEqual(output.filter((e) => e.op === 'entityVisibleTo').at(-1), {
    op: 'entityVisibleTo',
    id: preview.id,
    playerId: 7,
  });
  assert.equal(host.groundItems.pickup(other, preview.id), false, 'not shown to them');
  preview.showOnlyTo(null);
  assert.equal(preview.visibleTo, null);
  assert.equal(output.filter((e) => e.op === 'entityVisibleTo').at(-1).playerId, 0);
  preview.showOnlyTo(null);
  assert.equal(output.filter((e) => e.op === 'entityVisibleTo').length, 2, 'no change, no op');
  assert.throws(() => preview.showOnlyTo({ id: 99 }), TypeError);
  assert.equal(host.groundItems.pickup(other, preview.id), true);
});

// Items.info: a class's category and, for a weapon, its class and attack modifiers.
test('Items.info names the category, weapon class and attack modifiers', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const { Items } = host.makeAPI();
  const axe = Items.info('axeBattle01');
  assert.equal(axe.category, 'meleeWeapon');
  assert.equal(axe.weaponClass, 'axe');
  assert.ok(axe.attackModSlash > 0);
  assert.equal(Items.info(axe.id).name, 'axeBattle01');
  // The game's tables class a broken axe as a mace that only smashes.
  const broken = Items.info('axeBattle01_broken');
  assert.equal(broken.attackModSlash, 0);
  assert.ok(broken.attackModSmash > 0);
  assert.equal(Items.info('longSwordDuel').weaponClass, 'longsword');
  const apple = Items.info('apple');
  assert.equal(apple.weaponClass, null);
  assert.equal(apple.attackModSlash, 0);
  assert.equal(Items.info('no such item'), null);
});

// a landed shot raises playerMissileHit with where it lies and what it struck.
test('a landed shot raises playerMissileHit with its stack and the hit its game described', async () => {
  const { host } = hostWith('');
  await host.startAll();
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 10, y: 20, z: 30 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const p = host.players.get(7);
  const seen = [];
  host.makeAPI().Events.on('playerMissileHit', (player, hit) => seen.push([player, hit]));
  const { lookupItemClass } = require('./inventory/catalog.cjs');
  const guid = lookupItemClass('arrow_crude').id;
  host.missileLanded(p, {
    item: guid,
    position: { x: 14, y: 21, z: 31.5 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
    stuck: true,
    hit: {
      position: { x: 14, y: 21, z: 31.5 },
      normal: { x: 0, y: -1, z: 0 },
      distance: 0.4,
      terrain: false,
      entity: 0,
      surface: 'mat_wood',
      kind: 'entity',
      model: '',
      guid: '00ab00cd00ef0012',
      name: 'archery_target_1',
      cls: 'GeomEntity',
    },
  });
  host.missileLanded(p, { item: guid, position: { x: 1, y: 2, z: 3 }, rotation: { w: 1 } });
  assert.equal(seen.length, 2);
  const [[who, struck], [, plain]] = seen;
  assert.equal(who, p);
  assert.equal(struck.stuck, true);
  assert.equal(struck.surface, 'mat_wood');
  assert.equal(struck.entityName, 'archery_target_1');
  assert.equal(struck.entityClass, 'GeomEntity');
  assert.ok(struck.stack);
  assert.equal(plain.entityName, null);
  assert.equal(plain.surface, '');
});
