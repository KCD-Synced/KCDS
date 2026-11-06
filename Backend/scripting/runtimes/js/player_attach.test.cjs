'use strict';

// One player attached to another.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

function orders(f) {
  return f.sent.filter((message) => message.op === 'playerAttach');
}

test('attachTo tells the attached player, detach frees them', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const guard = f.report(7);
  const prisoner = f.report(8);
  assert.equal(prisoner.attachedTo, null);
  assert.equal(prisoner.detach(), false, 'nothing to end');

  assert.equal(prisoner.attachTo(guard, { offset: { x: 0.5, y: 0.25, z: 0 }, rotation: 15 }), true);
  assert.equal(prisoner.attachedTo, guard);
  assert.deepEqual(orders(f), [
    {
      op: 'playerAttach',
      id: 8,
      target: 7,
      follow: false,
      right: 0.5,
      forward: 0.25,
      up: 0,
      yaw: 15,
    },
  ]);

  assert.equal(prisoner.detach(), true);
  assert.equal(prisoner.attachedTo, null);
  assert.deepEqual(orders(f)[1], {
    op: 'playerAttach',
    id: 8,
    target: 0,
    follow: false,
    right: 0,
    forward: 0,
    up: 0,
    yaw: 0,
  });
});

test('attachTo refuses the player themselves, a circle and bad values', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const a = f.report(7);
  const b = f.report(8);
  assert.equal(a.attachTo(a), false);
  assert.equal(a.attachTo(null), false);
  assert.equal(a.attachTo(b), true);
  assert.equal(b.attachTo(a), false, 'a circle');
  assert.throws(() => b.attachTo(a, { rotation: 'left' }), TypeError);
  assert.throws(() => a.attachTo(b, { offset: { x: 100 } }), TypeError);
});

test('a player who leaves frees whoever was attached to them', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const guard = f.report(7);
  const prisoner = f.report(8);
  prisoner.attachTo(guard);
  f.host.playerAttachments.left(guard);
  assert.equal(prisoner.attachedTo, null);
  assert.equal(orders(f).at(-1).target, 0);
});

test('attachTo look: free by default, follow locks the view; bad values throw', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const guard = f.report(7);
  const prisoner = f.report(8);
  prisoner.attachTo(guard, { look: 'follow' });
  assert.equal(orders(f).at(-1).follow, true);
  prisoner.attachTo(guard);
  assert.equal(orders(f).at(-1).follow, false);
  assert.throws(() => prisoner.attachTo(guard, { look: 'up' }), TypeError);
  assert.throws(() => prisoner.attachTo(guard, { animation: 'Cower' }), TypeError);
});

test('attachTo plays both animations and detach stops them', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const guard = f.report(7);
  const prisoner = f.report(8);
  const plays = (id) =>
    f.sent.filter((m) => m.op === 'animation' && m.id === id && m.kind === 'play');
  const stops = (id) =>
    f.sent.filter((m) => m.op === 'animation' && m.id === id && m.kind === 'stop');
  prisoner.attachTo(guard, {
    animation: { fragment: 'Quest_CaptiveIdle' },
    otherAnimation: { fragment: 'Quest_HostageSituation_HoldHostage_Tense' },
  });
  assert.equal(plays(8).length, 1);
  assert.equal(plays(8)[0].fragment, 'Quest_CaptiveIdle');
  assert.equal(plays(8)[0].loop, true);
  assert.equal(plays(7).length, 1);
  assert.equal(plays(7)[0].fragment, '', 'the upper body plays the clip');
  assert.ok(plays(7)[0].clip);
  prisoner.detach();
  assert.equal(stops(8).length, 1);
  assert.equal(stops(7).length, 1);
});
