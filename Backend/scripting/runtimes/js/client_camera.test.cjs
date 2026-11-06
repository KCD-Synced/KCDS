'use strict';

// The client Camera (Client/resources/camera.cjs): a client script's shot goes to the
// game client as cameraShotLocal with the server's checks; its own end comes back as an event.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceCamera } = require('../../../../Client/resources/camera.cjs');

function setup() {
  const sent = [];
  const emitted = [];
  const camera = new ResourceCamera({
    send: (message) => sent.push(message),
    emit: (name, args) => emitted.push([name, args]),
  });
  return { Camera: camera.facade(), camera, sent, emitted };
}

test('startShot sends the checked shot; endShot clears only an active one', () => {
  const { Camera, sent } = setup();
  assert.equal(Camera.inShot(), false);
  assert.equal(Camera.endShot(), false);
  assert.equal(
    Camera.startShot({ position: { x: 1, y: 2, z: 3 }, lookAt: { x: 1, y: 5, z: 3 }, seconds: 2 }),
    true,
  );
  assert.equal(Camera.inShot(), true);
  assert.deepEqual(sent, [
    {
      op: 'cameraShotLocal',
      position: { x: 1, y: 2, z: 3 },
      lookAt: { x: 1, y: 5, z: 3 },
      roll: 0,
      fov: 0,
      seconds: 2,
      ease: true,
    },
  ]);
  assert.equal(Camera.endShot(), true);
  assert.deepEqual(sent.at(-1), { op: 'cameraClearLocal' });
  assert.equal(Camera.inShot(), false);
});

test('startShot refuses what the server refuses', () => {
  const { Camera, sent } = setup();
  assert.throws(() => Camera.startShot({ position: { x: 1, y: 2, z: 3 } }), /rotation or lookAt/);
  assert.throws(
    () =>
      Camera.startShot({ position: { x: 1, y: 2, z: 3 }, lookAt: { x: 1, y: 3, z: 3 }, fov: 3 }),
    /fov/,
  );
  assert.throws(
    () =>
      Camera.startShot({ position: { x: 1, y: 2, z: 3 }, lookAt: { x: 1, y: 3, z: 3 }, ease: 1 }),
    /ease/,
  );
  assert.deepEqual(sent, []);
  assert.equal(Camera.inShot(), false);
});

test('a shot that ended by itself raises cameraShotEnded once', () => {
  const { Camera, camera, emitted } = setup();
  Camera.startShot({ position: { x: 0, y: 0, z: 0 }, rotation: { x: 0, y: 0, z: 0, w: 1 } });
  camera.receiveEnded({ reason: 'worldChanged' });
  camera.receiveEnded({ reason: 'worldChanged' });
  assert.deepEqual(emitted, [['cameraShotEnded', ['worldChanged']]]);
  assert.equal(Camera.inShot(), false);
});

test('Camera.setThirdPerson: on with its seat, off with null, bad values refused', () => {
  const { ResourceCamera } = require('../../../../Client/resources/camera.cjs');
  const sent = [];
  const camera = new ResourceCamera({
    send: (message) => sent.push(message),
    emit() {},
    Vector3: class {},
  }).facade();
  assert.equal(camera.isThirdPerson(), false);
  camera.setThirdPerson();
  assert.equal(camera.isThirdPerson(), true);
  assert.deepEqual(sent.at(-1), {
    op: 'cameraThirdPerson',
    on: true,
    distance: 2.4,
    height: 1.6,
    shoulder: 0.45,
  });
  camera.setThirdPerson({ distance: 5 });
  assert.equal(sent.at(-1).distance, 5);
  camera.setThirdPerson(null);
  assert.equal(camera.isThirdPerson(), false);
  assert.deepEqual(sent.at(-1), { op: 'cameraThirdPerson', on: false });
  const before = sent.length;
  assert.throws(() => camera.setThirdPerson({ distance: 21 }), RangeError);
  assert.throws(() => camera.setThirdPerson({ height: NaN }), RangeError);
  assert.throws(() => camera.setThirdPerson(3), TypeError);
  assert.equal(sent.length, before);
  assert.equal(camera.isThirdPerson(), false);
});

// a camera path the game client plays every frame.
test('playPath sends the checked keys and raises cameraPathFinished at its end', () => {
  const { Camera, sent, emitted, camera } = setup();
  const keys = [
    { position: { x: 0, y: 0, z: 10 }, lookAt: { x: 0, y: 20, z: 0 }, time: 0 },
    { position: { x: 10, y: 0, z: 12 }, lookAt: { x: 0, y: 20, z: 0 }, time: 4, fov: 50 },
    { position: { x: 20, y: 5, z: 12 }, rotation: { x: 0, y: 0, z: 0, w: 1 }, time: 9 },
  ];
  assert.equal(Camera.playPath(keys, { smooth: true }), true);
  assert.equal(Camera.inShot(), true);
  const [message] = sent;
  assert.equal(message.op, 'cameraPathLocal');
  assert.equal(message.smooth, true);
  assert.deepEqual(
    message.keys.map((key) => [key.time, key.fov, key.lookAt ? 'lookAt' : 'rotation']),
    [
      [0, 0, 'lookAt'],
      [4, 50, 'lookAt'],
      [9, 0, 'rotation'],
    ],
  );
  camera.receivePathFinished();
  assert.deepEqual(emitted, [['cameraPathFinished', []]]);
  assert.throws(() => Camera.playPath([keys[0]]), RangeError);
  assert.throws(() => Camera.playPath([keys[1], keys[0]]), RangeError);
  assert.throws(() => Camera.playPath(keys, { smooth: 'yes' }), TypeError);
  const far = [keys[0], { ...keys[1], time: 601 }];
  assert.throws(() => Camera.playPath(far), RangeError);
});

// the game's view shake, a kick that tilts the view and settles.
test('shake sends the checked shake with its defaults and refuses values out of range', () => {
  const { Camera, sent } = setup();
  assert.equal(Camera.shake({ angle: { x: 12 } }), true);
  assert.deepEqual(sent.at(-1), {
    op: 'cameraShakeLocal',
    angle: { x: 12, y: 0, z: 0 },
    shift: { x: 0, y: 0, z: 0 },
    duration: 0.5,
    frequency: 0,
    randomness: 0,
  });
  Camera.shake({ angle: { y: -3 }, shift: { z: 0.1 }, duration: 2, frequency: 8, randomness: 0.5 });
  assert.equal(sent.at(-1).frequency, 8);
  assert.throws(() => Camera.shake(null), TypeError);
  assert.throws(() => Camera.shake({ angle: { x: 91 } }), RangeError);
  assert.throws(() => Camera.shake({ duration: 0 }), RangeError);
  assert.throws(() => Camera.shake({ randomness: 2 }), RangeError);
});
