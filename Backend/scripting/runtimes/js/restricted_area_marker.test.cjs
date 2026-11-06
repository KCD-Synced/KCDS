'use strict';

// RestrictedAreaMarker: the server switches the game's restricted-area marker off and
// on for everyone.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

function markerOrders(f) {
  return f.sent
    .filter((message) => message.op === 'restrictedAreaMarker')
    .map((message) => ({ ...message }));
}

test('RestrictedAreaMarker is on by default and tells nobody', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7);
  assert.equal(f.host.api.RestrictedAreaMarker.enabled, true);
  assert.deepEqual(markerOrders(f), []);
});

test('RestrictedAreaMarker.setEnabled tells every ready player once per change', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7);
  f.report(8);
  f.report(9, { ready: false });
  const marker = f.host.api.RestrictedAreaMarker;
  marker.setEnabled(false);
  marker.setEnabled(false);
  assert.equal(marker.enabled, false);
  marker.setEnabled(true);
  assert.deepEqual(markerOrders(f), [
    { op: 'restrictedAreaMarker', id: 7, shown: false },
    { op: 'restrictedAreaMarker', id: 8, shown: false },
    { op: 'restrictedAreaMarker', id: 7, shown: true },
    { op: 'restrictedAreaMarker', id: 8, shown: true },
  ]);
});

test('A player whose world gets ready hears the marker only while it is off', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.host.api.RestrictedAreaMarker.setEnabled(false);
  f.report(7, { ready: false });
  f.report(7);
  f.host.api.RestrictedAreaMarker.setEnabled(true);
  f.report(8);
  assert.deepEqual(markerOrders(f), [
    { op: 'restrictedAreaMarker', id: 7, shown: false },
    { op: 'restrictedAreaMarker', id: 7, shown: true },
  ]);
});

test('RestrictedAreaMarker.setEnabled takes only true or false', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  assert.throws(() => f.host.api.RestrictedAreaMarker.setEnabled('off'), TypeError);
  assert.throws(() => f.host.api.RestrictedAreaMarker.setEnabled(0), TypeError);
});
