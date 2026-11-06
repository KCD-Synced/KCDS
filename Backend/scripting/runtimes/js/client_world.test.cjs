'use strict';

// The client World and the camera's pose (Client/resources/world.cjs and camera.cjs):
// each question is one synchronous request to the game client, checked before it goes, and its
// answer turned into the hits, entities and poses the scripts read.
const test = require('node:test');
const assert = require('node:assert/strict');
const { Vector3 } = require('./math.cjs');
const { ResourceWorld, categoryOf } = require('../../../../Client/resources/world.cjs');
const { ResourceCamera } = require('../../../../Client/resources/camera.cjs');

// A SyncChannel whose game answers with `answer(request)`.
function fakeSync(answer) {
  const asked = [];
  return {
    asked,
    call(request) {
      asked.push(request);
      return answer(request);
    },
  };
}

const doorHit = {
  p: [10, 0, 1],
  n: [-1, 0, 0],
  d: 10,
  terrain: false,
  surface: 'mat_wood',
  kind: 'entity',
  model: '',
  entity: 4242,
  cls: 'AnimDoor',
  name: 'door_inn',
  guid: '108c8fabab6e4bf9',
};

const treeHit = {
  p: [20, 0, 1],
  n: [-1, 0, 0],
  d: 20,
  terrain: false,
  surface: 'mat_wood',
  kind: 'vegetation',
  model: 'Objects/natural/vegetation/trees/normal_trees/quercus_robur/quercus_robur_big_a.cgf',
  entity: 0,
  cls: '',
  name: '',
  guid: '',
};

test('raycast sends the checked ray and reads the first hit', () => {
  const sync = fakeSync(() => ({ status: 'hit', hits: [doorHit] }));
  const world = new ResourceWorld({
    sync,
    Vector3,
    playerOf: (engineId) => (engineId === 4242 ? 'the player' : null),
  });
  const World = world.facade('owner');
  const hit = World.raycast({ x: 0, y: 0, z: 1 }, { x: 50, y: 0, z: 1 }, { mode: 'anything' });
  assert.deepEqual(sync.asked, [
    {
      op: 'localWorldRay',
      from: { x: 0, y: 0, z: 1 },
      to: { x: 50, y: 0, z: 1 },
      mode: 1,
      max: 1,
      ignoreSelf: true,
    },
  ]);
  assert.ok(hit.position instanceof Vector3);
  assert.deepEqual(hit.position.toJSON(), { x: 10, y: 0, z: 1 });
  assert.equal(hit.distance, 10);
  assert.equal(hit.kind, 'entity');
  assert.equal(hit.entityClass, 'AnimDoor');
  assert.equal(hit.entityName, 'door_inn');
  assert.equal(hit.entityGuid, '108c8fabab6e4bf9');
  assert.equal(hit.entityId, 4242);
  assert.equal(hit.player, 'the player');
  assert.equal(hit.model, null);
  assert.equal(hit.category, null);
  assert.equal(hit.material, null);
});

test('traceAll reads every hit; a tree is told by its model', () => {
  const sync = fakeSync(() => ({ status: 'hit', hits: [doorHit, treeHit] }));
  const World = new ResourceWorld({ sync, Vector3 }).facade('owner');
  const hits = World.traceAll({ x: 0, y: 0, z: 1 }, { x: 50, y: 0, z: 1 }, { maxHits: 4 });
  assert.equal(sync.asked[0].max, 4);
  assert.equal(sync.asked[0].mode, 0);
  assert.equal(hits.length, 2);
  assert.equal(hits[1].kind, 'vegetation');
  assert.equal(hits[1].category, 'tree');
  // In one form, lower case.
  assert.equal(
    hits[1].model,
    'objects/natural/vegetation/trees/normal_trees/quercus_robur/quercus_robur_big_a.cgf',
  );
  assert.equal(hits[1].entityGuid, null);
  assert.equal(hits[1].entityClass, null);
  assert.equal(hits[1].player, null);
});

test('a ray the game cannot answer is null or empty', () => {
  const World = new ResourceWorld({ sync: fakeSync(() => undefined), Vector3 }).facade('o');
  assert.equal(World.raycast({ x: 0, y: 0, z: 0 }, { x: 1, y: 0, z: 0 }), null);
  assert.deepEqual(World.traceAll({ x: 0, y: 0, z: 0 }, { x: 1, y: 0, z: 0 }), []);
  const missed = new ResourceWorld({
    sync: fakeSync(() => ({ status: 'miss', hits: [] })),
    Vector3,
  }).facade('o');
  assert.equal(missed.raycast({ x: 0, y: 0, z: 0 }, { x: 1, y: 0, z: 0 }), null);
});

test('the rays refuse what the game would not trace', () => {
  const sync = fakeSync(() => ({ status: 'miss', hits: [] }));
  const World = new ResourceWorld({ sync, Vector3 }).facade('o');
  const origin = { x: 0, y: 0, z: 0 };
  const east = { x: 1, y: 0, z: 0 };
  assert.throws(() => World.raycast(origin, origin), RangeError);
  assert.throws(() => World.raycast(origin, { x: 5000, y: 0, z: 0 }), RangeError);
  assert.throws(() => World.raycast(origin, { x: 1, y: NaN, z: 0 }), TypeError);
  assert.throws(() => World.raycast(origin, east, { mode: 'terrain' }), TypeError);
  assert.throws(() => World.raycast(origin, east, { ignoreSelf: 1 }), TypeError);
  assert.throws(() => World.traceAll(origin, east, { maxHits: 9 }), RangeError);
  assert.deepEqual(sync.asked, []);
});

test('the ground under a point is a ground trace from above it', () => {
  const terrain = {
    ...treeHit,
    kind: 'terrain',
    terrain: true,
    model: '',
    p: [3, 4, 98.5],
    d: 6.5,
  };
  const sync = fakeSync(() => ({ status: 'hit', hits: [terrain] }));
  const World = new ResourceWorld({ sync, Vector3 }).facade('o');
  assert.equal(World.groundHeight({ x: 3, y: 4, z: 100 }), 98.5);
  assert.deepEqual(sync.asked[0], {
    op: 'localWorldRay',
    from: { x: 3, y: 4, z: 105 },
    to: { x: 3, y: 4, z: -100 },
    mode: 2,
    max: 1,
    ignoreSelf: true,
  });
  const ground = World.snapToGround({ x: 3, y: 4, z: 100 }, { up: 1, down: 10 });
  assert.equal(ground.terrain, true);
  assert.equal(sync.asked[1].from.z, 101);
  assert.equal(sync.asked[1].to.z, 90);
  assert.throws(() => World.groundHeight({ x: 0, y: 0, z: 0 }, { down: 600 }), RangeError);
});

test('entitiesAround sends the sphere and reads the entities', () => {
  const sync = fakeSync(() => [
    {
      entity: 7,
      p: [1, 2, 3],
      d: 1.5,
      cls: 'AnimDoor',
      name: 'door',
      guid: '00aa',
      physical: true,
    },
    { entity: 8, p: [2, 2, 3], d: 2.5, cls: 'BasicEntity', name: '', guid: '', physical: false },
  ]);
  const World = new ResourceWorld({ sync, Vector3 }).facade('o');
  const found = World.entitiesAround({ x: 0, y: 0, z: 0 }, 10, { class: 'AnimDoor', max: 5 });
  assert.deepEqual(sync.asked[0], {
    op: 'localWorldEntities',
    center: { x: 0, y: 0, z: 0 },
    radius: 10,
    cls: 'AnimDoor',
    max: 5,
    physicalOnly: false,
  });
  assert.equal(found.length, 2);
  assert.equal(found[0].class, 'AnimDoor');
  assert.equal(found[0].guid, '00aa');
  assert.equal(found[0].physicalized, true);
  assert.equal(found[1].guid, null);
  assert.equal(found[1].entityId, 8);
  assert.throws(() => World.entitiesAround({ x: 0, y: 0, z: 0 }, 300), RangeError);
  assert.throws(() => World.entitiesAround({ x: 0, y: 0, z: 0 }, 5, { max: 65 }), RangeError);
});

test('categories follow the game folders of natural objects', () => {
  assert.equal(categoryOf('objects/natural/vegetation/bushes/a.cgf'), 'bush');
  assert.equal(categoryOf('objects/natural/vegetation/grass/a.cgf'), 'plant');
  assert.equal(categoryOf('Objects/Natural/Rocks/cliff.cgf'), 'rock');
  assert.equal(categoryOf('objects/natural/stones/stone.cgf'), 'rock');
  assert.equal(categoryOf('objects/manmade/house.cgf'), null);
  assert.equal(categoryOf(null), null);
});

// A camera at (1, 2, 3) looking along +y, right +x, up +z, 60 degrees high.
const lookingNorth = { m: [1, 0, 0, 1, 0, 1, 0, 2, 0, 0, 1, 3], fov: Math.PI / 3 };

function cameraWith(answer, screen = { width: 1600, height: 900 }) {
  const sync = fakeSync(answer);
  const camera = new ResourceCamera({
    send: () => {},
    emit: () => {},
    sync,
    screenSize: () => screen,
    Vector3,
  });
  return { Camera: camera.facade(), sync };
}

test('getPose reads the view camera', () => {
  const { Camera, sync } = cameraWith(() => lookingNorth);
  const pose = Camera.getPose();
  assert.deepEqual(sync.asked, [{ op: 'localWorldCamera' }]);
  assert.deepEqual(pose.position.toJSON(), { x: 1, y: 2, z: 3 });
  assert.deepEqual(pose.forward.toJSON(), { x: 0, y: 1, z: 0 });
  assert.deepEqual(pose.right.toJSON(), { x: 1, y: 0, z: 0 });
  assert.deepEqual(pose.up.toJSON(), { x: 0, y: 0, z: 1 });
  assert.ok(Math.abs(pose.fov - 60) < 1e-9);
  assert.equal(pose.aspectRatio, 1600 / 900);
  assert.equal(cameraWith(() => null).Camera.getPose(), null);
});

test('rayFromScreen goes through the point of the screen asked for', () => {
  const { Camera } = cameraWith(() => lookingNorth, { width: 1000, height: 1000 });
  const centre = Camera.rayFromScreen();
  assert.deepEqual(centre.origin.toJSON(), { x: 1, y: 2, z: 3 });
  assert.deepEqual(centre.direction.toJSON(), { x: 0, y: 1, z: 0 });
  assert.deepEqual(centre.target.toJSON(), { x: 1, y: 102, z: 3 });
  // The top edge of a 60 degree view is 30 degrees up.
  const top = Camera.rayFromScreen({ y: 1, range: 10 });
  assert.ok(Math.abs(Math.atan2(top.direction.z, top.direction.y) - Math.PI / 6) < 1e-9);
  assert.throws(() => Camera.rayFromScreen({ x: 2 }), RangeError);
  assert.throws(() => Camera.rayFromScreen({ range: 0 }), RangeError);
  assert.equal(cameraWith(() => undefined).Camera.rayFromScreen(), null);
});

// WorldRayHit.levelObject: what LevelEdit.apply needs to find the hit object again.
test('a hit names the level object a level edit keys: a brush or an entity', () => {
  const { Quaternion } = require('./math.cjs');
  const brushHit = {
    ...treeHit,
    kind: 'brush',
    model: 'Objects/manmade/sheds/shed_g.cgf',
    lo: {
      kind: 'brush',
      name: 'Objects/manmade/sheds/shed_g.cgf',
      guid: '',
      cls: '',
      at: [10, 20, 30],
      rot: [0, 0, 0, 1],
      scale: [1, 1, 2],
    },
  };
  const doorWithObject = {
    ...doorHit,
    lo: {
      kind: 'entity',
      name: 'door_inn',
      guid: '108c8fabab6e4bf9',
      cls: 'AnimDoor',
      at: [10, 0, 1],
      rot: [0, 0, 0.7071, 0.7071],
      scale: [1, 1, 1],
    },
  };
  const sync = fakeSync(() => ({ status: 'hit', hits: [brushHit, doorWithObject, treeHit] }));
  const World = new ResourceWorld({ sync, Vector3, Quaternion }).facade('owner');
  const [brush, door, tree] = World.traceAll(
    { x: 0, y: 0, z: 1 },
    { x: 50, y: 0, z: 1 },
    {
      maxHits: 3,
    },
  );
  assert.equal(brush.levelObject.kind, 'brush');
  assert.equal(brush.levelObject.name, 'objects/manmade/sheds/shed_g.cgf');
  assert.equal(brush.levelObject.guid, undefined);
  assert.deepEqual(brush.levelObject.at.toJSON(), { x: 10, y: 20, z: 30 });
  assert.deepEqual(brush.levelObject.scale.toJSON(), { x: 1, y: 1, z: 2 });
  assert.ok(brush.levelObject.rotation instanceof Quaternion);
  assert.equal(door.levelObject.kind, 'entity');
  assert.equal(door.levelObject.guid, '108c8fabab6e4bf9');
  assert.equal(door.levelObject.class, 'AnimDoor');
  assert.ok(Math.abs(door.levelObject.rotation.w - 0.7071) < 1e-6);
  assert.equal(tree.levelObject, null, 'a vegetation instance names none yet');
});

test('a weapon hit raises weaponHit with the hit as a trace names it', () => {
  const raised = [];
  const world = new ResourceWorld({
    sync: fakeSync(() => null),
    Vector3,
    emit: (name, args) => raised.push([name, args]),
  });
  world.receiveWeaponHit({ op: 'weaponHit', hit: { ...treeHit, d: 1.4 } });
  assert.equal(raised.length, 1);
  const [name, [hit]] = raised[0];
  assert.equal(name, 'weaponHit');
  assert.deepEqual(hit.position.toJSON(), { x: 20, y: 0, z: 1 });
  assert.equal(hit.distance, 1.4);
  assert.equal(hit.kind, 'vegetation');
  assert.equal(hit.category, 'tree');
  assert.equal(hit.surface, 'mat_wood');
  assert.equal(hit.player, null);
  // Something that is not a hit raises nothing.
  world.receiveWeaponHit({ op: 'weaponHit', hit: null });
  world.receiveWeaponHit({ op: 'weaponHit', hit: { p: [1, 2] } });
  assert.equal(raised.length, 1);
});
