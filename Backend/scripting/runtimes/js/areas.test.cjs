'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { buildShape, geometryOf } = require('./areas/area_shapes.cjs');

// Area: the shapes, the lookups, and the enter and exit events a moving body raises.

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-areas-'));
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: () => {},
    log: () => {},
  });
  await host.startAll();
  const events = [];
  host.on('areaEnter', (area, entity, matching) =>
    events.push(['enter', area.id, entity.id, matching]),
  );
  host.on('areaExit', (area, entity, matching) =>
    events.push(['exit', area.id, entity.id, matching]),
  );
  const player = (id, x, y, z = 0, fields = {}) =>
    host.updatePlayer({
      id,
      nickname: `Player ${id}`,
      ready: true,
      position: { x, y, z },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      ...fields,
    });
  // The next tick: the settling pass of changed areas runs in a microtask.
  const tick = () => new Promise((resolve) => setImmediate(resolve));
  return { host, Area: host.api.Area, events, player, tick };
}

function containsOf(definition) {
  return buildShape(geometryOf(definition));
}

const sphere = (id, x, y, radius, more = {}) => ({
  id,
  type: 'sphere',
  position: { x, y, z: 0 },
  radius,
  ...more,
});

test('a sphere contains what lies within its radius', () => {
  const shape = containsOf(sphere('s', 10, 20, 5));
  assert.equal(shape.contains(10, 20, 0), true);
  assert.equal(shape.contains(15, 20, 0), true);
  assert.equal(shape.contains(15.1, 20, 0), false);
  assert.equal(shape.contains(10, 20, 5.1), false);
  assert.deepEqual(shape.bounds.min.toJSON(), { x: 5, y: 15, z: -5 });
});

test('a box is placed from its position and turned by its rotation', () => {
  const box = {
    id: 'b',
    type: 'box',
    position: { x: 100, y: 0, z: 0 },
    min: { x: -1, y: -5, z: 0 },
    max: { x: 1, y: 5, z: 2 },
  };
  const upright = containsOf(box);
  assert.equal(upright.contains(100, 4, 1), true);
  assert.equal(upright.contains(104, 0, 1), false);
  assert.equal(upright.contains(100, 0, 2.5), false);
  // Euler angles in degrees: a quarter turn about the vertical axis swaps the long side.
  const turned = containsOf({ ...box, rotation: { x: 0, y: 0, z: 90 } });
  assert.equal(turned.contains(104, 0, 1), true);
  assert.equal(turned.contains(100, 4, 1), false);
  assert.ok(Math.abs(turned.bounds.max.x - 105) < 1e-9);
});

test('a polygon follows its outline and reaches up from its lowest corner', () => {
  // An L: the square 0..10 without the corner 5..10 x 5..10.
  const points = [
    { x: 0, y: 0, z: 3 },
    { x: 10, y: 0, z: 3 },
    { x: 10, y: 5, z: 3 },
    { x: 5, y: 5, z: 3 },
    { x: 5, y: 10, z: 3 },
    { x: 0, y: 10, z: 3 },
  ];
  const walled = containsOf({ id: 'p', type: 'shape', points, height: 4 });
  assert.equal(walled.contains(2, 8, 5), true);
  assert.equal(walled.contains(8, 2, 5), true);
  assert.equal(walled.contains(8, 8, 5), false);
  assert.equal(walled.contains(2, 2, 2.9), false);
  assert.equal(walled.contains(2, 2, 7.1), false);
  const open = containsOf({ id: 'p', type: 'shape', points });
  assert.equal(open.contains(2, 2, -500), true);
  assert.equal(open.contains(2, 2, 5000), true);
});

test('a shape that cannot be built is refused', () => {
  const bad = [
    { id: 'x', type: 'cone' },
    { id: 'x', type: 'sphere', radius: 0 },
    { id: 'x', type: 'sphere', radius: Infinity },
    { id: 'x', type: 'box', min: { x: 0, y: 0, z: 0 }, max: { x: 1, y: 0, z: 1 } },
    { id: 'x', type: 'shape', points: [{ x: 0, y: 0, z: 0 }] },
    { id: 'x', type: 'shape', points: new Array(257).fill({ x: 0, y: 0, z: 0 }) },
    { id: 'x', type: 'sphere', radius: 1, position: { x: NaN, y: 0, z: 0 } },
    { id: 'x', type: 'sphere', radius: 1, rotation: { x: 0, y: 0, z: 0, w: 0 } },
  ];
  for (const definition of bad) {
    assert.throws(() => geometryOf(definition), /Error/, JSON.stringify(definition));
  }
});

test('create, look up, list and destroy', async () => {
  const { Area } = await startedHost();
  const area = Area.create(
    sphere('market', 0, 0, 10, { name: 'Market', labels: ['Town', 'safe'], metadata: { fee: 2 } }),
  );
  assert.equal(area.id, 'market');
  assert.equal(area.kind, 'script');
  assert.equal(area.type, 'sphere');
  assert.equal(area.name, 'Market');
  assert.equal(area.enabled, true);
  assert.equal(area.dimension, null);
  assert.deepEqual(area.metadata, { fee: 2 });
  assert.equal(Area.getById('market'), area);
  assert.equal(new Area(area.handle), area);
  assert.equal(Area.getById('nope'), null);
  assert.deepEqual(Area.all(), [area]);
  assert.deepEqual(Area.all('script'), [area]);
  assert.deepEqual(Area.all('level'), []);
  assert.deepEqual(Area.taggedWith('TOWN'), [area]);
  assert.deepEqual(Area.taggedWith('castle'), []);
  assert.equal(area.toString(), 'Area(market, script, sphere, Market)');
  assert.throws(() => Area.create(sphere('market', 5, 5, 1)), /already taken/);
  assert.throws(() => Area.create(sphere('00ff00ff00ff00ff', 5, 5, 1)), RangeError);
  assert.throws(
    () => Area.create(sphere('c', 0, 0, 1, { collision: true, wallThickness: 5 })),
    RangeError,
  );
  assert.throws(() => Area.create({ type: 'sphere', radius: 1 }), TypeError);
  assert.throws(() => Area.create(sphere('w', 0, 0, 1, { dimension: -1 })), TypeError);
  assert.equal(Area.all().length, 1, 'a refused definition builds nothing');
  area.name = 'Square';
  area.labels = ['plaza'];
  area.metadata = { fee: 3 };
  assert.equal(area.toString(), 'Area(market, script, sphere, Square)');
  assert.deepEqual(Area.taggedWith('town'), []);
  assert.deepEqual(area.metadata, { fee: 3 });
  assert.throws(() => (area.metadata = { x: 1n }), TypeError);
  assert.equal(area.destroy(), true);
  assert.equal(area.destroy(), false);
  assert.equal(area.exists, false);
  assert.equal(area.id, '');
  assert.equal(area.toJSON(), null);
  assert.deepEqual(area.players, []);
  assert.equal(Area.getById('market'), null);
  assert.deepEqual(Area.all(), []);
  // The id can be used again.
  assert.equal(Area.create(sphere('market', 0, 0, 1)).exists, true);
});

test('toJSON gives the definition that builds the area again', async () => {
  const { Area } = await startedHost();
  const area = Area.create({
    id: 'yard',
    type: 'shape',
    position: { x: 5, y: 6, z: 7 },
    points: [
      { x: 0, y: 0, z: 0 },
      { x: 4, y: 0, z: 0 },
      { x: 4, y: 4, z: 0 },
    ],
    height: 3,
    labels: ['private'],
    metadata: { owner: 'Jan' },
    dimension: 3,
    enabled: false,
  });
  const saved = JSON.parse(JSON.stringify(area));
  assert.equal(saved.dimension, 3);
  assert.equal(saved.enabled, false);
  assert.equal(saved.height, 3);
  assert.deepEqual(saved.position, { x: 5, y: 6, z: 7 });
  area.destroy();
  const again = Area.create(saved);
  assert.deepEqual(JSON.parse(JSON.stringify(again)), saved);
  assert.equal(again.height, 3);
  assert.equal(again.enabled, false);
});

test('lookups by position, body and distance', async () => {
  const { host, Area, player } = await startedHost();
  const near = Area.create(sphere('near', 0, 0, 10));
  const far = Area.create(sphere('far', 100, 0, 10, { dimension: 2 }));
  const off = Area.create(sphere('off', 0, 0, 3, { enabled: false }));
  const at = { x: 1, y: 1, z: 0 };
  assert.deepEqual(Area.getAt(at), [near]);
  assert.deepEqual(Area.getAt({ x: 100, y: 0, z: 0 }), [far]);
  assert.deepEqual(Area.getAt({ x: 100, y: 0, z: 0 }, 1), []);
  assert.deepEqual(Area.getAt({ x: 100, y: 0, z: 0 }, 2), [far]);
  assert.equal(off.contains(at), true, 'contains ignores enabled');
  // Nearest first, enabled or not: the bounds lie 35, 42 and 45 metres away.
  assert.deepEqual(
    Area.nearby({ x: 45, y: 0, z: 0 }, 46).map((area) => area.id),
    ['near', 'off', 'far'],
  );
  assert.deepEqual(Area.nearby({ x: 45, y: 0, z: 0 }, 20), []);
  assert.deepEqual(Area.nearby({ x: 90, y: 0, z: 0 }, 5, 1), []);
  const p = player(7, 1, 1, 0, { dimension: 4 });
  assert.equal(near.holdsEntity(p), true);
  assert.equal(far.holdsEntity(p), false);
  assert.deepEqual(Area.aroundEntity(p), [near]);
  assert.throws(() => Area.aroundEntity({ id: 7 }), TypeError);
  assert.throws(() => Area.getAt({ x: 'a', y: 0, z: 0 }), TypeError);
  assert.equal(host.areas.byId.size, 3);
});

test('a player walking in and out raises one enter and one exit', async () => {
  const { Area, events, player } = await startedHost();
  const area = Area.create(sphere('zone', 0, 0, 10));
  player(7, 50, 0);
  player(7, 5, 0);
  player(7, 5.01, 0);
  player(7, 6, 0);
  assert.deepEqual(events, [['enter', 'zone', 7, true]]);
  const p = player(7, 5, 0);
  assert.deepEqual(area.players, [p]);
  assert.deepEqual(area.occupants, [p]);
  player(7, 50, 0);
  assert.deepEqual(events.slice(1), [['exit', 'zone', 7, true]]);
  assert.deepEqual(area.players, []);
});

test('a player who is not in the world yet is never inside', async () => {
  const { Area, events, player } = await startedHost();
  Area.create(sphere('zone', 0, 0, 10));
  player(7, 0, 0, 0, { ready: false });
  assert.deepEqual(events, []);
  player(7, 0, 0);
  assert.deepEqual(events, [['enter', 'zone', 7, true]]);
});

test('an area bound to a world is crossed by other worlds with matchingDimension false', async () => {
  const { Area, events, player } = await startedHost();
  const area = Area.create(sphere('zone', 0, 0, 10, { dimension: 5 }));
  player(7, 0, 0, 0, { dimension: 5 });
  player(8, 0, 0, 0, { dimension: 6 });
  assert.deepEqual(events, [
    ['enter', 'zone', 7, true],
    ['enter', 'zone', 8, false],
  ]);
  assert.equal(area.players.length, 2, 'occupants are by geometry, in any world');
  area.dimension = null;
  player(7, 99, 0, 0, { dimension: 5 });
  assert.deepEqual(events[2], ['exit', 'zone', 7, true]);
});

test('a change of world alone is checked too', async () => {
  const { Area, events, player } = await startedHost();
  Area.create(sphere('zone', 0, 0, 10));
  const p = player(7, 0, 0, 0, { dimension: 1 });
  p.setDimension(2);
  assert.equal(events.length, 1, 'still inside, so no event');
});

test('an area made around a standing player raises the enter on the next tick', async () => {
  const { Area, events, player, tick } = await startedHost();
  player(7, 0, 0);
  const area = Area.create(sphere('zone', 0, 0, 10));
  assert.deepEqual(events, []);
  await tick();
  assert.deepEqual(events, [['enter', 'zone', 7, true]]);
  assert.equal(area.players.length, 1);
});

test('moving, reshaping and disabling an area raise the crossings it causes', async () => {
  const { Area, events, player, tick } = await startedHost();
  player(7, 0, 0);
  player(8, 100, 0);
  const area = Area.create(sphere('zone', 0, 0, 10));
  await tick();
  events.length = 0;
  area.position = { x: 100, y: 0, z: 0 };
  await tick();
  assert.deepEqual(events, [
    ['exit', 'zone', 7, true],
    ['enter', 'zone', 8, true],
  ]);
  events.length = 0;
  assert.equal(area.setShape({ radius: 0 }), false);
  assert.equal(area.setShape({ type: 'box' }), false, 'a box without corners cannot be built');
  assert.equal(area.setShape({ radius: 500 }), true);
  await tick();
  assert.deepEqual(events, [['enter', 'zone', 7, true]]);
  events.length = 0;
  area.enabled = false;
  await tick();
  // Those who were inside leave, in the order they came.
  assert.deepEqual(
    events.map((event) => event[0]),
    ['exit', 'exit'],
  );
  assert.deepEqual(events.map((event) => event[2]).sort(), [7, 8]);
  assert.deepEqual(Area.getAt({ x: 0, y: 0, z: 0 }), []);
  events.length = 0;
  area.enabled = true;
  await tick();
  assert.deepEqual(
    events.map((event) => event[0]),
    ['enter', 'enter'],
  );
  assert.throws(() => (area.enabled = 'yes'), TypeError);
});

test('several changes in one tick settle once, to the final shape', async () => {
  const { Area, events, player, tick } = await startedHost();
  player(7, 0, 0);
  const area = Area.create(sphere('zone', 500, 0, 10));
  area.position = { x: 0, y: 0, z: 0 };
  area.enabled = false;
  area.enabled = true;
  await tick();
  assert.deepEqual(events, [['enter', 'zone', 7, true]]);
});

test('destroy raises the exits at once, while the handle still resolves', async () => {
  const { host, Area, events, player } = await startedHost();
  const seen = [];
  host.on('areaExit', (area) => seen.push([area.exists, Area.getById(area.id) === area]));
  const area = Area.create(sphere('zone', 0, 0, 10));
  player(7, 0, 0);
  player(8, 0, 0);
  area.destroy();
  assert.deepEqual(events.slice(2), [
    ['exit', 'zone', 7, true],
    ['exit', 'zone', 8, true],
  ]);
  assert.deepEqual(seen, [
    [true, true],
    [true, true],
  ]);
  assert.equal(area.exists, false);
});

test('a body that leaves the server raises nothing', async () => {
  const { host, Area, events, player } = await startedHost();
  const area = Area.create(sphere('zone', 0, 0, 10));
  player(7, 0, 0);
  const horse = host.api.Horse.spawn({ x: 1, y: 1, z: 0 });
  const npc = host.api.Npc.create({ position: { x: 2, y: 2, z: 0 } });
  assert.equal(area.occupants.length, 3);
  events.length = 0;
  host.disconnect(7);
  horse.destroy();
  npc.remove();
  assert.deepEqual(events, []);
  assert.deepEqual(area.occupants, []);
  assert.equal(host.areas.bodies.size, 0);
});

test('NPCs and horses cross areas too', async () => {
  const { host, Area, events } = await startedHost();
  const area = Area.create(sphere('zone', 0, 0, 10));
  const npc = host.api.Npc.create({ position: { x: 50, y: 0, z: 0 } });
  const horse = host.api.Horse.spawn({ x: 60, y: 0, z: 0 });
  assert.deepEqual(events, []);
  // The simulator reports the NPC walking in; the native server reports the horse.
  host.npcs.update({
    id: npc.id,
    position: { x: 3, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.updateHorse({
    id: horse.id,
    position: { x: 4, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    riderId: 0,
  });
  assert.deepEqual(events, [
    ['enter', 'zone', npc.id, true],
    ['enter', 'zone', horse.id, true],
  ]);
  assert.deepEqual(Area.aroundEntity(npc), [area]);
  assert.deepEqual(Area.aroundEntity(horse), [area]);
  npc.teleport({ x: 40, y: 0, z: 0 });
  assert.deepEqual(events[2], ['exit', 'zone', npc.id, true]);
  npc.position = { x: 0, y: 0, z: 0 };
  assert.deepEqual(events[3], ['enter', 'zone', npc.id, true]);
  assert.deepEqual(area.occupants, [horse, npc]);
  assert.deepEqual(area.players, []);
});

test('a position update tests only the areas near it, and a standing body tests none', async () => {
  const { host, Area, events, player } = await startedHost();
  let tested = 0;
  for (let i = 0; i < 40; i += 1) {
    for (let j = 0; j < 25; j += 1) {
      Area.create(sphere(`a${i}_${j}`, 1000 + i * 100, j * 100, 20));
    }
  }
  Area.create(sphere('home', 0, 0, 20));
  for (const state of host.areas.byId.values()) {
    const contains = state.shape.contains;
    state.shape.contains = (x, y, z) => {
      tested += 1;
      return contains(x, y, z);
    };
  }
  player(7, 5, 0);
  assert.deepEqual(events, [['enter', 'home', 7, true]]);
  const first = tested;
  assert.ok(first <= 2, `tested ${first} of 1001 areas`);
  player(7, 5, 0);
  player(7, 5.01, 0);
  assert.equal(tested, first, 'a body that has not moved is not tested');
  player(7, 6, 0);
  assert.ok(tested - first <= 2);
});

test('an area across the whole map is still found', async () => {
  const { Area, events, player } = await startedHost();
  Area.create(sphere('map', 0, 0, 8000));
  player(7, 5000, -5000);
  player(8, 0, 0);
  player(9, 7000, -7000);
  assert.deepEqual(events, [
    ['enter', 'map', 7, true],
    ['enter', 'map', 8, true],
  ]);
});

// The level's own areas.

const { entryOf, loadLevelAreas } = require('./areas/level_areas.cjs');

// A level TriggerArea: a 10 m square at (100, 100), 4 m high from z 2.
const levelSquare = entryOf({
  id: '00AA00AA00AA00AA',
  name: 'test_areaMarker_house',
  type: 'TriggerArea',
  labels: ['private', 'interior'],
  height: 4,
  points: [100, 100, 2, 110, 100, 2, 110, 110, 2, 100, 110, 2],
});

test('a level area is read only but for enabled, and is listed first', async () => {
  const { host, Area, events, player, tick } = await startedHost();
  const script = Area.create(sphere('market', 0, 0, 5, { labels: ['private'] }));
  host.areas.setLevel('test', [levelSquare]);
  const area = Area.getById('00aa00aa00aa00aa');
  assert.equal(Area.getById('00AA00AA00AA00AA'), area);
  assert.equal(area.kind, 'level');
  assert.equal(area.type, 'TriggerArea');
  assert.equal(area.name, 'test_areaMarker_house');
  assert.deepEqual(area.labels, ['private', 'interior']);
  assert.deepEqual(area.metadata, {});
  assert.equal(area.dimension, null);
  assert.equal(area.height, 4);
  assert.deepEqual(area.position.toJSON(), { x: 105, y: 105, z: 4 });
  assert.deepEqual(area.rotation.toJSON(), { w: 1, x: 0, y: 0, z: 0 });
  assert.equal(
    area.toString(),
    'Area(00aa00aa00aa00aa, level, TriggerArea, test_areaMarker_house)',
  );
  assert.deepEqual(Area.all(), [area, script]);
  assert.deepEqual(Area.all('level'), [area]);
  assert.deepEqual(Area.all('script'), [script]);
  assert.deepEqual(Area.taggedWith('PRIVATE'), [area, script]);
  assert.throws(() => {
    area.name = 'mine';
  }, /level's and cannot be changed/);
  assert.throws(() => {
    area.labels = [];
  }, TypeError);
  assert.throws(() => {
    area.metadata = { owner: 1 };
  }, TypeError);
  assert.throws(() => {
    area.position = { x: 0, y: 0, z: 0 };
  }, TypeError);
  assert.equal(area.setShape({ height: 9 }), false);
  assert.equal(area.destroy(), false);
  assert.equal(area.toJSON(), null);
  assert.equal(area.exists, true);

  // A body in any world crosses it, its world matching; enabled off raises the exit.
  player(7, 105, 105, 3, { dimension: 4 });
  await tick();
  assert.deepEqual(events, [['enter', '00aa00aa00aa00aa', 7, true]]);
  assert.deepEqual(Area.getAt({ x: 105, y: 105, z: 3 }, 4), [area]);
  assert.deepEqual(Area.getAt({ x: 105, y: 105, z: 6.5 }), []);
  assert.deepEqual(
    Area.nearby({ x: 120, y: 105, z: 3 }, 200).map((found) => found.id),
    ['00aa00aa00aa00aa', 'market'],
  );
  area.enabled = false;
  await tick();
  assert.deepEqual(events.at(-1), ['exit', '00aa00aa00aa00aa', 7, true]);
  assert.deepEqual(Area.getAt({ x: 105, y: 105, z: 3 }), []);
});

test('another map takes the level areas with it', async () => {
  const { host, Area, events, player, tick } = await startedHost();
  host.areas.setLevel('test', [levelSquare]);
  player(7, 105, 105, 3);
  await tick();
  host.areas.setLevel('test', [levelSquare]);
  assert.equal(events.length, 1);
  host.areas.setLevel('other', []);
  assert.deepEqual(events, [
    ['enter', '00aa00aa00aa00aa', 7, true],
    ['exit', '00aa00aa00aa00aa', 7, true],
  ]);
  assert.deepEqual(Area.all('level'), []);
});

test('a level union is entered from any of its rooms and lists them', async () => {
  const { host, Area, events, player, tick } = await startedHost();
  const room = (id, x) =>
    entryOf({
      id,
      name: `room_${id}`,
      type: 'TriggerArea',
      labels: [],
      height: 4,
      points: [x, 100, 2, x + 10, 100, 2, x + 10, 110, 2, x, 110, 2],
    });
  const union = entryOf({
    id: 'UU00UU00UU00UU00',
    name: 'test_AreaUnion_house',
    type: 'AreaUnion',
    labels: ['private', 'interior'],
    members: ['0000000000000001', '0000000000000002', 'ffffffffffffffff'],
  });
  host.areas.setLevel('test', [
    union,
    room('0000000000000001', 100),
    room('0000000000000002', 110),
  ]);
  const house = Area.getById('uu00uu00uu00uu00');
  assert.equal(house.type, 'AreaUnion');
  assert.deepEqual(house.labels, ['private', 'interior']);
  assert.deepEqual(
    house.members.map((member) => member.id),
    ['0000000000000001', '0000000000000002'],
  );
  assert.deepEqual(Area.getById('0000000000000001').members, []);
  // Walking from one room into the next stays inside the house.
  player(7, 105, 105, 3);
  await tick();
  player(7, 115, 105, 3);
  await tick();
  player(7, 140, 105, 3);
  await tick();
  const houseEvents = events.filter((event) => event[1] === 'uu00uu00uu00uu00');
  assert.deepEqual(houseEvents, [
    ['enter', 'uu00uu00uu00uu00', 7, true],
    ['exit', 'uu00uu00uu00uu00', 7, true],
  ]);
  // A union whose members the level does not have is left out.
  host.areas.setLevel('other', [union]);
  assert.deepEqual(Area.all('level'), []);
});

test('the catalog holds the levels of the game', () => {
  const trosecko = loadLevelAreas('trosecko');
  assert.ok(trosecko.length > 3000);
  const inn = trosecko.find((entry) => entry.id === '108c8fabab6e4bf9');
  assert.equal(inn.name, 'pracharna_areaMarker_inn');
  assert.equal(inn.type, 'TriggerArea');
  assert.equal(inn.height, 20);
  assert.equal(inn.points.length, 6);
  const shape = buildShape({
    type: 'shape',
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    points: inn.points,
    height: inn.height,
  });
  assert.equal(shape.contains(2340, 2070, 110), true);
  assert.equal(shape.contains(2340, 2070, 130), false);
  assert.ok(trosecko.some((entry) => entry.type === 'SmartAreaShape'));
  assert.ok(loadLevelAreas('kutnohorsko').length > 6000);
  // The tavern's house in Trosecko joins five rooms.
  const house = trosecko.find((entry) => entry.id === '1d0bb29960b90c5b');
  assert.equal(house.type, 'AreaUnion');
  assert.equal(house.members.length, 5);
  assert.deepEqual(loadLevelAreas('klaster'), []);
});

test('a catalog entry that cannot be an area is left out', () => {
  assert.equal(entryOf({ id: 'a', type: 'AreaUnion', points: [0, 0, 0, 1, 0, 0, 0, 1, 0] }), null);
  assert.equal(entryOf({ id: 'a', type: 'AreaUnion', members: [] }), null);
  assert.equal(entryOf({ id: 'a', type: 'TriggerArea', points: [0, 0, 0, 1, 0, 0] }), null);
  assert.equal(
    entryOf({ id: 'a', type: 'TriggerArea', points: [0, 0, 0, 1, 0, 0, 0, NaN, 0] }),
    null,
  );
  assert.equal(
    entryOf({ id: 'a', type: 'SmartAreaShape', points: [0, 0, 0, 1, 0, 0, 0, 1, 0] }).height,
    0,
  );
});
