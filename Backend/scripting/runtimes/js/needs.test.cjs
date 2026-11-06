'use strict';

// Needs (a KCDS addition): the server turns the game's sleep and food needs off and on
// for every player, newcomers included.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  serverFixture,
  serverResource,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const FULL = 1000000;

function needsOrders(f) {
  return f.sent
    .filter((message) => message.op === 'needs' || message.op === 'setStat')
    .map((message) => ({ ...message }));
}

function off(id) {
  return [
    { op: 'needs', id, enabled: false },
    { op: 'setStat', id, stat: 2, value: FULL },
    { op: 'setStat', id, stat: 3, value: FULL },
  ];
}

test('Needs are off by default: every arriving player is told and filled', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7);
  assert.equal(f.host.api.Needs.enabled, false);
  assert.deepEqual(needsOrders(f), off(7));
});

test('Needs.setEnabled tells every ready player once per change and fills them when off', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7);
  f.report(8);
  f.report(9, { ready: false });
  const needs = f.host.api.Needs;
  needs.setEnabled(false);
  needs.setEnabled(false);
  assert.equal(needs.enabled, false);
  needs.setEnabled(true);
  assert.deepEqual(needsOrders(f), [
    ...off(7),
    ...off(8),
    { op: 'needs', id: 7, enabled: true },
    { op: 'needs', id: 8, enabled: true },
  ]);
});

test('A player whose world gets ready hears the needs only while they are off', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.host.api.Needs.setEnabled(false);
  f.report(7, { ready: false });
  f.report(7);
  f.report(7);
  f.host.api.Needs.setEnabled(true);
  f.report(8);
  assert.deepEqual(needsOrders(f), [...off(7), { op: 'needs', id: 7, enabled: true }]);
});

test('The fill on arrival comes before a resource restores stored values', async (t) => {
  const f = serverFixture(t);
  serverResource(
    f.root,
    'keeper',
    `kcds.players.Needs.setEnabled(false);
     kcds.core.Events.on('playerSpawned', (player) => player.setStat('hunger', 40));`,
  );
  await f.host.startAll();
  f.report(7);
  assert.deepEqual(needsOrders(f), [...off(7), { op: 'setStat', id: 7, stat: 3, value: 40 }]);
});

test('A knocked-out player hears the needs but is not filled', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.host.api.Needs.setEnabled(true);
  f.report(7);
  f.host.playerDied(7);
  f.host.api.Needs.setEnabled(false);
  assert.deepEqual(needsOrders(f), [{ op: 'needs', id: 7, enabled: false }]);
});

test('Needs.setEnabled takes only true or false', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  assert.throws(() => f.host.api.Needs.setEnabled('off'), TypeError);
  assert.throws(() => f.host.api.Needs.setEnabled(0), TypeError);
  assert.equal(f.host.api.Needs.enabled, false);
});
