'use strict';

// Player.crown and Player.hairCrown: what goes to the clients in the field "headPiece".

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { fieldOf } = require('./appearance/head_pieces.cjs');

function lastHeadPiece(sent) {
  return sent.filter((message) => message.op === 'playerHeadPiece').pop();
}

test('the field of every head piece', () => {
  assert.equal(fieldOf({ crown: null, hairCrown: true }), '');
  assert.equal(fieldOf({ crown: 'sigismund', hairCrown: true }), 'sigismund|1');
  assert.equal(fieldOf({ crown: 'sigismund', hairCrown: false }), 'sigismund|0');
  assert.equal(fieldOf({ crown: null, hairCrown: false }), '|0');
});

test('Player.crown and hairCrown go to every client, changes only', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  assert.equal(player.crown, null);
  assert.equal(player.hairCrown, true);

  player.crown = 'sigismund';
  assert.equal(player.crown, 'sigismund');
  assert.deepEqual(lastHeadPiece(sent), { op: 'playerHeadPiece', id: 3, field: 'sigismund|1' });

  player.hairCrown = false;
  assert.equal(player.hairCrown, false);
  assert.equal(lastHeadPiece(sent).field, 'sigismund|0');

  const count = sent.filter((message) => message.op === 'playerHeadPiece').length;
  player.hairCrown = false;
  assert.equal(sent.filter((message) => message.op === 'playerHeadPiece').length, count);

  player.crown = null;
  player.hairCrown = true;
  assert.equal(player.crown, null);
  assert.equal(lastHeadPiece(sent).field, '');
});

test('Player.crown takes only the game crowns, hairCrown only booleans', async (t) => {
  const { host, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  for (const wrong of ['', 'Sigismund', 'wenceslas', 1, {}]) {
    assert.throws(() => {
      player.crown = wrong;
    }, TypeError);
  }
  for (const wrong of [0, 1, 'true', null]) {
    assert.throws(() => {
      player.hairCrown = wrong;
    }, TypeError);
  }
  assert.equal(player.crown, null);
  assert.equal(player.hairCrown, true);
});
