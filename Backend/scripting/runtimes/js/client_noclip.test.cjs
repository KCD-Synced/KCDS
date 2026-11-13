'use strict';

// The client NoClip global (Client/resources/noclip.cjs): enable/disable and noclipChanged, the
// state mirrored from the game client, poses, aims, rays and projections.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceNoClip, basis, aim } = require('../../../../Client/resources/noclip.cjs');
const { Vector3 } = require('./math.cjs');

function setup() {
  const sent = [];
  const emitted = [];
  const noclip = new ResourceNoClip({
    send: (message) => sent.push(message),
    emit: (name, args) => emitted.push([name, ...args]),
    Vector3,
  });
  return { noclip, sent, emitted, facade: noclip.facade() };
}

// A flight the game client reported: at the origin, looking along +Y, 60 degrees, 16:9.
function flying() {
  const context = setup();
  context.facade.enable({ mode: 'camera' });
  context.noclip.receiveState({
    active: true,
    x: 0,
    y: 0,
    z: 0,
    yaw: 0,
    pitch: 0,
    fov: 60,
    renderedFov: 60,
    aspect: 16 / 9,
  });
  return context;
}

function near(actual, expected, epsilon = 1e-9) {
  assert.ok(Math.abs(actual - expected) < epsilon, `${actual} is not ${expected}`);
}

test('enable sends the settings once and reports the change', () => {
  const { sent, emitted, facade } = setup();
  assert.equal(facade.enable({ mode: 'camera', speed: 1000, fov: 70 }), 'enabled');
  assert.equal(facade.enable(), 'alreadyActive');
  assert.deepEqual(sent, [
    {
      op: 'noclipEnable',
      mode: 'camera',
      showBody: false,
      input: true,
      look: 'always',
      speed: 400,
      fov: 70,
    },
  ]);
  assert.deepEqual(emitted, [['noclipChanged', true, 'script']]);
  assert.equal(facade.isActive(), true);
});

test('enable with orbit circles a target', () => {
  const { sent, facade } = setup();
  assert.throws(() => facade.enable({ orbit: { distance: 2 } }), TypeError);
  assert.throws(
    () => facade.enable({ orbit: { target: new Vector3(1, 2, 3), distance: 99 } }),
    RangeError,
  );
  assert.throws(
    () => facade.enable({ orbit: { target: new Vector3(1, 2, 3) }, keepAboveGround: 1 }),
    TypeError,
  );
  assert.equal(sent.length, 0);
  facade.enable({ mode: 'camera', orbit: { target: new Vector3(1, 2, 3) } });
  assert.deepEqual(sent[0].orbit, { target: { x: 1, y: 2, z: 3 }, distance: 3 });
  assert.equal(sent[0].keepAboveGround, true);
});

test('enable asks for the own body to stay drawn with showBody', () => {
  const { sent, facade } = setup();
  assert.throws(() => facade.enable({ showBody: 'yes' }), TypeError);
  assert.equal(sent.length, 0);
  assert.equal(facade.enable({ mode: 'camera', showBody: true }), 'enabled');
  assert.equal(sent[0].showBody, true);
});

test('look: rightButton turns the camera only while the right button is held', () => {
  const { sent, facade } = setup();
  assert.equal(facade.enable({ look: 'rightButton' }), 'enabled');
  assert.equal(sent[0].look, 'rightButton');
});

test('enable rejects what it cannot guess', () => {
  const { facade } = setup();
  assert.throws(() => facade.enable({ mode: 'ghost' }), TypeError);
  assert.throws(() => facade.enable({ input: 'yes' }), TypeError);
  assert.throws(() => facade.enable({ look: 'mouse' }), TypeError);
  assert.throws(() => facade.enable({ speed: Number.NaN }), TypeError);
  assert.throws(() => facade.enable({ fov: 200 }), RangeError);
  assert.equal(facade.isActive(), false);
});

test('disable ends the flight once; the game ending it reports why', () => {
  const { sent, emitted, facade, noclip } = setup();
  facade.disable();
  assert.equal(sent.length, 0);
  facade.enable();
  facade.disable({ keepPosition: false });
  assert.deepEqual(sent.at(-1), { op: 'noclipDisable', keepPosition: false });
  assert.deepEqual(emitted.at(-1), ['noclipChanged', false, 'script']);
  facade.enable();
  noclip.receiveEnded({ reason: 'worldChanged' });
  assert.deepEqual(emitted.at(-1), ['noclipChanged', false, 'sessionOver']);
  assert.equal(facade.isActive(), false);
});

// The game reports the camera's first state a frame after enable().
const FIRST_STATE = {
  active: true,
  x: 0,
  y: 0,
  z: 0,
  yaw: 0,
  pitch: 0,
  fov: 60,
  renderedFov: 60,
  aspect: 16 / 9,
};

test('reads are null until the game reports the flight', () => {
  const { facade } = setup();
  assert.equal(facade.setPosition(new Vector3(1, 2, 3)), false, 'no flight at all');
  facade.enable();
  assert.equal(facade.getState(), null);
  assert.equal(facade.getPosition(), null);
  assert.equal(facade.screenLine(0, 0), null);
});

// a pose set right after enable() was dropped until the game's first state came.
test('moves made before the first state run as soon as it comes', () => {
  const { facade, noclip, sent } = setup();
  facade.enable({ mode: 'camera', input: false });
  assert.equal(facade.setPose(new Vector3(10, 20, 30), new Vector3(0, -1, 0)), true);
  assert.equal(facade.setRotation({ pitch: -10 }), true);
  assert.equal(
    sent.filter((message) => message.op === 'noclipPose').length,
    0,
    'nothing goes out before the first state',
  );
  noclip.receiveState(FIRST_STATE);
  const poses = sent.filter((message) => message.op === 'noclipPose');
  assert.equal(poses.length, 2);
  assert.deepEqual([poses[1].x, poses[1].y, poses[1].z], [10, 20, 30]);
  near(Math.abs(poses[1].yaw), 180);
  near(poses[1].pitch, -10);
  const position = facade.getPosition();
  assert.deepEqual([position.x, position.y, position.z], [10, 20, 30]);
  // A bad argument still throws at once, not later.
  facade.disable();
  facade.enable();
  assert.throws(() => facade.lookAt('north'), TypeError);
  // Ending the flight drops what waits.
  facade.setPosition(new Vector3(1, 1, 1));
  facade.disable();
  facade.enable();
  noclip.receiveState(FIRST_STATE);
  assert.equal(sent.filter((message) => message.op === 'noclipPose').length, 2);
});

test('enable takes a starting position and forward', () => {
  const { facade, noclip, sent } = setup();
  assert.throws(() => facade.enable({ position: 'here' }), TypeError);
  facade.enable({ position: new Vector3(5, 6, 7), forward: new Vector3(1, 0, 0) });
  noclip.receiveState(FIRST_STATE);
  const pose = sent.find((message) => message.op === 'noclipPose');
  assert.deepEqual([pose.x, pose.y, pose.z], [5, 6, 7]);
  near(Math.abs(pose.yaw), 90);
});

test('getState carries the pose and its unit axes', () => {
  const { facade } = flying();
  const state = facade.getState();
  assert.equal(state.mode, 'camera');
  assert.deepEqual([state.forward.x, state.forward.y, state.forward.z], [-0, 1, 0]);
  assert.deepEqual([state.right.x, state.right.y, state.right.z], [1, 0, 0]);
  near(state.up.z, 1);
  assert.equal(state.aspectRatio, 16 / 9);
});

test('basis and aim agree: yaw rises to the left, pitch up', () => {
  const left = basis(90, 0).forward;
  near(left[0], -1);
  near(left[1], 0);
  const angles = aim(-1, 0, 0);
  near(angles.yaw, 90);
  near(aim(0, 1, 1).pitch, 45);
  assert.equal(aim(0, 0, 0), null);
});

test('setRotation wraps and clamps and sends the pose', () => {
  const { facade, sent } = flying();
  assert.equal(facade.setRotation({ yaw: 270, pitch: 120 }), true);
  assert.deepEqual(facade.getRotation(), { yaw: -90, pitch: 89 });
  assert.deepEqual(sent.at(-1), { op: 'noclipPose', x: 0, y: 0, z: 0, yaw: -90, pitch: 89 });
});

test('lookAt turns to a point, focus stands back from it', () => {
  const { facade } = flying();
  facade.lookAt(new Vector3(-10, 0, 0));
  near(facade.getRotation().yaw, 90);
  facade.focus(new Vector3(0, 20, 0), 5);
  const position = facade.getPosition();
  near(position.x, 0);
  near(position.y, 15);
  near(facade.getRotation().yaw, 0);
});

test('a ray through the centre is the forward; a point ahead projects back to it', () => {
  const { facade } = flying();
  const ray = facade.screenLine(0, 0);
  near(ray.y, 1);
  const corner = facade.screenLine(1, 1);
  const projected = facade.toScreen(new Vector3(corner.x * 10, corner.y * 10, corner.z * 10));
  near(projected.x, 1, 1e-9);
  near(projected.y, 1, 1e-9);
  assert.equal(facade.toScreen(new Vector3(0, -5, 0)), null);
});

test('move is refused while the built-in driver flies', () => {
  const { facade, sent } = flying();
  assert.equal(facade.move({ forward: 1 }, 0.016), false);
  facade.setInputEnabled(false);
  assert.equal(facade.move({ forward: 2, yaw: 3, boost: true }, 1), true);
  assert.deepEqual(sent.at(-1), {
    op: 'noclipMove',
    forward: 1,
    strafe: 0,
    lift: 0,
    yaw: 3,
    pitch: 0,
    boost: true,
    crawl: false,
    dt: 0.1,
  });
});

test('speed and fov clamp and last across flights', () => {
  const { facade } = setup();
  facade.setSpeed(0.001);
  assert.equal(facade.getSpeed(), 0.05);
  facade.setFov(10);
  assert.equal(facade.getFov(), 20);
  assert.throws(() => facade.setSpeed('fast'), TypeError);
});

test('a player the server does not let fly gets notAllowed', () => {
  const { sent, emitted, facade, noclip } = setup();
  assert.equal(facade.isAllowed(), true);
  noclip.receiveAllowed({ allowed: false });
  assert.equal(facade.isAllowed(), false);
  assert.deepEqual(emitted, [['noclipAllowedChanged', false]]);
  assert.equal(facade.enable(), 'notAllowed');
  assert.equal(facade.isActive(), false);
  assert.deepEqual(sent, []);
  // Bad options still throw first.
  assert.throws(() => facade.enable({ mode: 'ghost' }), TypeError);

  noclip.receiveAllowed({ allowed: true });
  assert.deepEqual(emitted.at(-1), ['noclipAllowedChanged', true]);
  assert.equal(facade.enable(), 'enabled');
});

test('a flight the server forbids ends with notAllowed', () => {
  const { emitted, facade, noclip } = flying();
  noclip.receiveAllowed({ allowed: false });
  assert.equal(facade.isActive(), false);
  assert.equal(facade.getState(), null);
  assert.deepEqual(emitted.slice(-2), [
    ['noclipChanged', false, 'notAllowed'],
    ['noclipAllowedChanged', false],
  ]);
  // The same answer again changes nothing.
  noclip.receiveAllowed({ allowed: false });
  assert.equal(emitted.length, 3);
});

test('a flight the game refused as not allowed reports it', () => {
  const { emitted, facade, noclip } = setup();
  facade.enable();
  noclip.receiveEnded({ reason: 'notAllowed' });
  assert.deepEqual(emitted.at(-1), ['noclipChanged', false, 'notAllowed']);
  assert.equal(facade.isActive(), false);
});

test('a body flight on a horse ends with mounted', () => {
  const { emitted, facade, noclip } = setup();
  facade.enable({ mode: 'body' });
  noclip.receiveEnded({ reason: 'mounted' });
  assert.deepEqual(emitted.at(-1), ['noclipChanged', false, 'mounted']);
  assert.equal(facade.isActive(), false);
});
