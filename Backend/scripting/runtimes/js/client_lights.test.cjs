'use strict';

// The client Light (Client/resources/lights.cjs): point lights a client script shows
// in this game only, with the server's checks.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceLights, FIRST_ID, MAX_LIGHTS } = require('../../../../Client/resources/lights.cjs');

function setup() {
  const sent = [];
  const lights = new ResourceLights({ send: (message) => sent.push(message) });
  return { lights, sent, Light: lights.facade('torches'), Other: lights.facade('other') };
}

test('spawn shows a light with a local id; changes go down; destroy and the stop remove it', () => {
  const { lights, sent, Light, Other } = setup();
  const lamp = Light.spawn({ x: 1, y: 2, z: 3 }, { radius: 8 });
  assert.equal(lamp.id, FIRST_ID);
  assert.equal(lamp.valid, true);
  assert.deepEqual(sent[0], {
    op: 'lightLocal',
    kind: 'spawn',
    id: FIRST_ID,
    position: { x: 1, y: 2, z: 3 },
    color: { x: 1, y: 0.6, z: 0.3 },
    intensity: 1,
    radius: 8,
    castShadows: false,
    ambient: false,
    body: 0,
    joint: '',
  });
  lamp.intensity = 3;
  assert.equal(sent.at(-1).kind, 'update');
  assert.equal(sent.at(-1).intensity, 3);
  assert.equal(lamp.intensity, 3);
  assert.throws(() => {
    lamp.radius = 0;
  }, RangeError);
  assert.throws(() => new Light(), TypeError);

  lamp.destroy();
  lamp.destroy();
  assert.equal(lamp.valid, false);
  assert.equal(lamp.intensity, 3, 'a gone light still reads its last values');
  assert.deepEqual(sent.at(-1), { op: 'lightLocal', kind: 'destroy', id: FIRST_ID });
  const count = sent.length;
  lamp.intensity = 5;
  assert.equal(sent.length, count, 'a gone light sends nothing');

  const kept = Other.spawn({ x: 0, y: 0, z: 0 });
  const owned = Light.spawn({ x: 0, y: 0, z: 0 });
  lights.removeOwner('torches');
  assert.equal(owned.valid, false);
  assert.equal(kept.valid, true);
});

test('a room light (ambient) goes down as one, keeps being one, and takes only true or false', () => {
  const { sent, Light } = setup();
  const room = Light.spawn({ x: 0, y: 0, z: 5 }, { radius: 20, ambient: true });
  assert.equal(sent[0].ambient, true);
  assert.equal(room.ambient, true);
  room.intensity = 2;
  assert.equal(sent.at(-1).ambient, true);
  assert.equal(Light.spawn({ x: 0, y: 0, z: 5 }).ambient, false);
  assert.throws(
    () => Light.spawn({ x: 0, y: 0, z: 5 }, { ambient: 1 }),
    /ambient must be true or false/,
  );
});

test('lights the game dropped turn invalid without a destroy going down', () => {
  const { lights, sent, Light } = setup();
  const lamp = Light.spawn({ x: 0, y: 0, z: 0 });
  const count = sent.length;
  lights.receiveGone({ ids: [lamp.id, 12345] });
  assert.equal(lamp.valid, false);
  assert.equal(sent.length, count);
});

test(`a game shows at most ${MAX_LIGHTS} script lights`, () => {
  const { Light } = setup();
  for (let i = 0; i < MAX_LIGHTS; i += 1) {
    Light.spawn({ x: i, y: 0, z: 0 });
  }
  assert.throws(() => Light.spawn({ x: 0, y: 0, z: 0 }), RangeError);
});
