'use strict';

// World.raycast and Camera.startShot / endShot: the ops to one player's
// game, the answer by request number, and the ends that settle a ray or forget a shot.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

async function started(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const api = f.host.makeAPI();
  const ops = (...names) => f.sent.filter((m) => names.includes(m.op));
  return { ...f, api, ops };
}

const UP = { x: 0, y: 0, z: 10 };
const DOWN = { x: 0, y: 0, z: -10 };

test('a ray goes to the asked player and settles on the answer, with the body it hit', async (t) => {
  const { host, api, report, ops } = await started(t);
  report(7);
  report(8);
  const player = host.players.get(7);
  const asked = api.World.raycast(player, UP, DOWN, { mode: 'anything' });
  const [sent] = ops('worldRaycast');
  assert.deepEqual(sent, {
    op: 'worldRaycast',
    id: 7,
    request: sent.request,
    query: 0,
    start: UP,
    end: DOWN,
    mode: 1,
    max: 1,
  });
  host.worldTraces.reply(player, {
    request: sent.request,
    status: 'hit',
    hits: [
      {
        position: { x: 0, y: 0, z: 1 },
        normal: { x: 0, y: 0, z: 1 },
        distance: 9,
        terrain: false,
        entity: 8,
        surface: 'mat_wood',
      },
    ],
  });
  const result = await asked;
  assert.equal(result.answered, true);
  assert.equal(result.hit.distance, 9);
  assert.equal(result.hit.entity, host.players.get(8));
  assert.equal(result.hit.surface, 'mat_wood');
});

const hitAt = (z, entity = 0) => ({
  position: { x: 0, y: 0, z },
  normal: { x: 0, y: 0, z: 1 },
  distance: 10 - z,
  terrain: entity === 0,
  entity,
  surface: entity === 0 ? 'mat_grass' : '',
});

test('traceAll asks for every hit up to its cap and keeps them nearest first', async (t) => {
  const { host, api, report, ops } = await started(t);
  report(7);
  report(8);
  const player = host.players.get(7);
  const asked = api.World.traceAll(player, UP, DOWN, { mode: 'anything', max: 4 });
  const [sent] = ops('worldRaycast');
  assert.equal(sent.query, 1);
  assert.equal(sent.max, 4);
  host.worldTraces.reply(player, {
    request: sent.request,
    status: 'hit',
    hits: [hitAt(2, 8), hitAt(0)],
  });
  const result = await asked;
  assert.equal(result.hits.length, 2);
  assert.equal(result.hits[0].entity, host.players.get(8));
  assert.equal(result.hits[1].surface, 'mat_grass');
  api.World.traceAll(player, UP, DOWN);
  assert.equal(ops('worldRaycast')[1].max, 16);
  assert.throws(() => api.World.traceAll(player, UP, DOWN, { max: 17 }), RangeError);
  host.worldTraces.reply(player, { request: ops('worldRaycast')[1].request, status: 'miss' });
});

test('snapToGround without a player asks any ready one and keeps the answer', async (t) => {
  const { host, api, report, ops } = await started(t);
  // Nobody to ask yet.
  const nobody = await api.World.snapToGround(null, { x: 5, y: 6 });
  assert.deepEqual(nobody, { answered: false, reason: 'no player', z: null, hit: null });
  report(7);
  const player = host.players.get(7);
  const asked = api.World.snapToGround(null, { x: 5, y: 6 });
  const [sent] = ops('worldRaycast');
  assert.equal(sent.id, 7);
  host.worldTraces.reply(player, { request: sent.request, status: 'hit', hits: [hitAt(0)] });
  assert.equal((await asked).z, 0);
  // The same half-metre cell again: kept, nobody is asked.
  const again = await api.World.snapToGround(null, { x: 5.1, y: 6.1 });
  assert.equal(again.answered, true);
  assert.equal(again.reason, 'kept');
  assert.equal(again.z, 0);
  assert.equal(ops('worldRaycast').length, 1);
  // From a given height (under a bridge, in a cave) it is asked every time.
  api.World.snapToGround(null, { x: 5, y: 6, z: 30 });
  assert.equal(ops('worldRaycast').length, 2);
  // Another map's players are not asked for this one.
  const elsewhere = await api.World.snapToGround(null, { x: 9, y: 9 }, { map: 'kutnohorsko' });
  assert.equal(elsewhere.reason, 'no player');
  assert.throws(() => api.World.snapToGround(null, { x: 1, y: 1 }, { map: 3 }), TypeError);
});

test('snapToGround traces straight down under x / y and answers the height', async (t) => {
  const { host, api, report, ops } = await started(t);
  report(7);
  const player = host.players.get(7);
  const asked = api.World.snapToGround(player, { x: 5, y: 6 });
  const [sent] = ops('worldRaycast');
  assert.deepEqual(sent.start, { x: 5, y: 6, z: 4000 });
  assert.deepEqual(sent.end, { x: 5, y: 6, z: -96 });
  assert.equal(sent.mode, 2);
  host.worldTraces.reply(player, { request: sent.request, status: 'hit', hits: [hitAt(0)] });
  const result = await asked;
  assert.equal(result.z, 0);
  assert.equal(result.hit.terrain, true);
  const under = api.World.snapToGround(player, { x: 5, y: 6, z: 30 }, { mode: 'terrain' });
  assert.equal(ops('worldRaycast')[1].start.z, 30);
  assert.equal(ops('worldRaycast')[1].mode, 3);
  host.worldTraces.reply(player, { request: ops('worldRaycast')[1].request, status: 'miss' });
  assert.deepEqual(await under, { answered: true, reason: '', z: null, hit: null });
  assert.throws(() => api.World.snapToGround(player, { x: 1 }), TypeError);
  assert.throws(() => api.World.snapToGround(player, { x: 1, y: 1 }, { mode: 'cover' }), TypeError);
});

test('entitiesAround names the known bodies around a point, nearest first', async (t) => {
  const { host, api, report, ops } = await started(t);
  report(7);
  report(8);
  report(9);
  const player = host.players.get(7);
  const center = { x: 0, y: 0, z: 0 };
  const asked = api.World.entitiesAround(player, center, 20);
  const [sent] = ops('worldBodiesInRadius');
  assert.deepEqual(sent, {
    op: 'worldBodiesInRadius',
    id: 7,
    request: sent.request,
    center,
    radius: 20,
  });
  host.worldTraces.reply(player, {
    request: sent.request,
    status: 'hit',
    bodies: [
      { entity: 9, position: { x: 10, y: 0, z: 0 } },
      { entity: 4444, position: { x: 1, y: 0, z: 0 } },
      { entity: 8, position: { x: 3, y: 4, z: 0 } },
    ],
  });
  const result = await asked;
  assert.deepEqual(
    result.entities.map((found) => [found.entity, found.distance]),
    [
      [host.players.get(8), 5],
      [host.players.get(9), 10],
    ],
  );
  assert.throws(() => api.World.entitiesAround(player, center, 0), RangeError);
  assert.throws(() => api.World.entitiesAround(player, center, 600), RangeError);
});

// the level's own entities around a point, as a player's game has them.
test('levelEntitiesAround names the level entities around a point with their level object', async (t) => {
  const { host, api, report, ops } = await started(t);
  report(7);
  const player = host.players.get(7);
  const center = { x: 0, y: 0, z: 0 };
  const asked = api.World.levelEntitiesAround(player, center, 10, { max: 4 });
  const [sent] = ops('worldLevelEntities');
  assert.deepEqual(sent, {
    op: 'worldLevelEntities',
    id: 7,
    request: sent.request,
    center,
    radius: 10,
    max: 4,
  });
  const rack = {
    position: { x: 2, y: 0, z: 0 },
    normal: { x: 0, y: 0, z: 1 },
    distance: 2,
    terrain: false,
    entity: 0,
    surface: '',
    kind: 'entity',
    model: '',
    guid: '00ab00cd00ef0012',
    name: 'sausage_rack_1',
    cls: 'PickableItem',
    lo: {
      kind: 'entity',
      name: 'sausage_rack_1',
      guid: '00ab00cd00ef0012',
      cls: 'PickableItem',
      at: { x: 2, y: 0, z: 0 },
      rot: [0, 0, 0, 1],
      scale: { x: 1, y: 1, z: 1 },
    },
  };
  host.worldTraces.reply(player, { request: sent.request, status: 'hit', hits: [rack] });
  const result = await asked;
  assert.equal(result.answered, true);
  assert.equal(result.entities.length, 1);
  const [found] = result.entities;
  assert.equal(found.guid, '00ab00cd00ef0012');
  assert.equal(found.name, 'sausage_rack_1');
  assert.equal(found.class, 'PickableItem');
  assert.equal(found.distance, 2);
  assert.equal(found.levelObject.kind, 'entity');
  assert.equal(found.levelObject.guid, '00ab00cd00ef0012');
  assert.throws(() => api.World.levelEntitiesAround(player, center, 65), RangeError);
  assert.throws(() => api.World.levelEntitiesAround(player, center, 5, { max: 33 }), RangeError);
});

test('a miss, an unavailable trace and a leaving player each settle the ray', async (t) => {
  const { host, api, report, ops } = await started(t);
  report(7);
  const player = host.players.get(7);
  const missed = api.World.raycast(player, UP, DOWN);
  host.worldTraces.reply(player, { request: ops('worldRaycast')[0].request, status: 'miss' });
  assert.deepEqual(await missed, { answered: true, reason: '', hit: null });
  const unavailable = api.World.raycast(player, UP, DOWN, { mode: 'ground' });
  assert.equal(ops('worldRaycast')[1].mode, 2);
  host.worldTraces.reply(player, {
    request: ops('worldRaycast')[1].request,
    status: 'unavailable',
  });
  assert.equal((await unavailable).reason, 'unavailable');
  const leaving = api.World.raycast(player, UP, DOWN);
  host.worldTraces.left(player);
  assert.deepEqual(await leaving, { answered: false, reason: 'player left', hit: null });
});

test('a ray is refused when it is empty, too long or of an unknown mode, and a player not ready gets no op', async (t) => {
  const { host, api, report, ops } = await started(t);
  report(7);
  report(9, { ready: false });
  const player = host.players.get(7);
  assert.throws(() => api.World.raycast(player, UP, UP), RangeError);
  assert.throws(() => api.World.raycast(player, UP, { x: 0, y: 0, z: 5000 }), RangeError);
  assert.throws(() => api.World.raycast(player, UP, DOWN, { mode: 'all' }), TypeError);
  assert.throws(() => api.World.raycast(player, { x: 0 }, DOWN), TypeError);
  const result = await api.World.raycast(host.players.get(9), UP, DOWN);
  assert.equal(result.answered, false);
  assert.equal(ops('worldRaycast').length, 0);
});

test('an answer from another player or for an unknown request is ignored', async (t) => {
  const { host, api, report, ops } = await started(t);
  report(7);
  report(8);
  const asked = api.World.raycast(host.players.get(7), UP, DOWN);
  const request = ops('worldRaycast')[0].request;
  host.worldTraces.reply(host.players.get(8), { request, status: 'miss' });
  host.worldTraces.reply(host.players.get(7), { request: request + 1, status: 'miss' });
  assert.equal(host.worldTraces.pending.size, 1);
  host.worldTraces.reply(host.players.get(7), { request, status: 'miss' });
  assert.equal((await asked).answered, true);
});

test('a camera shot goes to its player only, aimed by a rotation or a point, and clears once', async (t) => {
  const { host, api, report, ops } = await started(t);
  report(7);
  report(8);
  const player = host.players.get(7);
  const rotation = { x: 0, y: 0, z: 0, w: 1 };
  assert.equal(api.Camera.startShot(player, { position: UP, rotation }), true);
  assert.deepEqual(ops('cameraShot')[0], {
    op: 'cameraShot',
    id: 7,
    position: UP,
    rotation,
    fov: 0,
    seconds: 0,
    ease: true,
  });
  // ease: false moves at one speed, so chained shots run on.
  api.Camera.startShot(player, { position: UP, rotation, seconds: 0.5, ease: false });
  assert.equal(ops('cameraShot').at(-1).ease, false);
  assert.equal(api.Camera.inShot(player), true);
  assert.equal(api.Camera.inShot(host.players.get(8)), false);
  api.Camera.startShot(player, { position: UP, lookAt: DOWN, roll: 15 });
  assert.equal(ops('cameraShot').at(-1).roll, 15);
  // look: the mouse turns the shot within its limits; a missing limit has its default.
  api.Camera.startShot(player, { position: UP, lookAt: DOWN, look: { yaw: 35, down: 25 } });
  const looked = ops('cameraShot').at(-1);
  assert.deepEqual([looked.lookYaw, looked.lookUp, looked.lookDown], [35, 15, 25]);
  assert.throws(
    () => api.Camera.startShot(player, { position: UP, lookAt: DOWN, look: { yaw: 0 } }),
    RangeError,
  );
  assert.throws(
    () => api.Camera.startShot(player, { position: UP, lookAt: DOWN, look: { left: 5 } }),
    TypeError,
  );
  assert.throws(() => api.Camera.startShot(player, { position: UP, rotation, roll: 5 }), TypeError);
  assert.throws(
    () => api.Camera.startShot(player, { position: UP, lookAt: DOWN, roll: 200 }),
    RangeError,
  );
  assert.equal(api.Camera.endShot(player), true);
  assert.equal(api.Camera.endShot(player), false);
  assert.deepEqual(ops('cameraClear'), [{ op: 'cameraClear', id: 7 }]);
});

test('a camera shot is refused when it cannot be filmed', async (t) => {
  const { host, api, report, ops } = await started(t);
  report(7);
  report(9, { ready: false });
  const player = host.players.get(7);
  assert.throws(() => api.Camera.startShot(player, { position: UP }), TypeError);
  assert.throws(() => api.Camera.startShot(player, { position: UP, lookAt: UP }), RangeError);
  assert.throws(
    () => api.Camera.startShot(player, { position: UP, lookAt: DOWN, fov: 200 }),
    RangeError,
  );
  assert.throws(
    () => api.Camera.startShot(player, { position: UP, lookAt: DOWN, seconds: -1 }),
    RangeError,
  );
  assert.throws(() => api.Camera.startShot({ id: 99 }, { position: UP, lookAt: DOWN }), TypeError);
  assert.equal(api.Camera.startShot(host.players.get(9), { position: UP, lookAt: DOWN }), false);
  assert.equal(ops('cameraShot').length, 0);
});

test('a shot the client ended is forgotten and raises cameraShotEnded', async (t) => {
  const { host, api, report } = await started(t);
  report(7);
  const player = host.players.get(7);
  const ends = [];
  host.on('cameraShotEnded', (who, reason) => ends.push([who.id, reason]));
  api.Camera.startShot(player, { position: UP, lookAt: DOWN });
  host.cameraShots.ended(player, 'worldChanged');
  assert.deepEqual(ends, [[7, 'worldChanged']]);
  assert.equal(api.Camera.inShot(player), false);
  host.cameraShots.ended(player, 'lost');
  assert.equal(ends.length, 1);
});

// What a server ray's hit is, as a client script's hit says it.
test('a hit says what it met and the level object a level edit finds it by', async (t) => {
  const { host, api, report, ops } = await started(t);
  report(7);
  const player = host.players.get(7);
  const asked = api.World.traceAll(player, UP, DOWN, { maxHits: 2 });
  host.worldTraces.reply(player, {
    request: ops('worldRaycast')[0].request,
    status: 'hit',
    hits: [
      {
        ...hitAt(5),
        terrain: false,
        kind: 'brush',
        model: 'Objects/natural/rocks/rock_a.cgf',
        guid: '',
        name: '',
        cls: '',
        lo: {
          kind: 'brush',
          name: 'Objects/natural/rocks/rock_a.cgf',
          guid: '',
          cls: '',
          at: { x: 1, y: 2, z: 3 },
          rot: [0, 0, 0, 1],
          scale: { x: 1, y: 1, z: 1 },
        },
      },
      {
        ...hitAt(1),
        terrain: false,
        kind: 'entity',
        model: '',
        guid: '108c8fabab6e4bf9',
        name: 'door_inn',
        cls: 'AnimDoor',
      },
    ],
  });
  const { hits } = await asked;
  const [rock, door] = hits;
  assert.equal(rock.kind, 'brush');
  assert.equal(rock.model, 'objects/natural/rocks/rock_a.cgf');
  assert.equal(rock.category, 'rock');
  assert.equal(rock.material, null);
  assert.equal(rock.entityGuid, null);
  assert.equal(rock.levelObject.kind, 'brush');
  assert.equal(rock.levelObject.name, 'objects/natural/rocks/rock_a.cgf');
  assert.deepEqual(rock.levelObject.at, { x: 1, y: 2, z: 3 });
  assert.equal(rock.levelObject.rotation.w, 1);
  assert.equal(door.kind, 'entity');
  assert.equal(door.entityGuid, '108c8fabab6e4bf9');
  assert.equal(door.entityName, 'door_inn');
  assert.equal(door.entityClass, 'AnimDoor');
  assert.equal(door.levelObject, null, 'the client named no object');
});

test('a player weapon hit raises playerWeaponHit with the hit as a trace names it', async (t) => {
  const { host, api, report } = await started(t);
  report(7);
  const player = host.players.get(7);
  const raised = [];
  api.Events.on('playerWeaponHit', (who, hit) => raised.push([who, hit]));
  host.worldTraces.weaponHit(player, {
    ...hitAt(1),
    distance: 1.2,
    terrain: false,
    kind: 'vegetation',
    model: 'Objects/natural/vegetation/trees/normal_trees/quercus_robur/quercus_robur_big_a.cgf',
    surface: 'mat_wood',
  });
  assert.equal(raised.length, 1);
  const [who, hit] = raised[0];
  assert.equal(who, player);
  assert.equal(hit.distance, 1.2);
  assert.equal(hit.kind, 'vegetation');
  assert.equal(hit.category, 'tree');
  assert.equal(hit.entity, null);
  assert.equal(hit.levelObject, null);
  // An unknown player or a missing hit raises nothing.
  host.worldTraces.weaponHit(undefined, hitAt(1));
  host.worldTraces.weaponHit(player, null);
  assert.equal(raised.length, 1);
});
