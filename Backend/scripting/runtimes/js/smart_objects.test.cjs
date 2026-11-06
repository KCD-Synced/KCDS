'use strict';

// The level's stations as SmartObject lists them, and taking them out of a world.
const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const GLOBAL_WORLD = 0xffffffff;
// Troskovice's dice table, its pharmacist's alchemy table and a drying rack on trosecko.
const TABLE = '2bbc32a3fc2f0ce5';
const ALCHEMY = '2229e312da70017e';
const RACK = '079a07d7437d0255';

async function started(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const api = f.host.makeAPI();
  const ops = (...names) => f.sent.filter((m) => names.includes(m.op));
  return { ...f, api, ops };
}

const ids = (rows) => rows.map((row) => row.id);

test("stations lists the level's dice tables, crafting stations, racks and smokehouses", async (t) => {
  const { api } = await started(t);
  const stations = api.SmartObject.stations();
  const kinds = new Set(stations.map((station) => station.kind));
  assert.deepEqual([...kinds].sort(), ['alchemy', 'dice', 'drying', 'smithing', 'smoking']);
  const table = stations.find((station) => station.id === TABLE);
  assert.equal(table.kind, 'dice');
  assert.equal(table.placed, false);
  assert.equal(table.smartObject, null);
  assert.ok(table.position instanceof api.Vector3);
  assert.ok(ids(stations).includes(ALCHEMY));
  assert.ok(ids(stations).includes(RACK));
  assert.ok(stations.every((station) => /^[0-9a-f]{16}$/.test(station.id)));
  // The level's own rows say so.
  assert.equal(api.Dice.tables().find((row) => row.id === TABLE).placed, false);
  assert.equal(api.Crafting.stations('alchemy')[0].placed, false);
});

test('remove hides a station through a level edit in one world; restore puts it back', async (t) => {
  const { api, ops } = await started(t);
  api.SmartObject.remove(TABLE, 5);
  const [edit] = ops('levelEditApply');
  assert.equal(edit.kind, 'entity');
  assert.equal(edit.guid, TABLE);
  assert.equal(edit.hidden, true);
  assert.equal(edit.dimension, 5);
  assert.ok(!ids(api.SmartObject.stations(5)).includes(TABLE));
  assert.ok(ids(api.SmartObject.stations(6)).includes(TABLE));
  assert.ok(ids(api.Dice.tables()).includes(TABLE), 'only a removal from every world unlists it');
  assert.deepEqual(
    api.SmartObject.removed().map((row) => [row.id, row.kind, row.dimension]),
    [[TABLE, 'dice', 5]],
  );
  assert.equal(api.SmartObject.restore(TABLE, 6), false);
  assert.equal(api.SmartObject.restore(TABLE, 5), true);
  assert.equal(ops('levelEditRemove').length, 1);
  assert.deepEqual(api.SmartObject.removed(), []);
  assert.equal(api.SmartObject.restore(TABLE, 5), false);
});

test('out of the global world is out of every world, listed nowhere', async (t) => {
  const { api } = await started(t);
  api.SmartObject.remove('trosecko/2229e312-da70-017e');
  api.SmartObject.remove(TABLE);
  assert.ok(!ids(api.SmartObject.stations(7)).includes(ALCHEMY));
  assert.ok(!ids(api.Crafting.stations('alchemy')).includes('trosecko/2229e312-da70-017e'));
  assert.ok(!ids(api.Dice.tables()).includes(TABLE));
  assert.deepEqual(
    api.SmartObject.removed(GLOBAL_WORLD)
      .map((row) => row.id)
      .sort(),
    [ALCHEMY, TABLE].sort(),
  );
});

test('a level edit of a World Builder export counts as a removal too', async (t) => {
  const { api } = await started(t);
  api.LevelEdit.apply({ kind: 'entity', guid: RACK, at: { x: 0, y: 0, z: 0 }, hidden: true }, 3);
  assert.deepEqual(
    api.SmartObject.removed().map((row) => [row.id, row.kind, row.dimension]),
    [[RACK, 'drying', 3]],
  );
});

test('a removed station takes no new session', async (t) => {
  const { host, api, report, ops } = await started(t);
  const table = api.Dice.tables().find((row) => row.id === TABLE);
  report(7, { position: table.position, dimension: 5 });
  api.SmartObject.remove(TABLE, 5);
  assert.equal(host.dice.sit(7), -1);
  api.SmartObject.restore(TABLE, 5);
  assert.equal(host.dice.sit(7), 0);

  const rack = api.SmartObject.stations().find((row) => row.id === RACK);
  api.SmartObject.remove(RACK);
  host.preparing.ask(host.players.get(7), { request: 4, reason: 'drying', station: rack.position });
  assert.deepEqual(ops('craftVerdict').at(-1), {
    op: 'craftVerdict',
    id: 7,
    request: 4,
    allowed: false,
  });
});

test('remove throws for anything but one of the level stations', async (t) => {
  const { api } = await started(t);
  assert.throws(() => api.SmartObject.remove('0000000000000000'), RangeError);
  assert.throws(() => api.SmartObject.remove(42), RangeError);
  // A station of another level.
  assert.throws(() => api.SmartObject.remove('0679658da25d0a80'), RangeError);
  assert.throws(() => api.SmartObject.remove(TABLE, -1), TypeError);
  assert.throws(() => new api.SmartObject(), TypeError);
});

test('spawn places a prefab for its world, a player who comes later too, and destroy takes it down', async (t) => {
  const { api, ops, sent, report } = await started(t);
  const blueprints = api.SmartObject.blueprints();
  const names = blueprints.map((row) => row.name);
  assert.deepEqual(names, [...names].sort());
  assert.deepEqual(
    { ...blueprints.find((row) => row.name === 'Bench/bench_2places_low') },
    { name: 'Bench/bench_2places_low', kind: 'seat', seats: 2 },
  );
  for (const kind of ['seat', 'bed', 'dice', 'alchemy', 'smithing', 'drying', 'smoking']) {
    assert.ok(
      blueprints.some((row) => row.kind === kind),
      kind,
    );
  }
  const bench = api.SmartObject.spawn({
    blueprint: 'bench/BENCH_2places_low',
    position: [10, 20, 30],
    rotation: { x: 0, y: 0, z: 2, w: 2 },
    name: 'guild bench',
  });
  assert.equal(bench.blueprint, 'Bench/bench_2places_low');
  assert.equal(bench.kind, 'seat');
  assert.equal(bench.name, 'guild bench');
  assert.ok(bench instanceof api.Entity);
  assert.deepEqual(bench.position, new api.Vector3(10, 20, 30));
  assert.equal(api.SmartObject.getById(bench.id), bench);
  // The keys every client gives the bench's two seats (kit: 6b63000010120001 and ...02 for 4114),
  // in the level files' order (low 32 bits, bits 32 to 47, bits 48 to 63) as the stations are.
  const key = (id, place) => {
    const text = ((0x6b63n << 48n) | (BigInt(id) << 16n) | BigInt(place)).toString(16);
    return text.slice(8, 16) + text.slice(4, 8) + text.slice(0, 4);
  };
  assert.deepEqual(bench.seats, [key(bench.id, 1), key(bench.id, 2)]);
  assert.equal(bench.stationKey, '');
  const table = api.SmartObject.spawn({ blueprint: 'Table/table_dice', position: [0, 0, 0] });
  assert.equal(table.kind, 'dice');
  assert.equal(table.stationKey, key(table.id, 27));
  assert.equal(table.seats.length, 2);
  table.destroy();
  assert.equal(new api.SmartObject(bench.id), bench);
  assert.deepEqual(api.SmartObject.all(), [bench]);
  assert.deepEqual(api.SmartObject.all(4), []);
  const placed = ops('smartObjectSpawn').find((m) => m.id === bench.id);
  assert.equal(placed.id, bench.id);
  assert.equal(placed.template, 'f6a5d971-3544-415b-a75a-33bd9dcac9e4');
  assert.deepEqual(placed.position, { x: 10, y: 20, z: 30 });
  assert.equal(placed.rotation.z.toFixed(4), '0.7071');
  assert.equal(placed.dimension, GLOBAL_WORLD);
  assert.deepEqual(bench.toJSON(), {
    blueprint: 'Bench/bench_2places_low',
    position: { x: 10, y: 20, z: 30 },
    rotation: placed.rotation,
    name: 'guild bench',
  });
  // A player whose world became ready builds it again.
  sent.length = 0;
  const player = report(7);
  assert.deepEqual(
    ops('smartObjectSpawn').map((m) => [m.id, m.to]),
    [[bench.id, player.id]],
  );
  sent.length = 0;
  bench.destroy();
  assert.deepEqual(ops('smartObjectDestroy'), [{ op: 'smartObjectDestroy', id: bench.id }]);
  assert.equal(bench.toJSON(), null);
  assert.deepEqual(api.SmartObject.all(), []);
  assert.equal(api.SmartObject.getById(bench.id), null);
  assert.throws(() => new api.SmartObject(bench.id), TypeError);
});

test('spawn refuses an unknown blueprint and a transform that is not finite', async (t) => {
  const { api } = await started(t);
  const place = (change) => () =>
    api.SmartObject.spawn({ blueprint: 'Bed/bed_low', position: [1, 2, 3], ...change });
  assert.throws(place({ blueprint: 'Bed/no_such_bed' }), /unknown blueprint/);
  assert.throws(place({ position: [1, 2] }), /position/);
  assert.throws(place({ position: [1, 2, 99999] }), /position/);
  assert.throws(place({ rotation: [0, 0, 0, 0] }), /rotation/);
  assert.throws(place({ name: 'x'.repeat(65) }), /name/);
  assert.throws(
    () => api.SmartObject.spawn({ blueprint: 'Bed/bed_low', position: [1, 2, 3] }, -1),
    /dimension/,
  );
  const bed = api.SmartObject.spawn(
    { blueprint: 'Bed/bed_low', position: { x: 1, y: 2, z: 3 } },
    5,
  );
  assert.equal(bed.kind, 'bed');
  assert.equal(bed.name, '');
  assert.deepEqual(api.SmartObject.all(5), [bed]);
  assert.equal(bed.toJSON().name, undefined);
});

test('a placed station joins Dice.tables, Crafting.stations and SmartObject.stations', async (t) => {
  const { api, host, report } = await started(t);
  // A dice table turned a quarter: its seats' midpoint (-0.3452, 0, 0) in the prefab turns too.
  const table = api.SmartObject.spawn({
    blueprint: 'Table/table_dice',
    position: [100, 200, 30],
    rotation: [0, 0, Math.SQRT1_2, Math.SQRT1_2],
  });
  const row = api.Dice.tables().find((entry) => entry.id === table.stationKey);
  assert.ok(row, 'listed by Dice.tables');
  assert.equal(row.placed, true);
  assert.equal(row.seats.length, 2);
  assert.equal(row.seats[0], table.seats[0]);
  assert.ok(Math.abs(row.position.x - 100) < 1e-3);
  assert.ok(Math.abs(row.position.y - 199.6548) < 1e-3);
  // A player standing at it finds it, in its world only.
  const player = report(7, { position: { x: 100, y: 199.5, z: 30 } });
  assert.equal(host.dice.tableNear(player.position, 0).id, table.stationKey);
  const elsewhere = api.SmartObject.spawn(
    { blueprint: 'Table/table_dice', position: [300, 200, 30] },
    5,
  );
  assert.equal(host.dice.tableNear({ x: 300, y: 200, z: 30 }, 4), null);
  assert.equal(host.dice.tableNear({ x: 299.7, y: 200, z: 30 }, 5).id, elsewhere.stationKey);
  // An alchemy table is a crafting station, and every kind is listed by SmartObject.stations.
  const alchemy = api.SmartObject.spawn({
    blueprint: 'profession/apothecary/alchemyTable',
    position: [50, 60, 70],
  });
  const station = api.Crafting.stations('alchemy').find((entry) => entry.id === alchemy.stationKey);
  assert.deepEqual([station.placed, station.position], [true, { x: 50, y: 60, z: 70 }]);
  const listed = api.SmartObject.stations();
  assert.equal(listed.find((entry) => entry.id === alchemy.stationKey).smartObject, alchemy);
  assert.equal(listed.find((entry) => entry.id === table.stationKey).kind, 'dice');
  assert.ok(!api.SmartObject.stations(4).some((entry) => entry.id === elsewhere.stationKey));
  // Taken down, it is listed nowhere.
  table.destroy();
  alchemy.destroy();
  assert.ok(!api.Dice.tables().some((entry) => entry.id === table.stationKey));
  assert.ok(!api.Crafting.stations().some((entry) => entry.id === alchemy.stationKey));
});

test('a dice table lists its chairs, free at first', async (t) => {
  const { api } = await started(t);
  const table = api.Dice.tables().find((row) => row.id === TABLE);
  assert.equal(table.chairs.length, 2);
  for (const chair of table.chairs) {
    assert.equal(chair.player, null);
    assert.ok(
      Math.hypot(chair.position.x - table.position.x, chair.position.y - table.position.y) < 3,
    );
  }
});
