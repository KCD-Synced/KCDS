'use strict';

// ThirdPerson: the server decides whether players may switch to the third-person
// camera, must stay in first person or must stay in third person.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

function modeOrders(f) {
  return f.sent
    .filter((message) => message.op === 'thirdPerson')
    .map((message) => ({ ...message }));
}

test('ThirdPerson is allowed by default and tells nobody', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7);
  assert.equal(f.host.api.ThirdPerson.mode, 'allowed');
  assert.deepEqual(modeOrders(f), []);
});

test('ThirdPerson.setMode tells every ready player once per change', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7);
  f.report(8);
  f.report(9, { ready: false });
  const camera = f.host.api.ThirdPerson;
  camera.setMode('forced');
  camera.setMode('forced');
  assert.equal(camera.mode, 'forced');
  camera.setMode('disabled');
  camera.setMode('allowed');
  assert.equal(camera.mode, 'allowed');
  assert.deepEqual(modeOrders(f), [
    { op: 'thirdPerson', id: 7, mode: 2 },
    { op: 'thirdPerson', id: 8, mode: 2 },
    { op: 'thirdPerson', id: 7, mode: 1 },
    { op: 'thirdPerson', id: 8, mode: 1 },
    { op: 'thirdPerson', id: 7, mode: 0 },
    { op: 'thirdPerson', id: 8, mode: 0 },
  ]);
});

test('A player whose world gets ready hears the mode only while it is not allowed', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.host.api.ThirdPerson.setMode('disabled');
  f.report(7, { ready: false });
  f.report(7);
  f.host.api.ThirdPerson.setMode('allowed');
  f.report(8);
  assert.deepEqual(modeOrders(f), [
    { op: 'thirdPerson', id: 7, mode: 1 },
    { op: 'thirdPerson', id: 7, mode: 0 },
  ]);
});

test('ThirdPerson.setMode takes only allowed, disabled or forced', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const camera = f.host.api.ThirdPerson;
  assert.throws(() => camera.setMode('on'), TypeError);
  assert.throws(() => camera.setMode(true), TypeError);
  assert.throws(() => camera.setMode(2), TypeError);
  assert.throws(() => camera.setMode(), TypeError);
  assert.throws(() => camera.setMode('toString'), TypeError);
  assert.equal(camera.mode, 'allowed');
});
