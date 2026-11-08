'use strict';

// Player.setLeadRope and Player.leadRope: a rope from the player's body to another body, sent to
// every client as the NetworkPlayer field leadRope.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const lastRope = (sent) => sent.filter((message) => message.op === 'playerLeadRope').pop();

test('a guard leads a prisoner: hand to waist by default, as the field text', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const guard = report(3);
  const prisoner = report(4);
  assert.equal(guard.leadRope, null);
  guard.setLeadRope(prisoner);
  assert.deepEqual(guard.leadRope, { target: 4, from: 'RightHand', to: 'Spine', length: 2.5 });
  assert.deepEqual(lastRope(sent), { op: 'playerLeadRope', id: 3, rope: '4|RightHand|Spine|2.50' });
  guard.setLeadRope(4, { from: 'left', to: 'Neck', length: 1.25 });
  assert.equal(lastRope(sent).rope, '4|LeftHand|Neck|1.25');
  // A copy: changing it changes nothing.
  guard.leadRope.length = 9;
  assert.equal(guard.leadRope.length, 1.25);
  guard.setLeadRope(null);
  assert.equal(guard.leadRope, null);
  assert.deepEqual(lastRope(sent), { op: 'playerLeadRope', id: 3, rope: '' });
});

test('setLeadRope refuses what no client can draw', async (t) => {
  const { host, report } = serverFixture(t);
  await host.startAll();
  const guard = report(3);
  assert.throws(() => guard.setLeadRope(guard), TypeError);
  assert.throws(() => guard.setLeadRope('someone'), TypeError);
  assert.throws(() => guard.setLeadRope(4, { length: 0.1 }), RangeError);
  assert.throws(() => guard.setLeadRope(4, { length: 30 }), RangeError);
  assert.throws(() => guard.setLeadRope(4, { from: 'a|b' }), TypeError);
  assert.throws(() => guard.setLeadRope(4, { to: '' }), TypeError);
  assert.throws(() => guard.setLeadRope(4, { colour: 'red' }), TypeError);
  assert.equal(guard.leadRope, null);
});
