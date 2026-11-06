'use strict';

// Animations: the game's own animation rows and hand props.
const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

test('Animations.list answers the game rows with their metadata, filtered by a search', async (t) => {
  const { host } = serverFixture(t);
  await host.startAll();
  const { Animations } = host.makeAPI();
  const all = Animations.list();
  assert.ok(all.length > 6000);
  const fragments = all.map((row) => row.fragment);
  assert.deepEqual(fragments, [...fragments].sort());
  const mill = Animations.list('bakermill');
  const loop = mill.find((row) => row.fragment === 'BakerMillLoop');
  assert.equal(loop.sequence, 'BakerMill');
  assert.equal(loop.phase, 'loop');
  assert.equal(typeof loop.duration, 'number');
  assert.equal(typeof loop.oneShot, 'boolean');
  assert.ok(loop.male || loop.female);
  assert.ok(mill.every((row) => row.fragment.toLowerCase().includes('bakermill')));
  const bartender = Animations.list('Bartender_ServeBeer');
  assert.ok(bartender.length > 0 && bartender.every((row) => row.female));
  // Fresh objects every call.
  loop.fragment = 'changed';
  assert.ok(Animations.list('bakermillloop').every((row) => row.fragment !== 'changed'));
  assert.throws(() => Animations.list(5), /search text/);
});

test('Animations.props answers the hand tools sorted by name', async (t) => {
  const { host } = serverFixture(t);
  await host.startAll();
  const { Animations } = host.makeAPI();
  const props = Animations.props();
  assert.ok(props.length > 100);
  const names = props.map((prop) => prop.name);
  assert.deepEqual(names, [...names].sort());
  assert.ok(props.every((prop) => prop.model && prop.hand && prop.hand !== 'default'));
  const hands = new Set(Animations.list().flatMap((row) => [row.rightHand, row.leftHand]));
  assert.ok(props.some((prop) => hands.has(prop.hand)));
  // An animation that picks its item up from the station names that slot.
  const mash = Animations.list('GetMash');
  assert.equal(mash.length, 3);
  assert.ok(mash.every((row) => row.rightHand === 'scoopSlot' && row.leftHand === ''));
});

test('playAnimation refuses an animation the game has only for the other body', async (t) => {
  const { host, report } = serverFixture(t);
  await host.startAll();
  const { Animations } = host.makeAPI();
  const player = report(3);
  const femaleOnly = Animations.list().find((row) => row.female && !row.male && row.oneShot);
  const maleOnly = Animations.list().find((row) => row.male && !row.female && row.oneShot);
  assert.equal(player.playAnimation(femaleOnly.fragment, { tags: femaleOnly.tags }), false);
  assert.equal(player.playAnimation(maleOnly.fragment, { tags: maleOnly.tags }), true);
  // A fragment the catalog does not list is the game's to judge.
  assert.equal(player.playAnimation('NotInTheCatalogAtAll'), true);
});

test('a woman plays the men work rows, and the women-only rows by their clips', async (t) => {
  const { host, report, sent } = serverFixture(t);
  await host.startAll();
  const { Animations } = host.makeAPI();
  const player = report(5, { appearance: { gender: 'female' } });
  // Only the game's women have it: not in the player's animation database, so its clip plays.
  const tap = Animations.list('Bartender_TakeAndTapStein')[0];
  assert.ok(tap.female && !tap.male && !tap.player);
  assert.equal(player.playAnimation('Bartender_TakeAndTapStein'), true);
  assert.equal(player.playAnimation('PickingStuff'), true);
  const orders = sent.filter((message) => message.op === 'animation' && message.clip);
  assert.deepEqual(
    orders.map((order) => [order.fragment, order.clip, order.loop]),
    [
      ['', tap.clip, false],
      ['', Animations.list('PickingStuff')[0].clip, true],
    ],
  );
  // A man still does not have them.
  const man = report(6);
  assert.equal(man.playAnimation('Bartender_TakeAndTapStein'), false);
  // A man's work animation the player's database has plays on her (clip 511).
  const spigot = Animations.list('BehaviorDrinkBarrelSmallSpigot')[0];
  assert.ok(spigot.male && !spigot.female && spigot.player);
  assert.equal(player.playAnimation('BehaviorDrinkBarrelSmallSpigot', { tags: spigot.tags }), true);
  assert.ok(
    Animations.list()
      .filter((row) => row.male)
      .every((row) => row.player),
  );
});

test('setExpression sends a face order beside the body, kept for a new world', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(4);
  const faces = () => sent.filter((m) => m.op === 'animation' && m.kind === 'face');
  assert.equal(player.setExpression('FE_DialogueSpeaking', { tags: 'happy' }), true);
  assert.equal(player.playAnimation('Greetings', { tags: 'bowBig' }), true);
  assert.deepEqual(faces().at(-1), {
    op: 'animation',
    id: 4,
    kind: 'face',
    request: 0,
    fragment: 'FE_DialogueSpeaking',
    tags: 'happy',
  });
  sent.length = 0;
  host.playerAnimations.arrived(player);
  assert.equal(faces().length, 1);
  assert.equal(player.setExpression(''), true);
  assert.equal(faces().at(-1).fragment, '');
  sent.length = 0;
  host.playerAnimations.arrived(player);
  assert.equal(faces().length, 0);
  assert.throws(() => player.setExpression('FE Default'), /printable/);
  assert.throws(() => player.setExpression('FE_Default', { mood: 'x' }), /option mood/);
});

test('setSpeaking sends the style, the player own style by default, kept for a new world', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(4);
  const talks = () => sent.filter((m) => m.op === 'animation' && m.kind === 'talk');
  assert.equal(player.talking, null);
  assert.equal(player.talkingStyle, 'neutral');
  assert.equal(player.setSpeaking(true), true);
  assert.deepEqual(talks().at(-1), {
    op: 'animation',
    id: 4,
    kind: 'talk',
    request: 0,
    fragment: 'neutral',
  });
  // The player's own style: used at once while talking in it, and by the next setSpeaking.
  assert.equal(player.setTalkingStyle('drunk'), true);
  assert.equal(player.talkingStyle, 'drunk');
  assert.equal(player.talking, 'drunk');
  assert.equal(talks().at(-1).fragment, 'drunk');
  // A style given with setSpeaking wins over the player's own until the next setSpeaking.
  assert.equal(player.setSpeaking(true, 'chew'), true);
  sent.length = 0;
  assert.equal(player.setTalkingStyle('happy'), true);
  assert.equal(talks().length, 0);
  assert.equal(player.talking, 'chew');
  assert.equal(player.setSpeaking(true), true);
  assert.equal(talks().at(-1).fragment, 'happy');
  sent.length = 0;
  host.playerAnimations.arrived(player);
  assert.equal(talks().at(-1).fragment, 'happy');
  // No style resets to normal speech.
  assert.equal(player.setTalkingStyle(), true);
  assert.equal(player.talkingStyle, 'neutral');
  assert.equal(player.talking, 'neutral');
  assert.equal(player.setSpeaking(false), true);
  assert.equal(player.talking, null);
  assert.equal(talks().at(-1).fragment, '');
  sent.length = 0;
  host.playerAnimations.arrived(player);
  assert.equal(talks().length, 0);
  assert.throws(() => player.setSpeaking('yes'), /true or false/);
  assert.throws(() => player.setSpeaking(true, 'whisper'), /style must be one of/);
  assert.throws(() => player.setTalkingStyle('whisper'), /style must be one of/);
});

// the animations a man's or a woman's body plays.
test('Animations.list filters the rows a body of a gender plays', () => {
  const { list } = require('./animation/catalog.cjs');
  const all = list('WoundedLying');
  const female = list('WoundedLying', { gender: 'female' });
  const male = list('WoundedLying', { gender: 'male' });
  assert.ok(male.every((row) => row.male));
  assert.ok(female.every((row) => row.player || (row.female && row.clip !== '')));
  assert.ok(female.length <= all.length && male.length <= all.length);
  assert.ok(list(undefined, { gender: 'female' }).length > 0);
  assert.throws(() => list('x', { gender: 'other' }), TypeError);
  assert.throws(() => list('x', null), TypeError);
});

// a game clip's length by its name or path, null for one no row plays.
test('Animations.clipLength answers a clip by name or path', () => {
  const { clipLength } = require('./animation/catalog.cjs');
  assert.equal(clipLength('bartender_serve_tray_f_01'), 2.133);
  assert.equal(clipLength('Animations/humans/male/bartender/BARTENDER_SERVE_TRAY_F_01.caf'), 2.133);
  assert.equal(clipLength('my_resource_dance'), null);
  assert.throws(() => clipLength(''), TypeError);
});
