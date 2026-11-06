'use strict';

// A player's whistle for their horse: playerWhistle, the players near them told, Player.whistle.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

function whistles(sent) {
  return sent.filter((message) => message.op === 'whistle').map((m) => [m.id, m.whistler]);
}

test('a whistle reaches the players within range in the same world', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const heard = [];
  host.api.Events.on('playerWhistle', (player) => heard.push(player.id));
  const whistler = report(1, { position: { x: 0, y: 0, z: 0 } });
  report(2, { position: { x: 60, y: 0, z: 0 } });
  report(3, { position: { x: 150, y: 0, z: 0 } });
  report(4, { position: { x: 5, y: 0, z: 0 }, dimension: 9 });
  host.whistles.reported(whistler);
  assert.deepEqual(heard, [1]);
  assert.deepEqual(whistles(sent), [[2, 1]]);
});

test('Player.whistle tells the player own game to whistle', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(5);
  assert.equal(player.whistle(), true);
  assert.deepEqual(whistles(sent), [[5, 0]]);
});
