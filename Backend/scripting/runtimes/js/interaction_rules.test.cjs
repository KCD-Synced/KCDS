'use strict';

// Interactions.set and Player.setInteraction: the game's own interactions a KCDS server allows,
// off by default; each player's game hears every change from nothing allowed.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

function rules(sent, id) {
  return sent
    .filter((message) => message.op === 'interactionRules' && message.id === id)
    .map((message) => message.allowed);
}

test('everything is off at first; the server and a player setting turn it on', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const { Interactions } = host.api;
  assert.deepEqual(Interactions.names, [
    'horseWhistle',
    'npcTalk',
    'npcChat',
    'playerTalk',
    'npcChatter',
    'pointing',
    'trespass',
    'cooking',
    'smoking',
  ]);
  const player = report(7);
  assert.deepEqual(rules(sent, 7), [], 'nothing allowed: nothing to tell');
  assert.equal(player.interactionAllowed('horseWhistle'), false);
  Interactions.set('horseWhistle', true);
  assert.equal(Interactions.get('horseWhistle'), true);
  assert.equal(player.interactionAllowed('horseWhistle'), true);
  // The player's own setting wins over the server's; null goes back to it.
  assert.equal(player.setInteraction('horseWhistle', false), true);
  assert.equal(player.setInteraction('playerTalk', true), true);
  assert.equal(player.setInteraction('horseWhistle', null), true);
  // An unchanged setting is not sent again.
  Interactions.set('horseWhistle', true);
  assert.deepEqual(rules(sent, 7), [1, 0, 8, 9]);
});

test('a player arriving later hears the server setting; bad arguments throw', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const { Interactions } = host.api;
  Interactions.set('playerTalk', true);
  const player = report(8);
  assert.deepEqual(rules(sent, 8), [8]);
  assert.throws(() => Interactions.set('dance', true), TypeError);
  assert.throws(() => Interactions.set('npcTalk', 1), TypeError);
  assert.throws(() => player.setInteraction('npcTalk', 'yes'), TypeError);
  assert.throws(() => Interactions.get('dance'), TypeError);
});

test('the NPCs talk unless npcChatter is off: its bit tells the game to keep them quiet', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const { Interactions } = host.api;
  const player = report(7);
  assert.equal(Interactions.get('npcChatter'), true);
  assert.equal(player.interactionAllowed('npcChatter'), true);
  assert.deepEqual(rules(sent, 7), [], 'talk on: nothing to tell');
  Interactions.set('npcChatter', false);
  assert.equal(player.interactionAllowed('npcChatter'), false);
  Interactions.set('npcChatter', true);
  assert.deepEqual(rules(sent, 7), [16, 0]);
});

test('pointing is on unless turned off: its bit tells the game to refuse the key', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const { Interactions } = host.api;
  const player = report(7);
  assert.equal(Interactions.get('pointing'), true);
  assert.equal(player.interactionAllowed('pointing'), true);
  assert.deepEqual(rules(sent, 7), [], 'pointing on: nothing to tell');
  assert.equal(player.setInteraction('pointing', false), true);
  assert.equal(player.interactionAllowed('pointing'), false);
  player.setInteraction('pointing', null);
  assert.deepEqual(rules(sent, 7), [32, 0]);
});

test('a player allowed in private areas: setTrespassAllowed sets their own rule', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const { Interactions } = host.api;
  const guard = report(7);
  const visitor = report(8);
  assert.equal(guard.trespassAllowed, false, "the game's rule by default");
  assert.equal(guard.setTrespassAllowed(true), true);
  assert.equal(guard.trespassAllowed, true);
  assert.equal(visitor.trespassAllowed, false, 'only that player');
  assert.deepEqual(rules(sent, 7), [64]);
  assert.deepEqual(rules(sent, 8), []);

  assert.throws(() => guard.setTrespassAllowed('yes'), TypeError);
  guard.setTrespassAllowed(null);
  assert.equal(guard.trespassAllowed, false, "null: the server's setting");
  Interactions.set('trespass', true);
  assert.equal(guard.trespassAllowed, true, 'the whole server');
  assert.equal(visitor.trespassAllowed, true);
  guard.setTrespassAllowed(false);
  assert.equal(guard.trespassAllowed, false, 'their own setting wins');
  assert.deepEqual(rules(sent, 7), [64, 0, 64, 0]);
});

test('npcTalk and npcChat can never be turned on', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const { Interactions } = host.api;
  const player = report(7);
  Interactions.set('npcTalk', true);
  Interactions.set('npcChat', true);
  assert.equal(Interactions.get('npcTalk'), false);
  assert.equal(Interactions.get('npcChat'), false);
  assert.equal(player.setInteraction('npcTalk', true), true);
  assert.equal(player.setInteraction('npcChat', true), true);
  assert.equal(player.interactionAllowed('npcTalk'), false);
  assert.equal(player.interactionAllowed('npcChat'), false);
  assert.deepEqual(rules(sent, 7), [], 'nothing allowed: nothing is sent');
});
