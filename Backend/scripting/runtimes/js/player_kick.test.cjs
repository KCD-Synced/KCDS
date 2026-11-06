'use strict';

// Player.kick: the server ends a player's session with a reason its client shows.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

function kicks(f) {
  return f.sent.filter((message) => message.op === 'kick').map((message) => ({ ...message }));
}

test('Player.kick sends the reason for the player', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const player = f.report(7);
  assert.equal(player.kick('Spamming the chat'), true);
  assert.equal(f.report(8).kick(), true);
  assert.deepEqual(kicks(f), [
    { op: 'kick', id: 7, reason: 'Spamming the chat' },
    { op: 'kick', id: 8, reason: '' },
  ]);
});

test('Player.kick takes a reason of at most 200 bytes', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const player = f.report(7);
  assert.throws(() => player.kick(42), TypeError);
  assert.throws(() => player.kick('x'.repeat(201)), RangeError);
  // Two-byte letters count twice.
  assert.throws(() => player.kick('č'.repeat(101)), RangeError);
  assert.equal(player.kick('č'.repeat(100)), true);
});

test('Player.disconnect lets a player go without "kicked", or closes their game', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  assert.equal(f.report(7).disconnect('Uloženo, nashledanou'), true);
  assert.equal(f.report(8).disconnect('', { quit: true }), true);
  assert.equal(f.report(9).disconnect(), true);
  assert.deepEqual(kicks(f), [
    { op: 'kick', id: 7, reason: 'Uloženo, nashledanou', kind: 'leave' },
    { op: 'kick', id: 8, reason: '', kind: 'quit' },
    { op: 'kick', id: 9, reason: '', kind: 'leave' },
  ]);
  const player = f.report(10);
  assert.throws(() => player.disconnect(1), TypeError);
  assert.throws(() => player.disconnect('', { quit: 'yes' }), TypeError);
  assert.throws(() => player.disconnect('', null), TypeError);
  assert.throws(() => player.disconnect('x'.repeat(201)), RangeError);
});
