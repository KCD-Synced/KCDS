'use strict';

// a horse's tack, its presets and items from the game's tables, and the breeds.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { ResourceHost } = require('../host.cjs');
const { gearOf, gearSets, gearList, presetGear, piecesOf } = require('./gear.cjs');

async function hostWith(config) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-horse-gear-'));
  if (config !== undefined) {
    fs.writeFileSync(path.join(root, 'horse-config.json'), JSON.stringify(config));
  }
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  await host.startAll();
  const sent = (op) => output.filter((e) => e.op === op);
  return { host, output, sent, Horse: host.api.Horse };
}

test('the presets and items come from the game tables, by slot', () => {
  const presets = gearSets();
  assert.ok(presets.includes('horse_common01'));
  assert.ok(presets.includes('horse_noble05'));
  assert.ok(!presets.includes('generic_nakedHorse'), 'an empty preset is no tack');
  assert.deepEqual([...presets].sort(), presets);
  assert.deepEqual(presetGear('horse_noble05'), {
    saddle: 'NobleSaddle01_m04',
    head: 'NobleBridle01_m03',
    torso: 'NobleCaparison04_m08',
    shoe: 'horseshoeNoble',
  });
  const saddles = gearList('saddle');
  assert.ok(saddles.includes('BasicSaddle01_m01'));
  assert.ok(!saddles.includes('BasicBridle01_m01'));
  const all = gearList();
  assert.equal(all[0], saddles[0], 'saddles first');
  assert.ok(all.includes('horseshoeNoble'));
  assert.throws(() => gearList('tail'), TypeError);
});

test('gear by slot is checked: horse gear only, in its own slot, a caparison on a saddle', () => {
  assert.deepEqual(gearOf({ head: 'BasicBridle01_m01' }), {
    saddle: null,
    head: 'BasicBridle01_m01',
    torso: null,
    shoe: null,
  });
  assert.throws(() => gearOf({ head: 'BasicSaddle01_m01' }), /saddle gear, not head/);
  assert.throws(() => gearOf({ saddle: 'longSwordDuel' }), /no horse gear/);
  assert.throws(() => gearOf({ tail: 'x' }), /unknown slot/);
  assert.throws(() => gearOf({ torso: 'NobleCaparison04_m08' }), RangeError);
  assert.throws(() => gearOf('horse_nothing'), /no horse gear preset/);
  assert.throws(() => gearOf(5), TypeError);
  assert.deepEqual(gearOf({}), { saddle: null, head: null, torso: null, shoe: null });
  assert.equal(piecesOf(gearOf('horse_common01')).length, 3);
});

test('a horse wears the game generic tack unless told, and every change goes down', async () => {
  const { host, sent, Horse } = await hostWith();
  const changed = [];
  host.on('horseGearChanged', (horse, player, gear) => changed.push([horse.id, player, gear]));
  const plain = Horse.spawn({ x: 1, y: 2, z: 3 });
  assert.notEqual(plain.gear.saddle, null);
  assert.equal(sent('horseGear')[0].id, plain.id);
  assert.equal(sent('horseGear')[0].pieces.length, 3);
  const bare = Horse.spawn({}, {}, '', 'Bare', {});
  assert.deepEqual(bare.gear, { saddle: null, head: null, torso: null, shoe: null });
  assert.deepEqual(sent('horseGear').at(-1), { op: 'horseGear', id: bare.id, pieces: [] });
  assert.throws(() => Horse.spawn({}, {}, '', '', { torso: 'NobleCaparison04_m08' }), RangeError);

  assert.equal(bare.setTack('horse_noble05'), true);
  assert.equal(bare.gear.torso, 'NobleCaparison04_m08');
  assert.equal(sent('horseGear').at(-1).pieces.length, 4);
  assert.deepEqual(changed.at(-1).slice(0, 2), [bare.id, null]);
  assert.equal(bare.fitGear('BasicBridle01_m01'), true);
  assert.equal(bare.gear.head, 'BasicBridle01_m01');
  assert.equal(bare.gear.saddle, 'NobleSaddle01_m04', 'the rest stays');
  assert.equal(bare.fitGear('longSwordDuel'), false);
  // The caparison comes off with its saddle.
  assert.equal(bare.takeOffGear('saddle'), true);
  assert.equal(bare.gear.torso, null);
  assert.equal(bare.takeOffGear('saddle'), false);
  assert.equal(bare.fitGear('NobleCaparison04_m08'), false, 'no saddle under it');
  assert.throws(() => bare.takeOffGear('tail'), TypeError);
  assert.equal(bare.gearFixed, false);
  bare.gearFixed = true;
  assert.equal(bare.gearFixed, true);
  assert.equal(bare.setTack({}), true, 'a script is not held by gearFixed');
  bare.destroy();
  assert.equal(bare.setTack('horse_common01'), false);
});

test('the breeds are the server config ones, and a breed names a soul too', async () => {
  const { Horse } = await hostWith({ breeds: { courser: { speed: 50 } } });
  assert.deepEqual(Horse.breeds(), ['courser']);
  const horse = Horse.spawn({}, {}, 'courser');
  assert.equal(horse.breed, 'courser');
  assert.equal(horse.speed, 50);
  assert.equal(horse.soul, '');
});

test('player.mount orders the player game to get on a living horse nobody else rides', async () => {
  const { host, sent, Horse } = await hostWith();
  const player = host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const horse = Horse.spawn({ x: 2, y: 0, z: 0 });
  assert.equal(player.mount(horse), true);
  assert.deepEqual(sent('playerMount').at(-1), {
    op: 'playerMount',
    id: 7,
    horse: horse.id,
    instant: false,
  });
  assert.equal(player.mount(horse, { instant: true }), true);
  assert.equal(sent('playerMount').at(-1).instant, true);
  assert.throws(() => player.mount({ id: horse.id }), TypeError);
  assert.throws(() => player.mount(horse, { instant: 'yes' }), TypeError);
  horse.riderId = 99;
  assert.equal(player.mount(horse), false, 'somebody else rides it');
  horse.riderId = 0;
  horse.kill();
  assert.equal(player.mount(horse), false, 'a dead horse');
});

test('a rider raises horseMount and horseDismount; horseMounting may refuse the ride', async () => {
  const { host, sent, Horse } = await hostWith();
  const join = (id) =>
    host.updatePlayer({
      id,
      nickname: `P${id}`,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  const rider = join(7);
  const thief = join(8);
  const horse = Horse.spawn({ x: 2, y: 0, z: 0 });
  const seen = [];
  host.on('horseMount', (h, p) => seen.push(['mount', p?.id]));
  host.on('horseDismount', (h, p) => seen.push(['dismount', p?.id]));
  host.on('horseMounting', (h, p) => p.id !== 8);
  const report = (riderId) =>
    host.updateHorse({
      id: horse.id,
      position: { x: 2, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      riderId,
    });
  report(7);
  assert.equal(rider.dismount(), true);
  assert.deepEqual(sent('playerDismount').at(-1), { op: 'playerDismount', id: 7 });
  report(0);
  // The thief is refused: told to get off, and neither event follows.
  report(8);
  assert.deepEqual(sent('playerDismount').at(-1), { op: 'playerDismount', id: 8 });
  report(0);
  assert.deepEqual(seen, [
    ['mount', 7],
    ['dismount', 7],
  ]);
  assert.equal(thief.dismount(), false, 'riding nothing');
});

test('Horse.unseatRider takes the rider off; World.replicasAround finds both', async () => {
  const { host, sent, Horse } = await hostWith();
  const rider = host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const horse = Horse.spawn({ x: 2, y: 0, z: 0 });
  assert.equal(horse.unseatRider(), null, 'an empty saddle');
  assert.equal(horse.buckRider(), false, 'an empty saddle');
  assert.equal(horse.dragRider(rider), false, 'an empty saddle');
  assert.throws(() => horse.dragRider(null), TypeError);
  host.updateHorse({
    id: horse.id,
    position: { x: 2, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    riderId: 7,
  });
  assert.equal(horse.dragRider(rider), false, 'the rider themselves');
  const attacker = host.updatePlayer({
    id: 8,
    nickname: 'P8',
    ready: true,
    position: { x: 3, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.equal(horse.dragRider(attacker), true);
  assert.deepEqual(sent('horsePullDown').at(-1), { op: 'horsePullDown', attacker: 8, rider: 7 });
  host.updatePlayer({
    id: 8,
    nickname: 'P8',
    ready: true,
    position: { x: 30, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  assert.equal(horse.dragRider(attacker), false, 'too far');
  assert.equal(horse.buckRider(), true);
  assert.deepEqual(sent('horseRear').at(-1), { op: 'horseRear', id: 7 });
  assert.equal(horse.unseatRider(), rider);
  assert.deepEqual(sent('playerDismount').at(-1), { op: 'playerDismount', id: 7 });
  const { World } = host.api ?? host.makeAPI();
  const near = World.replicasAround({ x: 0, y: 0, z: 0 }, 5);
  // The horse's saddlebags are a stash of their own, where the horse is.
  assert.deepEqual(near.slice(0, 2), [rider, horse]);
  assert.deepEqual(World.replicasAround({ x: 0, y: 0, z: 0 }, 1), [rider]);
});

test('an owner changes the tack in the game screen: items move, gearFixed and horseGearChanging refuse', async () => {
  const { host, sent, Horse } = await hostWith();
  const { lookupItemClass } = require('../inventory/catalog.cjs');
  const join = (id) => {
    host.updatePlayer({
      id,
      nickname: `P${id}`,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
    return host.players.get(id);
  };
  const owner = join(7);
  const other = join(8);
  host.playerInventories.operate(owner, (inventory) =>
    inventory.set({ items: [{ id: 's', item: 'NobleSaddle01_m04', amount: 1 }] }),
  );
  const names = (player) =>
    host.playerInventories
      .entryOf(player)
      .inventory.snapshot()
      .items.map((row) => row.name);
  const horse = Horse.spawn({ x: 2, y: 0, z: 0 }, {}, '', '', 'horse_common01');
  const before = horse.gear;
  horse.giveTo(owner);
  const changing = [];
  const changed = [];
  let answer = true;
  host.on('horseGearChanging', (h, p, gear) => {
    changing.push([h.id, p.id, gear.saddle]);
    return answer;
  });
  host.on('horseGearChanged', (h, p, gear) => changed.push([h.id, p?.id ?? null, gear.saddle]));

  // Somebody else's horse, a row that is no tack, a row that is gone: nothing happens.
  assert.equal(host.playerHorseGear(other, horse.id, { row: 's' }), false);
  assert.equal(host.playerHorseGear(owner, horse.id, { row: 'missing' }), false);
  assert.equal(changing.length, 0);

  // A handler refuses: the saddle stays in the inventory and the horse keeps its tack.
  answer = false;
  const sentBefore = sent('horseGear').length;
  assert.equal(host.playerHorseGear(owner, horse.id, { row: 's' }), false);
  assert.deepEqual(changing.at(-1), [horse.id, 7, 'NobleSaddle01_m04']);
  assert.deepEqual(horse.gear, before);
  assert.deepEqual(names(owner), ['NobleSaddle01_m04']);
  assert.equal(sent('horseGear').length, sentBefore);

  // Accepted: the saddle goes on, the old one into the saddlebags, as in the game's screen.
  const bagNames = () => horse.storage.stock.snapshot().items.map((row) => row.name);
  answer = true;
  assert.equal(host.playerHorseGear(owner, horse.id, { row: 's' }), true);
  assert.equal(horse.gear.saddle, 'NobleSaddle01_m04');
  assert.equal(horse.gear.head, before.head, 'the rest stays');
  assert.deepEqual(names(owner), []);
  assert.deepEqual(bagNames(), [before.saddle]);
  assert.deepEqual(changed.at(-1), [horse.id, 7, 'NobleSaddle01_m04']);

  // Taken off by class: into the saddlebags.
  const saddleGuid = lookupItemClass('NobleSaddle01_m04').id;
  assert.equal(host.playerHorseGear(owner, horse.id, { item: saddleGuid }), true);
  assert.equal(horse.gear.saddle, null);
  assert.deepEqual(bagNames().sort(), [before.saddle, 'NobleSaddle01_m04'].sort());
  assert.equal(host.playerHorseGear(owner, horse.id, { item: saddleGuid }), false, 'not worn');

  // Put on again from the saddlebags.
  const bagRow = horse.storage.stock
    .snapshot()
    .items.find((row) => row.name === 'NobleSaddle01_m04').id;
  assert.equal(host.playerHorseGear(owner, horse.id, { row: bagRow, fromBags: true }), true);
  assert.equal(horse.gear.saddle, 'NobleSaddle01_m04');
  assert.deepEqual(bagNames(), [before.saddle]);

  // gearFixed refuses before horseGearChanging is asked.
  horse.gearFixed = true;
  const asked = changing.length;
  const headGuid = lookupItemClass(before.head).id;
  assert.equal(host.playerHorseGear(owner, horse.id, { item: headGuid }), false);
  assert.equal(changing.length, asked);
  assert.equal(horse.gear.head, before.head);
});

test('a horseGearChanging handler that changes the gear itself makes the player change stale', async () => {
  const { host, Horse } = await hostWith();
  host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const owner = host.players.get(7);
  host.playerInventories.operate(owner, (inventory) =>
    inventory.set({ items: [{ id: 's', item: 'NobleSaddle01_m04', amount: 1 }] }),
  );
  const horse = Horse.spawn({ x: 2, y: 0, z: 0 }, {}, '', '', 'horse_common01');
  horse.giveTo(owner);
  host.on('horseGearChanging', (h) => {
    h.setTack({});
  });
  assert.equal(host.playerHorseGear(owner, horse.id, { row: 's' }), false);
  assert.deepEqual(horse.gear, { saddle: null, head: null, torso: null, shoe: null });
  const left = host.playerInventories.entryOf(owner).inventory.snapshot().items;
  assert.deepEqual(
    left.map((row) => row.name),
    ['NobleSaddle01_m04'],
    'the saddle stays with the player',
  );
});
