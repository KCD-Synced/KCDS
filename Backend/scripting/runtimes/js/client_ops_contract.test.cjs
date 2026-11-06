'use strict';

// The 0.2.1 client ops: one contract for the bridge ops that areas, markers and blips,
// the world clock and weather, props, doors and vfx send to the players' games.
// 0.2.1-client-ops.md documents the same contract; a change to a field has to change both.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const GLOBAL_WORLD = 0xffffffff;
const MAGIC = 'WH_Particels.other.magic';

const int = Number.isInteger;
const num = (v) => typeof v === 'number' && Number.isFinite(v);
const str = (v) => typeof v === 'string';
const bool = (v) => typeof v === 'boolean';
const vec = (v) => Boolean(v) && num(v.x) && num(v.y) && num(v.z);
const quat = (v) => vec(v) && num(v.w);
const ids = (v) => Array.isArray(v) && v.every(int);

const TUNING = {
  scale: num,
  countScale: num,
  speedScale: num,
  timeScale: num,
  strength: num,
  pulsePeriod: num,
};

// op name -> field name -> type check. Every field is required; no other field may be sent.
// The part of a siege engine's state that siegeSpawn and siegeUpdate both carry.
const SIEGE_STATE = {
  cycle: str,
  seconds: num,
  speed: num,
  rotation: quat,
  aimGuide: bool,
  target: vec,
  wrecked: bool,
  usable: bool,
  operator: int,
};

const CONTRACT = {
  markerShow: {
    id: int,
    marker: int,
    material: str,
    shape: str,
    position: vec,
    rotation: quat,
    size: num,
    height: num,
    spin: num,
    bob: num,
    color: () => true,
  },
  markerHide: { id: int, marker: int },
  blipShow: {
    id: int,
    blip: int,
    type: str,
    markState: num,
    label: str,
    compass: bool,
    map: bool,
    position: vec,
  },
  blipHide: { id: int, blip: int },
  worldTime: { id: int, day: num, hour: num, timeScale: num },
  worldWeather: {
    id: int,
    preset: str,
    previous: str,
    seconds: num,
    remaining: num,
    rainStrength: num,
    rainLevel: num,
    wind: vec,
  },
  propSpawn: {
    id: int,
    model: str,
    position: vec,
    rotation: quat,
    scale: vec,
    physics: str,
    dimension: num,
    mass: num,
  },
  propUpdate: { id: int, physics: str, scale: vec, mass: num },
  propSimulate: { id: int, simulate: bool },
  propPush: { id: int, push: str, linear: vec, angular: vec, point: vec, hasPoint: bool },
  propMove: { id: int, position: vec, rotation: quat },
  recipeTable: { file: str, patch: str },
  recipePicture: { which: str, id: int, from: int, dds: Buffer.isBuffer },
  propDestroy: { id: int },
  doorSet: { key: str, id: int, guid: str, open: bool, side: bool, locked: bool, difficulty: num },
  doorNearest: { id: int, request: int },
  smartObjectSpawn: {
    id: int,
    template: str,
    position: vec,
    rotation: quat,
    dimension: int,
    corrections: (v) =>
      Array.isArray(v) && v.every((c) => int(c.place) && vec(c.at) && quat(c.turn)),
  },
  smartObjectDestroy: { id: int },
  doorSpawn: {
    guid: str,
    model: str,
    position: vec,
    rotation: quat,
    animSet: str,
    helpers: str,
    smartObjectType: str,
    rigid: bool,
    pushable: bool,
    locked: bool,
    lockTypes: str,
    canBePicked: bool,
    difficulty: num,
    key: str,
    dimension: int,
  },
  doorDespawn: { guid: str },
  vfxSpawn: {
    id: int,
    effect: str,
    position: vec,
    rotation: quat,
    dimension: num,
    ...TUNING,
    prime: bool,
    body: num,
    joint: str,
  },
  vfxUpdate: { id: int, ...TUNING },
  vfxDestroy: { id: int },
  lightSpawn: {
    id: int,
    position: vec,
    color: vec,
    intensity: num,
    radius: num,
    castShadows: bool,
    body: num,
    joint: str,
  },
  lightUpdate: {
    id: int,
    position: vec,
    color: vec,
    intensity: num,
    radius: num,
    castShadows: bool,
    body: num,
    joint: str,
  },
  lightDestroy: { id: int },
  vfxBurst: {
    effect: str,
    position: vec,
    rotation: quat,
    dimension: num,
    players: ids,
    durationMs: num,
    prime: bool,
    ...TUNING,
  },
  vfxBurstOn: {
    effect: str,
    entity: int,
    offset: vec,
    joint: str,
    rotation: quat,
    players: ids,
    durationMs: num,
    prime: bool,
    ...TUNING,
  },
  siegeSpawn: {
    id: int,
    kind: str,
    position: vec,
    rotation: quat,
    dimension: num,
    ...SIEGE_STATE,
  },
  siegeUpdate: { id: int, ...SIEGE_STATE },
  siegeShot: {
    id: int,
    shot: int,
    from: vec,
    target: vec,
    flight: num,
    apex: num,
    reporter: int,
    dimension: num,
  },
  siegeDestroy: { id: int },
  siegeLadderSpawn: {
    id: int,
    kind: str,
    model: str,
    height: num,
    lean: num,
    position: vec,
    rotation: quat,
    pose: str,
    usable: bool,
    dimension: num,
  },
  siegeLadderPose: { id: int, pose: str, actor: int, usable: bool },
  siegeLadderDestroy: { id: int },
  siegeLadderRefused: { id: int },
  stonePileSpawn: {
    id: int,
    position: vec,
    rotation: quat,
    stones: int,
    usable: bool,
    dimension: num,
  },
  stonePileUpdate: { id: int, stones: int, usable: bool },
  stonePileDestroy: { id: int },
  worldRaycast: { id: int, request: int, query: int, start: vec, end: vec, mode: int, max: int },
  worldBodiesInRadius: { id: int, request: int, center: vec, radius: num },
  worldLevelEntities: { id: int, request: int, center: vec, radius: num, max: int },
  // The lookAt form; a shot aimed by a rotation carries `rotation` (a quaternion) and no roll.
  cameraShot: {
    id: int,
    position: vec,
    lookAt: vec,
    roll: num,
    fov: num,
    seconds: num,
    ease: bool,
  },
  cameraClear: { id: int },
};

function checkContract(message) {
  const contract = CONTRACT[message.op];
  assert.ok(contract, `${message.op} is not in the contract`);
  // `to` is on an op replayed to one player's game; the first send is a broadcast without it.
  const fields = Object.keys(message).filter((name) => name !== 'op' && name !== 'to');
  assert.ok(message.to === undefined || int(message.to), `${message.op}.to`);
  assert.deepEqual(fields.sort(), Object.keys(contract).sort(), `${message.op} fields`);
  for (const [name, valid] of Object.entries(contract)) {
    assert.ok(valid(message[name]), `${message.op}.${name} = ${JSON.stringify(message[name])}`);
  }
}

const DOOR = {
  guid: '00000000deadbeef',
  name: 'house_door',
  position: { x: 1, y: 2, z: 3 },
  open: false,
  side: false,
  locked: false,
  canBePicked: true,
  key: '',
  builtLocked: false,
};

async function started(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const api = f.host.makeAPI();
  const ops = (...names) => f.sent.filter((m) => names.includes(m.op));
  const clear = () => {
    f.sent.length = 0;
  };
  const tick = () => new Promise((resolve) => setImmediate(resolve));
  return { ...f, api, ops, clear, tick };
}

const idsOf = (messages) => messages.map((m) => m.id).sort((a, b) => a - b);

test('every op the features send matches the contract', async (t) => {
  const { host, api, report, sent, tick } = await started(t);
  report(7);
  report(8, { dimension: 5 });
  const marker = api.Marker.place('materials/decals/x', { x: 1, y: 2, z: 3 }, { size: 4 });
  const blip = api.Blip.create({ position: { x: 1, y: 2, z: 3 }, label: 'camp' });
  api.World.setHour(5);
  api.World.setWeather('overcast_rain', 60);
  const prop = api.Prop.spawn('barrel_a', { x: 1, y: 2, z: 3 }, undefined, 2, 'rigid', 5);
  // Player 8 (world 5) simulates it and takes its pushes.
  host.props.tick();
  prop.applyImpulse({ x: 0, y: 0, z: 10 }, { x: 1, y: 2, z: 3.5 });
  prop.sleep();
  prop.physics = 'static';
  prop.position = { x: 2, y: 2, z: 3 };
  // A recipe a resource registered: its table patch for every client.
  api.Crafting.defineRecipe({
    kind: 'smithing',
    id: 'contract_shoe',
    name: 'Contract shoe',
    category: 'Other',
    workpiece: 'horseshoe',
    ingredients: [{ item: '94d8f5f1-20a7-4840-98ad-1d198d381389', amount: 1 }],
    products: [{ item: '549ab26e-df73-43d6-ac9b-f4f74afec67f', threshold: 40 }],
  });
  // And an alchemy recipe with the game's book pictures of recipe 1, each picture an op.
  api.Crafting.defineRecipe({
    kind: 'alchemy',
    id: 910900,
    name: 'Contract tonic',
    base: 'Water',
    bookPicturesFrom: 1,
    ingredients: [{ item: 'b9de1d84-a0c1-4b81-9f60-8d7fbb3cb9d4', amount: 1 }],
    products: [{ item: '7a1e8447-4449-473b-aac9-63f7a324fa0b', threshold: 1, maxYield: 1 }],
    steps: [{ id: 1, type: 'Trivial', condition: 'PotionBase()' }],
  });
  const vfx = api.Vfx.spawn(MAGIC, { x: 8, y: 8, z: 8 }, { prime: true }, 5);
  vfx.scale = 2;
  const light = api.Light.spawn({ x: 8, y: 8, z: 9 }, { castShadows: true });
  light.radius = 9;
  host.doors.report(host.players.get(7), [DOOR]);
  api.Door.find(DOOR.guid).setOpen(true);
  // A smart object a script places, and takes down.
  api.SmartObject.spawn({ blueprint: 'Bench/bench_2places_low', position: [1, 2, 3] }).destroy();
  // A door a script hangs, and takes down.
  api.Door.spawn({ model: 'lvl1_door_a_left', position: [1, 2, 3] }).destroy();
  host.doors.closestTo(host.players.get(7));
  api.World.raycast(host.players.get(7), { x: 0, y: 0, z: 10 }, { x: 0, y: 0, z: -10 });
  api.World.traceAll(host.players.get(7), { x: 0, y: 0, z: 10 }, { x: 0, y: 0, z: -10 });
  api.World.snapToGround(host.players.get(7), { x: 3, y: 4 });
  api.World.entitiesAround(host.players.get(7), { x: 0, y: 0, z: 0 }, 20);
  api.World.levelEntitiesAround(host.players.get(7), { x: 0, y: 0, z: 0 }, 10, { max: 4 });
  api.Camera.startShot(host.players.get(7), {
    position: { x: 1, y: 2, z: 3 },
    lookAt: { x: 5, y: 5, z: 3 },
    fov: 60,
    seconds: 2,
  });
  api.Camera.endShot(host.players.get(7));
  api.Vfx.burst(MAGIC, { x: 0, y: 0, z: 0 }, {}, GLOBAL_WORLD);
  api.Vfx.burstFrom(MAGIC, host.players.get(7), { offset: { x: 0, y: 0, z: 1 } });
  const engine = api.SiegeEngine.spawn('cannon', { x: 0, y: 0, z: 0 }, undefined, 5);
  engine.speed = 20;
  engine.load();
  await new Promise((resolve) => setTimeout(resolve, 750));
  assert.equal(engine.fire({ x: 50, y: 0, z: 0 }).accepted, true);
  await new Promise((resolve) => setTimeout(resolve, 150));
  engine.destroy();
  const ladder = api.SiegeLadder.spawn('siege', { x: 1, y: 2, z: 3 });
  ladder.push(7);
  ladder.usable = false;
  host.siegeLadders.use(host.players.get(7), ladder.id, 'push');
  ladder.destroy();
  const pile = api.StonePile.spawn({ x: 1, y: 2, z: 3 });
  pile.stones = 3;
  pile.destroy();
  marker.destroy();
  blip.destroy();
  prop.destroy();
  vfx.destroy();
  light.destroy();
  await tick();
  const seen = new Set();
  for (const message of sent) {
    if (!(message.op in CONTRACT)) continue;
    checkContract(message);
    seen.add(message.op);
  }
  assert.deepEqual(
    Object.keys(CONTRACT).filter((op) => !seen.has(op)),
    [],
  );
});

test('markers and blips go to ready players in their world, once, and again on world-ready', async (t) => {
  const { api, report, ops, clear } = await started(t);
  report(7, { dimension: 5 });
  report(8, { dimension: 6 });
  report(9, { ready: false });
  const marker = api.Marker.place('m', undefined, undefined, 5);
  const blip = api.Blip.create({ position: { x: 0, y: 0, z: 0 }, dimension: 5 });
  assert.deepEqual(idsOf(ops('markerShow')), [7]);
  assert.deepEqual(idsOf(ops('blipShow')), [7]);
  // Nothing is sent while nothing changes.
  clear();
  report(7, { dimension: 5 });
  assert.deepEqual(ops('markerShow', 'blipShow'), []);
  // The player's world got ready again: everything goes again.
  report(7, { dimension: 5, ready: false });
  report(7, { dimension: 5 });
  assert.equal(ops('markerShow').length, 1);
  assert.equal(ops('blipShow').length, 1);
  // Leaving the world hides both.
  clear();
  report(7, { dimension: 6 });
  assert.deepEqual(idsOf(ops('markerHide')), [7]);
  assert.deepEqual(idsOf(ops('blipHide')), [7]);
  marker.destroy();
  blip.destroy();
});

test('the world time and weather go to ready players, on change, and again on world-ready', async (t) => {
  const { api, report, ops, clear } = await started(t);
  report(7);
  report(9, { ready: false });
  assert.deepEqual(
    ops('worldTime', 'worldWeather').map((m) => m.op),
    ['worldTime', 'worldWeather'],
  );
  clear();
  assert.equal(api.World.setWeather('Not A Preset!'), false);
  assert.deepEqual(ops('worldTime', 'worldWeather'), []);
  api.World.setHour(3);
  assert.deepEqual(idsOf(ops('worldTime')), [7]);
  api.World.setWeather('overcast_rain', 30);
  assert.deepEqual(idsOf(ops('worldWeather')), [7]);
  clear();
  report(7, { ready: false });
  report(7);
  assert.deepEqual(
    ops('worldTime', 'worldWeather')
      .map((m) => m.op)
      .sort(),
    ['worldTime', 'worldWeather'],
  );
});

test('door state goes only to ready players in the door world, and only on change', async (t) => {
  const { host, api, report, ops, clear } = await started(t);
  const a = report(7, { dimension: 5 });
  report(8, { dimension: 5 });
  report(9, { dimension: 6 });
  report(10, { dimension: 5, ready: false });
  host.doors.report(a, [DOOR]);
  const door = api.Door.find(DOOR.guid, 5);
  clear();
  assert.equal(door.setOpen(true), true);
  assert.equal(door.setOpen(true), false);
  assert.deepEqual(idsOf(ops('doorSet')), [7, 8]);
  // A player's game reporting a door that differs is put right for them alone.
  clear();
  host.doors.report(host.players.get(8), [{ ...DOOR, open: false }]);
  assert.deepEqual(
    ops('doorSet').map((m) => [m.id, m.open]),
    [[8, true]],
  );
});

test('the nearest-door question needs a ready player and is answered by request number', async (t) => {
  const { host, report, ops } = await started(t);
  report(7);
  report(9, { ready: false });
  const asked = host.doors.closestTo(host.players.get(7));
  const request = ops('doorNearest')[0].request;
  host.doors.nearestReply(host.players.get(7), request, DOOR);
  assert.equal((await asked).found, true);
  assert.equal((await host.doors.closestTo(host.players.get(9))).found, false);
  assert.equal(ops('doorNearest').length, 1);
});

test('props and vfx name their dimension and are sent only on change', async (t) => {
  const { api, ops, clear } = await started(t);
  const prop = api.Prop.spawn('barrel_a', undefined, undefined, 1, 'rigid', 5);
  const vfx = api.Vfx.spawn(MAGIC, { x: 0, y: 0, z: 0 }, {}, 5);
  assert.equal(ops('propSpawn')[0].dimension, 5);
  assert.equal(ops('vfxSpawn')[0].dimension, 5);
  clear();
  prop.physics = prop.physics;
  vfx.scale = vfx.scale;
  assert.deepEqual(ops('propUpdate', 'vfxUpdate'), []);
  prop.physics = 'static';
  vfx.strength = 0.5;
  assert.equal(ops('propUpdate').length, 1);
  assert.equal(ops('vfxUpdate').length, 1);
  prop.destroy();
  prop.destroy();
  vfx.destroy();
  vfx.destroy();
  assert.equal(ops('propDestroy').length, 1);
  assert.equal(ops('vfxDestroy').length, 1);
});

test('a vfx burst lists only the players in its world and range, and nothing goes when empty', async (t) => {
  const { api, report, ops, clear } = await started(t);
  report(7, { dimension: 5, position: { x: 1, y: 0, z: 0 } });
  report(8, { dimension: 6, position: { x: 1, y: 0, z: 0 } });
  report(9, { dimension: 5, position: { x: 900, y: 0, z: 0 } });
  assert.equal(api.Vfx.burst(MAGIC, { x: 0, y: 0, z: 0 }, {}, 5), 1);
  assert.deepEqual(ops('vfxBurst')[0].players, [7]);
  clear();
  assert.equal(api.Vfx.burst(MAGIC, { x: 0, y: 0, z: 0 }, {}, 77), 0);
  assert.deepEqual(ops('vfxBurst'), []);
});

test('areas run on the server alone and send no op', async (t) => {
  const { api, report, sent, tick } = await started(t);
  report(7);
  sent.length = 0;
  api.Area.create({ id: 'a', type: 'sphere', position: { x: 0, y: 0, z: 0 }, radius: 5 });
  await tick();
  report(7, { position: { x: 100, y: 0, z: 0 } });
  await tick();
  assert.deepEqual(sent, []);
});

// Props, vfx and carts are broadcast ops, so a player whose world got ready after they were
// spawned is sent them again, with `to` naming the one player who hears it.
test("props, vfx and carts are sent again to a player's world-ready, and to nobody else", async (t) => {
  const { api, host, report, ops, clear } = await started(t);
  const rider = report(8);
  const prop = api.Prop.spawn('barrel_a', { x: 1, y: 2, z: 3 }, undefined, 2, 'rigid', 5);
  prop.physics = 'static';
  api.Vfx.spawn(MAGIC, { x: 0, y: 0, z: 0 }, { prime: true });
  api.Light.spawn({ x: 0, y: 0, z: 2 });
  const cart = api.Cart.spawn('cart_a');
  cart.seatPlayer(host.players.get(8), 'driver');
  assert.ok(rider);
  clear();
  report(7);
  assert.equal(ops('propSpawn').length, 1);
  assert.equal(ops('propSpawn')[0].to, 7);
  assert.equal(ops('propSpawn')[0].physics, 'static');
  assert.equal(ops('propSpawn')[0].dimension, 5);
  assert.equal(ops('vfxSpawn').length, 1);
  assert.equal(ops('vfxSpawn')[0].to, 7);
  assert.equal(ops('vfxSpawn')[0].prime, true);
  assert.equal(ops('lightSpawn').length, 1);
  assert.equal(ops('lightSpawn')[0].to, 7);
  assert.equal(ops('cartSpawn').length, 1);
  assert.equal(ops('cartSpawn')[0].to, 7);
  assert.deepEqual(
    ops('cartSeat').map((m) => [m.player, m.seat, m.to]),
    [[8, 'driver', 7]],
  );
  for (const message of [...ops('propSpawn'), ...ops('vfxSpawn'), ...ops('lightSpawn')]) {
    checkContract(message);
  }
  // Nothing goes again while the player stays ready.
  clear();
  report(7);
  assert.deepEqual(ops('propSpawn', 'vfxSpawn', 'lightSpawn', 'cartSpawn', 'cartSeat'), []);
});

// Siege engines, ladders and stone piles are broadcast too, so they are replayed the same way,
// with the state they have now.
test("siege engines, ladders and stone piles are sent again to a player's world-ready", async (t) => {
  const { api, report, ops, clear } = await started(t);
  const engine = api.SiegeEngine.spawn('cannon', { x: 1, y: 2, z: 3 }, undefined, 5);
  engine.usable = true;
  const ladder = api.SiegeLadder.spawn('siege', { x: 1, y: 2, z: 3 });
  ladder.usable = false;
  const pile = api.StonePile.spawn({ x: 4, y: 5, z: 6 });
  pile.stones = 3;
  clear();
  report(7);
  assert.equal(ops('siegeSpawn').length, 1);
  assert.equal(ops('siegeSpawn')[0].to, 7);
  assert.equal(ops('siegeSpawn')[0].usable, true);
  assert.equal(ops('siegeSpawn')[0].dimension, 5);
  assert.equal(ops('siegeLadderSpawn').length, 1);
  assert.equal(ops('siegeLadderSpawn')[0].to, 7);
  assert.equal(ops('siegeLadderSpawn')[0].usable, false);
  assert.equal(ops('stonePileSpawn').length, 1);
  assert.equal(ops('stonePileSpawn')[0].to, 7);
  assert.equal(ops('stonePileSpawn')[0].stones, 3);
  for (const message of ops('siegeSpawn', 'siegeLadderSpawn', 'stonePileSpawn')) {
    checkContract(message);
  }
  // Nothing goes again while the player stays ready.
  clear();
  report(7);
  assert.deepEqual(ops('siegeSpawn', 'siegeLadderSpawn', 'stonePileSpawn'), []);
});

test("a player's own previews and placement are sent again on their world-ready", async (t) => {
  const { api, report, ops, clear } = await started(t);
  const player = report(7);
  const other = report(8);
  const ghost = api.PropGhost.create(player, { model: 'barrel_a', position: { x: 4, y: 0, z: 0 } });
  ghost.setHidden(true);
  api.PropGhost.create(other, { model: 'barrel_a', position: { x: 0, y: 0, z: 0 } });
  api.PropPlacer.begin(player, { model: 'barrel_a' }).veto(true);
  clear();
  report(7, { ready: false });
  report(7);
  assert.deepEqual(
    ops('ghostCreate').map((m) => [m.id, m.hidden, m.position.x]),
    [[7, true, 4]],
  );
  assert.deepEqual(
    ops('placerBegin').map((m) => m.id),
    [7],
  );
  assert.deepEqual(
    ops('placerVeto').map((m) => [m.id, m.vetoed]),
    [[7, true]],
  );
});
