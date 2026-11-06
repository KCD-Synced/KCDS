'use strict';

// Handcuffs: Player.setCuffed puts the hands behind the back and lets the player only
// walk and look around until a script takes the cuffs off.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { CUFFED_CLIP } = require('./cuffs/cuffs.cjs');

function orders(f) {
  return f.sent
    .filter((message) => message.op === 'cuffs' || message.op === 'animation')
    .map((message) =>
      message.op === 'cuffs'
        ? ['cuffs', message.id, message.cuffed]
        : ['animation', message.id, message.kind, message.clip ?? '', message.loop ?? false],
    );
}

test('setCuffed tells the player and plays the pose; uncuffing stops it', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const player = f.report(7);
  const changes = [];
  f.host.on('playerCuffedChange', (who, cuffed) => changes.push([who.id, cuffed]));
  assert.equal(player.cuffed, false);
  assert.equal(player.setCuffed(true), true);
  assert.equal(player.setCuffed(true), true);
  assert.equal(player.cuffed, true);
  assert.equal(player.setCuffed(false), true);
  assert.equal(player.cuffed, false);
  assert.deepEqual(orders(f), [
    ['cuffs', 7, true],
    ['animation', 7, 'play', CUFFED_CLIP, true],
    ['cuffs', 7, false],
    ['animation', 7, 'stop', '', false],
  ]);
  assert.deepEqual(changes, [
    [7, true],
    [7, false],
  ]);
  assert.throws(() => player.setCuffed(1), TypeError);
});

test('a cuffed player cannot be given another animation or have the pose stopped', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const player = f.report(7);
  player.setCuffed(true);
  assert.equal(player.playAnimation('', { clip: 'reading_stand_inv_in' }), false);
  assert.equal(player.stopAnimation(), false);
  assert.equal(orders(f).length, 2);
  player.setCuffed(false);
  assert.equal(player.playAnimation('', { clip: 'reading_stand_inv_in' }), true);
});

test('a cuffed player is cuffed again when their world is ready again', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const loading = f.report(8, { ready: false });
  loading.setCuffed(true);
  assert.deepEqual(orders(f), []);
  f.report(8);
  f.report(8, { ready: false });
  f.report(8);
  const sent = orders(f);
  // The first time the pose goes with it; after that the player's animation is put on the new
  // body by itself and the cuffs are told again.
  assert.deepEqual(sent.slice(0, 2), [
    ['cuffs', 8, true],
    ['animation', 8, 'play', CUFFED_CLIP, true],
  ]);
  assert.deepEqual(
    sent.filter((order) => order[0] === 'cuffs'),
    [
      ['cuffs', 8, true],
      ['cuffs', 8, true],
    ],
  );
  assert.equal(sent.filter((order) => order[2] === 'play').length, 2);
});
