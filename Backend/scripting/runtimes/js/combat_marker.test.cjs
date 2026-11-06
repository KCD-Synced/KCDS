'use strict';

// CombatMarker: the server switches the game's combat marker off and on for everyone.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

function markerOrders(f) {
  return f.sent
    .filter((message) => message.op === 'combatMarker')
    .map((message) => ({ ...message }));
}

test('CombatMarker is on by default and tells nobody', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7);
  assert.equal(f.host.api.CombatMarker.enabled, true);
  assert.deepEqual(markerOrders(f), []);
});

test('CombatMarker.setEnabled tells every ready player once per change', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7);
  f.report(8);
  f.report(9, { ready: false });
  const marker = f.host.api.CombatMarker;
  marker.setEnabled(false);
  marker.setEnabled(false);
  assert.equal(marker.enabled, false);
  marker.setEnabled(true);
  assert.deepEqual(markerOrders(f), [
    { op: 'combatMarker', id: 7, shown: false },
    { op: 'combatMarker', id: 8, shown: false },
    { op: 'combatMarker', id: 7, shown: true },
    { op: 'combatMarker', id: 8, shown: true },
  ]);
});

test('A player whose world gets ready hears the marker only while it is off', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.host.api.CombatMarker.setEnabled(false);
  f.report(7, { ready: false });
  f.report(7);
  f.host.api.CombatMarker.setEnabled(true);
  f.report(8);
  assert.deepEqual(markerOrders(f), [
    { op: 'combatMarker', id: 7, shown: false },
    { op: 'combatMarker', id: 7, shown: true },
  ]);
});

test('CombatMarker.setEnabled takes only true or false', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  assert.throws(() => f.host.api.CombatMarker.setEnabled('off'), TypeError);
  assert.throws(() => f.host.api.CombatMarker.setEnabled(0), TypeError);
});

test('One player has their own combat marker over the server setting, null gives it back', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7);
  f.report(8);
  const first = f.host.api.Player.getById(7);
  first.setCombatMarker(false);
  assert.equal(first.combatMarker, false);
  // The server's switch does not reach a player with their own setting.
  f.host.api.CombatMarker.setEnabled(false);
  first.setCombatMarker(null);
  f.host.api.CombatMarker.setEnabled(true);
  first.setCombatMarker(true);
  first.setCombatMarker(true);
  assert.equal(first.combatMarker, true);
  assert.throws(() => first.setCombatMarker('off'), TypeError);
  assert.deepEqual(markerOrders(f), [
    { op: 'combatMarker', id: 7, shown: false },
    { op: 'combatMarker', id: 8, shown: false },
    { op: 'combatMarker', id: 7, shown: true },
    { op: 'combatMarker', id: 8, shown: true },
  ]);
});
