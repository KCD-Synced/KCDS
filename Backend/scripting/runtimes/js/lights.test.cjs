'use strict';

// The server's Light (lights/lights.cjs): point lights every player's game shows, a
// player whose world gets ready later too, until destroyed or their resource stops.
const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { OWNER } = require('./lifecycle/owned_objects.cjs');

async function started(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const api = f.host.makeAPI();
  const ops = (...names) => f.sent.filter((m) => names.includes(m.op));
  const clear = () => {
    f.sent.length = 0;
  };
  return { ...f, api, ops, clear };
}

test('spawn shows a warm light with defaults; changes and destroy reach every game', async (t) => {
  const { api, ops, clear } = await started(t);
  const light = api.Light.spawn({ x: 1, y: 2, z: 3 });
  assert.deepEqual(ops('lightSpawn'), [
    {
      op: 'lightSpawn',
      id: light.id,
      position: { x: 1, y: 2, z: 3 },
      color: { x: 1, y: 0.6, z: 0.3 },
      intensity: 1,
      radius: 5,
      castShadows: false,
      body: 0,
      joint: '',
    },
  ]);
  assert.ok(light instanceof api.Entity);
  assert.equal(new api.Light(light.id), light);
  assert.equal(new api.Entity(light.id), light);
  assert.deepEqual(light.position, new api.Vector3(1, 2, 3));
  assert.equal(light.radius, 5);

  clear();
  light.intensity = 4;
  light.color = { x: 0.2, y: 0.4, z: 1 };
  light.castShadows = true;
  light.position = new api.Vector3(1, 2, 4);
  light.intensity = 4;
  const updates = ops('lightUpdate');
  assert.equal(updates.length, 4, 'an unchanged value sends nothing');
  assert.deepEqual(updates.at(-1), {
    op: 'lightUpdate',
    id: light.id,
    position: { x: 1, y: 2, z: 4 },
    color: { x: 0.2, y: 0.4, z: 1 },
    intensity: 4,
    radius: 5,
    castShadows: true,
    body: 0,
    joint: '',
  });

  clear();
  light.destroy();
  light.destroy();
  assert.deepEqual(ops('lightDestroy'), [{ op: 'lightDestroy', id: light.id }]);
  assert.throws(() => new api.Light(light.id), /Unknown light/);
});

test('bad values throw before anything changes', async (t) => {
  const { api, ops, clear } = await started(t);
  assert.throws(() => api.Light.spawn(), TypeError);
  assert.throws(() => api.Light.spawn({ x: 1, y: 2 }), TypeError);
  assert.throws(() => api.Light.spawn({ x: 1e6, y: 0, z: 0 }), RangeError);
  assert.throws(() => api.Light.spawn({ x: 0, y: 0, z: 0 }, { radius: 0.05 }), RangeError);
  assert.throws(() => api.Light.spawn({ x: 0, y: 0, z: 0 }, { intensity: 101 }), RangeError);
  assert.throws(
    () => api.Light.spawn({ x: 0, y: 0, z: 0 }, { color: { x: 2, y: 0, z: 0 } }),
    RangeError,
  );
  assert.throws(() => api.Light.spawn({ x: 0, y: 0, z: 0 }, { castShadows: 1 }), TypeError);
  // On a body: the position is the offset, the joint needs an entity.
  assert.throws(() => api.Light.spawn({ x: 0, y: 0, z: 0 }, { joint: 'head' }), TypeError);
  assert.throws(() => api.Light.spawn({ x: 0, y: 0, z: 0 }, { entity: 5 }), TypeError);
  assert.throws(
    () => api.Light.spawn({ x: 9, y: 0, z: 0 }, { entity: { id: 5 }, joint: 'head' }),
    RangeError,
  );
  assert.throws(() => api.Light.spawn({ x: 0, y: 0, z: 0 }, 'red'), TypeError);
  assert.deepEqual(ops('lightSpawn'), []);

  const light = api.Light.spawn({ x: 0, y: 0, z: 0 });
  clear();
  assert.throws(() => {
    light.radius = 101;
  }, RangeError);
  assert.throws(() => {
    light.castShadows = 'yes';
  }, TypeError);
  assert.throws(() => {
    light.position = { x: NaN, y: 0, z: 0 };
  }, TypeError);
  assert.equal(light.radius, 5);
  assert.deepEqual(ops('lightUpdate'), []);
});

test('a player whose world gets ready is told of every light, and only they hear it', async (t) => {
  const { api, report, ops, clear } = await started(t);
  report(8);
  const light = api.Light.spawn({ x: 5, y: 5, z: 5 }, { radius: 12 });
  clear();
  report(7);
  assert.equal(ops('lightSpawn').length, 1);
  assert.equal(ops('lightSpawn')[0].to, 7);
  assert.equal(ops('lightSpawn')[0].id, light.id);
  assert.equal(ops('lightSpawn')[0].radius, 12);
  clear();
  report(7);
  assert.deepEqual(ops('lightSpawn'), []);
});

test("a resource's lights go when it stops; a world export's lights stay", async (t) => {
  const { host, api, ops, clear } = await started(t);
  const stays = api.Light.spawn({ x: 0, y: 0, z: 0 });
  // As if the resource "lanterns" had spawned it.
  const owned = api.Light.spawn({ x: 1, y: 0, z: 0 });
  owned[OWNER] = 'lanterns';
  clear();
  host.lights.resourceStopped('lanterns');
  assert.deepEqual(ops('lightDestroy'), [{ op: 'lightDestroy', id: owned.id }]);
  assert.equal(host.lights.lights.get(stays.id), stays);
});
