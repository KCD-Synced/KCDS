'use strict';

// A frozen player: Player.frozen tells the player's game to let only looking around
// through, and puts a body that moves anyway back where it was frozen.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { HOLD_RANGE } = require('./frozen/frozen_players.cjs');

function frozenOrders(f) {
  return f.sent
    .filter((message) => message.op === 'playerFrozen')
    .map((message) => [message.id, message.frozen]);
}

function putBacks(f) {
  return f.sent
    .filter((message) => message.op === 'entityPosition')
    .map((message) => [message.id, message.position.x, message.position.y]);
}

test('player.frozen tells the player and holds the body where it was frozen', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const player = f.report(7, { position: { x: 100, y: 200, z: 30 } });
  assert.equal(player.frozen, false);
  player.frozen = true;
  player.frozen = true;
  assert.equal(player.frozen, true);
  assert.deepEqual(frozenOrders(f), [[7, true]]);

  // A small drift is left alone; a body that walked off goes back.
  f.report(7, { position: { x: 100 + HOLD_RANGE / 2, y: 200, z: 30 } });
  assert.deepEqual(putBacks(f), []);
  f.report(7, { position: { x: 104, y: 200, z: 30 } });
  assert.deepEqual(putBacks(f), [[7, 100, 200]]);

  // A script's teleport moves where the player is held.
  player.teleport({ x: 150, y: 250, z: 30 });
  f.report(7, { position: { x: 150, y: 250, z: 30 } });
  assert.equal(putBacks(f).length, 2);

  player.frozen = false;
  assert.equal(player.frozen, false);
  f.report(7, { position: { x: 190, y: 250, z: 30 } });
  assert.equal(putBacks(f).length, 2);
  assert.deepEqual(frozenOrders(f), [
    [7, true],
    [7, false],
  ]);
  assert.throws(() => {
    player.frozen = 1;
  }, TypeError);
});

test('a frozen player whose world gets ready again is told again', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const player = f.report(7, { position: { x: 100, y: 200, z: 30 } });
  player.frozen = true;
  f.report(7, { ready: false });
  f.report(7, { ready: true, position: { x: 120, y: 200, z: 30 } });
  assert.deepEqual(frozenOrders(f), [
    [7, true],
    [7, true],
  ]);
});
