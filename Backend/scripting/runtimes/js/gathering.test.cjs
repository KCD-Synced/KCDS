'use strict';

// Herb gathering and regrowth: a pick asked of the server, what it gives, who is told
// and when the plants grow back.
const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { KINDS } = require('./gathering/gathering.cjs');

// St John's wort (Trezalka), 156 game hours in the game: 31.2 by default here.
const WORT = KINDS.get(17);
const AT = { x: 2350, y: 1980, z: 98.7 };

async function world(t) {
  const fixture = serverFixture(t);
  await fixture.host.startAll();
  const api = fixture.host.makeAPI();
  api.World.setTimeScale(0);
  const picker = fixture.report(7, { position: { x: AT.x + 1, y: AT.y, z: AT.z } });
  const other = fixture.report(8, { position: { x: AT.x + 50, y: AT.y, z: AT.z } });
  fixture.host.playerInventories.flush();
  return { ...fixture, api, picker, other };
}

function pick(overrides = {}) {
  return {
    request: 1,
    item: WORT.item,
    amount: 2,
    kind: WORT.id,
    position: [AT.x, AT.y, AT.z],
    radius: 1.5,
    plants: 2,
    ...overrides,
  };
}

function ops(sent, op) {
  return sent.filter((message) => message.op === op);
}

function herbs(api, player) {
  return api.Inventory.get(player)
    .items.filter((item) => item.item === WORT.item)
    .reduce((sum, item) => sum + item.amount, 0);
}

test('a pick gives the herb and XP, tells the world, and raises its events in order', async (t) => {
  const { host, api, sent, picker } = await world(t);
  const events = [];
  const realEmit = host.emit.bind(host);
  host.emit = (name, ...args) => {
    if (name.startsWith('gathering') || name === 'playerInventoryChanged') {
      events.push([name, ...args]);
    }
    return realEmit(name, ...args);
  };
  sent.length = 0;
  assert.equal(host.gathering.report(picker, pick()), true);
  assert.deepEqual(
    ops(sent, 'gatherVerdict').map(({ id, request, granted }) => [id, request, granted]),
    [[7, 1, true]],
  );
  assert.equal(herbs(api, picker), 2);
  // Every game in the world hides the same plants (the picker's already did, and learns the id).
  const plants = ops(sent, 'gatherPlants');
  const entry = [[1, WORT.id, AT.x, AT.y, AT.z, 1.5]];
  assert.deepEqual(
    plants.map(({ id, full, picked, grown }) => [id, full, picked, grown]),
    [
      [7, false, entry, []],
      [8, false, entry, []],
    ],
  );
  assert.deepEqual(
    events.map(([name]) => name),
    ['gatheringHarvest', 'playerInventoryChanged', 'gatheringHarvested'],
  );
  const [, , proposal] = events[0];
  assert.deepEqual(
    [proposal.item, proposal.amount, proposal.kind, proposal.position, proposal.dimension],
    [WORT.item, 2, WORT.id, [AT.x, AT.y, AT.z], picker.dimension],
  );
  assert.ok(Object.isFrozen(proposal));
  const [, , harvested] = events[2];
  assert.equal(harvested.items.length, 1);
  assert.equal(harvested.items[0].amount, 2);
  // 5 Survival XP for the pick.
  assert.ok(sent.some((message) => message.op === 'progressionOrder' && message.id === 7));
});

test('a pick is refused out of reach, too fast, already picked, by a handler or for another herb', async (t) => {
  const { host, api, sent, picker, other } = await world(t);
  const verdicts = () => ops(sent, 'gatherVerdict').map((verdict) => verdict.granted);
  // Not the kind's own herb.
  assert.equal(host.gathering.report(picker, pick({ item: KINDS.get(2).item })), false);
  // Too far from the player.
  assert.equal(host.gathering.report(picker, pick({ position: [AT.x + 20, AT.y, AT.z] })), false);
  assert.equal(host.gathering.report(picker, pick()), true);
  // Too fast, then the same plants again.
  assert.equal(host.gathering.report(picker, pick({ request: 2 })), false);
  host.gathering.lastPick.clear();
  assert.equal(
    host.gathering.report(picker, pick({ request: 3, position: [AT.x + 0.5, AT.y, AT.z] })),
    false,
  );
  // A handler refuses: nothing is given.
  host.gathering.lastPick.clear();
  const realEmit = host.emit.bind(host);
  host.emit = (name, ...args) => (name === 'gatheringHarvest' ? false : realEmit(name, ...args));
  const near = { x: AT.x + 49, y: AT.y, z: AT.z };
  assert.equal(
    host.gathering.report(other, pick({ request: 4, position: [near.x, near.y, near.z] })),
    false,
  );
  host.emit = realEmit;
  assert.deepEqual(verdicts(), [false, false, true, false, false, false]);
  assert.equal(herbs(api, picker), 2);
  assert.equal(herbs(api, other), 0);
});

test('picks grow back on the world clock, by policy, and at a script call', async (t) => {
  const { host, api, sent, picker } = await world(t);
  assert.equal(api.Gathering.regrowTime, null);
  host.gathering.report(picker, pick());
  const due = [...host.gathering.picksOf(picker.dimension).values()][0].due;
  // The default: the game's 156 hours / 5.
  assert.ok(Math.abs(due - host.worldState.clock.seconds() - 31.2 * 3600) < 1);
  sent.length = 0;
  // Wind the clock past it: the pick grows back on every game in the world.
  host.worldState.clock.windTo(due + 1);
  host.gathering.tick();
  assert.deepEqual(
    ops(sent, 'gatherPlants').map(({ id, grown }) => [id, grown]),
    [
      [7, [1]],
      [8, [1]],
    ],
  );
  // A fixed time, and timed regrowth off.
  assert.equal(api.Gathering.setRegrowth(2, 2), true);
  assert.deepEqual(api.Gathering.regrowTime, { minHours: 2, maxHours: 2 });
  assert.equal(api.Gathering.setRegrowth(3, 2), false);
  assert.equal(api.Gathering.setRegrowth(0, 87601), false);
  host.gathering.lastPick.clear();
  host.gathering.report(picker, pick({ request: 5 }));
  const fixed = [...host.gathering.picksOf(picker.dimension).values()][0];
  assert.ok(Math.abs(fixed.due - host.worldState.clock.seconds() - 7200) < 1);
  assert.equal(api.Gathering.setRegrowth(0, 0), true);
  assert.equal(fixed.due, null);
  host.worldState.clock.windTo(host.worldState.clock.seconds() + 1e6);
  host.gathering.tick();
  assert.equal(host.gathering.picksOf(picker.dimension).size, 1);
  // Only a script brings them back now.
  assert.throws(() => api.Gathering.respawn({ x: 0, y: 0, z: Number.NaN }, 1), TypeError);
  assert.equal(api.Gathering.respawn({ x: AT.x, y: AT.y + 10, z: AT.z }, 5, picker.dimension), 0);
  // A world left out is the global one, where the players are.
  assert.equal(picker.dimension, 0xffffffff);
  assert.equal(api.Gathering.respawn(AT, 5), 1);
  assert.equal(api.Gathering.regrowEverywhere(), 0);
  api.Gathering.resetRegrowth();
  assert.equal(api.Gathering.regrowTime, null);
});

test('a player whose world gets ready is told every pick of it', async (t) => {
  const { host, sent, picker } = await world(t);
  host.gathering.report(picker, pick());
  sent.length = 0;
  const late = { id: 9, ready: true, dimension: picker.dimension };
  host.gathering.arrived(late);
  assert.deepEqual(ops(sent, 'gatherPlants'), [
    {
      op: 'gatherPlants',
      id: 9,
      full: true,
      picked: [[1, WORT.id, AT.x, AT.y, AT.z, 1.5]],
      grown: [],
    },
  ]);
});

test('natural spawns switched off hide a kind map-wide for everyone, late joiners too', async (t) => {
  const { host, api, sent, picker } = await world(t);
  assert.throws(() => api.Gathering.setNaturalSpawns('no'), /enabled must be true or false/);
  assert.throws(() => api.Gathering.setNaturalSpawns(false, 'nettle?'), /unknown herb kind/);
  sent.length = 0;
  assert.equal(api.Gathering.setNaturalSpawns(false, WORT.name), 1);
  assert.equal(api.Gathering.setNaturalSpawns(false, WORT.id), 0);
  assert.deepEqual(api.Gathering.naturalSpawnsOff(), [WORT.name]);
  const off = ops(sent, 'gatherPlants').find((message) => message.id === picker.id);
  assert.deepEqual(off.picked, [[1, WORT.id, 0, 0, 0, 100000]]);
  assert.equal(off.full, false);
  // Nobody can pick a switched-off kind; a late joiner hides it too.
  assert.equal(host.gathering.report(picker, pick()), false);
  sent.length = 0;
  host.gathering.arrived({ id: 9, ready: true, dimension: picker.dimension });
  assert.deepEqual(ops(sent, 'gatherPlants')[0].picked, [[1, WORT.id, 0, 0, 0, 100000]]);
  // Back on: the pick grows back and the herb can be picked again.
  sent.length = 0;
  assert.equal(api.Gathering.setNaturalSpawns(true), 1);
  assert.deepEqual(api.Gathering.naturalSpawnsOff(), []);
  const on = ops(sent, 'gatherPlants').find((message) => message.id === picker.id);
  assert.deepEqual(on.grown, [1]);
  host.gathering.lastPick.clear();
  assert.equal(host.gathering.report(picker, pick({ request: 2 })), true);
  // Every kind at once.
  assert.equal(api.Gathering.setNaturalSpawns(false), KINDS.size);
});

// The level's own herbs: three St John's worts near AT, a dandelion beyond
// them and one wort far away.
const { AuthoredHerbs, loadAuthoredHerbs } = require('./gathering/authored_herbs.cjs');
const DANDELION = KINDS.get(22);

function withHerbs(host) {
  host.areas.level = 'test-level';
  host.gathering.loadHerbs = (level) => {
    assert.equal(level, 'test-level');
    const plants = [
      [WORT.id, AT.x, AT.y, AT.z],
      [WORT.id, AT.x + 3, AT.y, AT.z],
      [WORT.id, AT.x, AT.y + 6, AT.z],
      [DANDELION.id, AT.x + 9, AT.y, AT.z],
      [WORT.id, AT.x + 500, AT.y, AT.z],
    ];
    return AuthoredHerbs.of(plants);
  };
}

test('herbsWithin lists the level plants in a sphere that are there to pick', async (t) => {
  const { host, api, picker } = await world(t);
  withHerbs(host);
  const near = api.Gathering.herbsWithin(AT, 10);
  assert.equal(near.length, 4);
  assert.deepEqual(Object.keys(near[0]).sort(), ['item', 'kind', 'position']);
  assert.equal(near[0].kind, WORT.id);
  assert.equal(near[0].item, WORT.item);
  assert.deepEqual([near[0].position.x, near[0].position.y], [AT.x, AT.y]);
  assert.equal(api.Gathering.herbsWithin(AT, 5).length, 2, 'the radius is inclusive and 3D');
  // A pick takes the plants it covers in its world only.
  assert.equal(host.gathering.report(picker, pick({ radius: 1 })), true);
  assert.equal(api.Gathering.herbsWithin(AT, 10).length, 3);
  assert.equal(api.Gathering.herbsWithin(AT, 10, 5).length, 4);
  // A kind switched off is not there to pick anywhere.
  api.Gathering.setNaturalSpawns(false, DANDELION.name);
  assert.deepEqual(
    api.Gathering.herbsWithin(AT, 10).map((herb) => herb.kind),
    [WORT.id, WORT.id],
  );
  assert.throws(() => api.Gathering.herbsWithin({ x: NaN, y: 0, z: 0 }, 1), TypeError);
  assert.throws(() => api.Gathering.herbsWithin(AT, -1), TypeError);
  assert.throws(() => api.Gathering.herbsWithin(AT, 1, -2), TypeError);
});

test('clearHerbs hides the level plants until a respawn brings them back', async (t) => {
  const { host, api, sent, picker } = await world(t);
  withHerbs(host);
  assert.equal(host.gathering.report(picker, pick({ radius: 1 })), true);
  sent.length = 0;
  // Picked plants count too; a second removal of the same plants counts nothing.
  assert.equal(api.Gathering.clearHerbs(AT, 4), 2);
  assert.equal(api.Gathering.clearHerbs(AT, 4), 0);
  const told = ops(sent, 'gatherPlants');
  assert.deepEqual(
    told.map((message) => message.id),
    [7, 8],
  );
  assert.deepEqual(
    told[0].picked.map((entry) => entry.slice(1)),
    [
      [WORT.id, AT.x, AT.y, AT.z, 0.1],
      [WORT.id, AT.x + 3, AT.y, AT.z, 0.1],
    ],
  );
  assert.equal(api.Gathering.herbsWithin(AT, 10).length, 2);
  // Another world keeps its plants; a new regrowth policy does not bring removed plants back.
  assert.equal(api.Gathering.herbsWithin(AT, 10, 5).length, 4);
  assert.equal(api.Gathering.setRegrowth(0.001, 0.001), true);
  api.World.setTimeScale(1000);
  host.worldState.clock.advance?.(3600);
  host.gathering.tick();
  assert.equal(api.Gathering.herbsWithin(AT, 4).length, 0);
  // A respawn ends the pick and both removals; the plants can be removed again.
  assert.equal(api.Gathering.respawn(AT, 4), 3);
  assert.equal(api.Gathering.herbsWithin(AT, 10).length, 4);
  assert.equal(api.Gathering.clearHerbs(AT, 10), 4);
  assert.equal(api.Gathering.regrowEverywhere(), 4);
  assert.throws(() => api.Gathering.clearHerbs(AT, 100001), TypeError);
});

test('an area lists, removes and respawns the level plants inside it', async (t) => {
  const { host, api } = await world(t);
  withHerbs(host);
  const area = api.Area.create({
    id: 'meadow',
    type: 'sphere',
    position: { x: AT.x + 1, y: AT.y, z: AT.z },
    radius: 5,
  });
  assert.equal(area.herbsWithin().length, 2);
  assert.equal(area.clearHerbs(), 2);
  assert.equal(area.clearHerbs(), 0);
  assert.equal(area.herbsWithin().length, 0);
  assert.equal(api.Gathering.herbsWithin(AT, 10).length, 2, 'the plants outside stay');
  assert.equal(area.herbsWithin(5).length, 2, 'another world keeps its plants');
  assert.equal(area.regrowHerbs(), 2);
  assert.equal(area.herbsWithin().length, 2);
  // An area of one world answers nothing for another; a destroyed area answers nothing.
  const pond = api.Area.create({
    id: 'pond',
    type: 'sphere',
    position: AT,
    radius: 1,
    dimension: 3,
  });
  assert.equal(pond.herbsWithin().length, 1);
  assert.deepEqual(pond.herbsWithin(4), []);
  assert.equal(pond.clearHerbs(4), 0);
  assert.throws(() => pond.herbsWithin(-1), TypeError);
  pond.destroy();
  assert.deepEqual(pond.herbsWithin(), []);
  assert.equal(pond.regrowHerbs(), 0);
});

test('the herb catalog decodes each kind from centimetre steps, and the level has its herbs', () => {
  // Kind 2 with one plant, then kind 22 with two, each step in centimetres.
  const file = Buffer.alloc(2 * 5 + 3 * 12);
  file.writeUInt8(2, 0);
  file.writeUInt32LE(1, 1);
  [1000, 0, 0].forEach((step, i) => file.writeInt32LE(step, 5 + 4 * i));
  file.writeUInt8(22, 17);
  file.writeUInt32LE(2, 18);
  [100, 200, 300, 50, 0, -100].forEach((step, i) => file.writeInt32LE(step, 22 + 4 * i));
  const decoded = AuthoredHerbs.decode(file);
  assert.equal(decoded.size, 3);
  assert.deepEqual(
    [0, 1, 2].map((index) => [decoded.kindOf(index), decoded.positionOf(index)]),
    [
      [2, { x: 10, y: 0, z: 0 }],
      [22, { x: 1, y: 2, z: 3 }],
      [22, { x: 1.5, y: 2, z: 2 }],
    ],
  );
  assert.deepEqual(decoded.within({ x: 1, y: 2, z: 3 }, 1), [1]);
  // The shipped catalogs: both levels' herbs and mushrooms, every plant of a known kind.
  for (const name of ['trosecko', 'kutnohorsko']) {
    const level = loadAuthoredHerbs(name);
    assert.ok(level.size > 100000, `${name} has its herbs`);
    const kinds = new Set();
    for (let index = 0; index < level.size; index++) {
      kinds.add(level.kindOf(index));
    }
    assert.ok(
      [...kinds].every((kind) => KINDS.has(kind)),
      `${name}: known kinds`,
    );
    assert.ok(kinds.has(2), `${name}: nettles`);
    assert.ok(kinds.has(28) && kinds.has(31), `${name}: mushrooms`);
  }
  assert.equal(loadAuthoredHerbs('klaster').size, 0, 'a level the catalog does not hold');
});
