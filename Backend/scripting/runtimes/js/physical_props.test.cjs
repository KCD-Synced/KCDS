'use strict';

// Player.setPhysicalProp and Player.physicalProp (Asset/Animation Studio):
// a rope or a whip in the player's hand with its grip and physics, carried to every client
// by the NetworkPlayer field physicalProp (kcd2s/integration/physical_prop.hpp).
const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const lastProp = (sent) => sent.filter((message) => message.op === 'playerPhysicalProp').pop();

// What a prop's settings are when none is given: the studio's first ones.
const FIRST_SETTINGS = Object.freeze({
  hand: 'RightHand',
  grip: { pitch: 90, twist: 0 },
  physics: {
    gravity: 9.8,
    damping: 0.3,
    mass: 0.5,
    air: 0.1,
    bend: 0,
    stiffness: 0,
    collide: 1,
    shape: 0,
    curl: 0,
    sway: 0,
  },
});

test('a whip with its defaults goes to every client as the field text', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  assert.equal(player.physicalProp, null);
  player.setPhysicalProp({ kind: 'whip' });
  assert.deepEqual(player.physicalProp, {
    kind: 'whip',
    segments: 20,
    length: 1.6,
    ...FIRST_SETTINGS,
  });
  assert.deepEqual(lastProp(sent), { op: 'playerPhysicalProp', id: 3, prop: 'whip 20 1.60' });
});

test('a chain is a rope drawn with the game’s chain links', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  player.setPhysicalProp({ kind: 'chain' });
  assert.deepEqual(player.physicalProp, {
    kind: 'chain',
    segments: 24,
    length: 1.5,
    ...FIRST_SETTINGS,
  });
  assert.deepEqual(lastProp(sent), { op: 'playerPhysicalProp', id: 3, prop: 'chain 24 1.50' });
});

test('a rope with its own settings; null takes it away', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  player.setPhysicalProp({ kind: 'rope', segments: 24, length: 2.345 });
  assert.deepEqual(player.physicalProp, {
    kind: 'rope',
    segments: 24,
    length: 2.35,
    ...FIRST_SETTINGS,
  });
  assert.equal(lastProp(sent).prop, 'rope 24 2.35');
  // The value returned is a copy.
  player.physicalProp.segments = 2;
  player.physicalProp.physics.gravity = 1;
  assert.equal(player.physicalProp.segments, 24);
  assert.equal(player.physicalProp.physics.gravity, 9.8);
  player.setPhysicalProp(null);
  assert.equal(player.physicalProp, null);
  assert.deepEqual(lastProp(sent), { op: 'playerPhysicalProp', id: 3, prop: '' });
});

test('the hand, grip and physics go with it as the studio set them', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  player.setPhysicalProp({
    kind: 'whip',
    hand: 'LeftHand',
    grip: { pitch: 20 },
    physics: { damping: 0.1, collide: 2, bend: 0 },
  });
  assert.deepEqual(player.physicalProp.grip, { pitch: 20, twist: 0 });
  assert.equal(player.physicalProp.hand, 'LeftHand');
  assert.equal(player.physicalProp.physics.collide, 2);
  // Only the settings that differ from the first ones are sent.
  assert.equal(lastProp(sent).prop, 'whip 20 1.60 h=1 p=20 d=0.1 c=2');
});

test('a whip’s lash posed instead of moved by physics goes with it', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  player.setPhysicalProp({ kind: 'whip', physics: { shape: 1, curl: 120, sway: -30 } });
  assert.deepEqual(
    [
      player.physicalProp.physics.shape,
      player.physicalProp.physics.curl,
      player.physicalProp.physics.sway,
    ],
    [1, 120, -30],
  );
  assert.equal(lastProp(sent).prop, 'whip 20 1.60 k=1 u=120 w=-30');
  assert.throws(() => player.setPhysicalProp({ kind: 'whip', physics: { shape: 2 } }), RangeError);
});

test('what does not fit throws and changes nothing', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  player.setPhysicalProp({ kind: 'rope' });
  const before = sent.length;
  for (const wrong of [
    undefined,
    'rope',
    { kind: 'cloth' },
    {},
    { kind: 'rope', hand: 'Foot' },
    { kind: 'rope', grip: 5 },
    { kind: 'rope', grip: { angle: 3 } },
    { kind: 'rope', physics: { wobble: 1 } },
    { kind: 'rope', physics: { pitch: 20 } },
  ]) {
    assert.throws(() => player.setPhysicalProp(wrong), TypeError);
  }
  for (const wrong of [
    { kind: 'rope', segments: 1 },
    { kind: 'rope', segments: 65 },
    { kind: 'rope', segments: 2.5 },
    { kind: 'whip', length: 0.1 },
    { kind: 'whip', length: 21 },
    { kind: 'whip', length: Number.NaN },
    { kind: 'whip', grip: { pitch: 200 } },
    { kind: 'whip', physics: { collide: 3 } },
    { kind: 'whip', physics: { gravity: -1 } },
  ]) {
    assert.throws(() => player.setPhysicalProp(wrong), RangeError);
  }
  assert.equal(sent.length, before);
  assert.deepEqual(player.physicalProp, {
    kind: 'rope',
    segments: 16,
    length: 1.2,
    ...FIRST_SETTINGS,
  });
});
