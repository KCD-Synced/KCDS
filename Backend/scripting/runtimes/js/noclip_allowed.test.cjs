'use strict';

// NoClip allowed per player: Player.noclipAllowed tells the player's game, and tells
// it again when their world gets ready while they may not fly.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

function noclipOrders(f) {
  return f.sent
    .filter((message) => message.op === 'playerNoClipAllowed')
    .map((message) => [message.id, message.allowed]);
}

test('player.noclipAllowed tells the player once per change', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const player = f.report(7, { position: { x: 100, y: 200, z: 30 } });
  assert.equal(player.noclipAllowed, true);
  player.noclipAllowed = false;
  player.noclipAllowed = false;
  assert.equal(player.noclipAllowed, false);
  player.noclipAllowed = true;
  assert.equal(player.noclipAllowed, true);
  assert.deepEqual(noclipOrders(f), [
    [7, false],
    [7, true],
  ]);
  assert.throws(() => {
    player.noclipAllowed = 'no';
  }, TypeError);
});

test('a player who may not fly is told again when their world gets ready', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const player = f.report(7, { position: { x: 100, y: 200, z: 30 } });
  player.noclipAllowed = false;
  f.report(7, { ready: false });
  f.report(7, { ready: true });
  assert.deepEqual(noclipOrders(f), [
    [7, false],
    [7, false],
  ]);

  // A player who may fly is not told on arrival: their game allows it already.
  const other = f.report(8, { position: { x: 0, y: 0, z: 0 } });
  f.report(8, { ready: false });
  f.report(8, { ready: true });
  assert.equal(other.noclipAllowed, true);
  assert.deepEqual(
    noclipOrders(f).filter(([id]) => id === 8),
    [],
  );
});

test('a player forbidden before their world is ready hears it once it is', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const player = f.report(9, { ready: false, position: { x: 0, y: 0, z: 0 } });
  player.noclipAllowed = false;
  assert.deepEqual(noclipOrders(f), []);
  f.report(9, { ready: true });
  assert.deepEqual(noclipOrders(f), [[9, false]]);
});
