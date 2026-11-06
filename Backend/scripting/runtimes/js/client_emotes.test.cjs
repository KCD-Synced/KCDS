'use strict';

// The client's EmoteWheel global: the shipped catalog and the wheel's slots.
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  EmoteWheelState,
  CATALOG,
  STOCK_WHEEL,
} = require('../../../../Client/resources/emotes.cjs');
const gameCatalog = require('./animation/catalog.json');

test('every gesture is a game animation on both bodies, with its duration and body part', () => {
  const MALE = 4;
  const FEMALE = 8;
  const FULL_BODY = 2;
  const rows = new Map(gameCatalog.animations.map((row) => [`${row[0]}|${row[1]}`, row]));
  for (const entry of CATALOG) {
    const male = rows.get(`${entry.fragment}|${entry.tags}`);
    const female = rows.get(`${entry.fragment}|${entry.femaleTags ?? entry.tags}`);
    assert.ok(male && (male[3] & MALE) !== 0, `${entry.name} plays on a man`);
    assert.ok(female && (female[3] & FEMALE) !== 0, `${entry.name} plays on a woman`);
    assert.equal(entry.duration, male[2], entry.name);
    assert.equal(entry.fullBody, (male[3] & FULL_BODY) !== 0, entry.name);
  }
  assert.equal(new Set(CATALOG.map((entry) => entry.id)).size, CATALOG.length);
});

test('the catalog shows each emote with its game animation, fresh each call', () => {
  const wheel = new EmoteWheelState().facade();
  const catalog = wheel.getCatalog();
  assert.deepEqual(catalog[0], {
    id: 1,
    name: 'Wave',
    duration: 2.833,
    fullBody: false,
    fragment: 'GreetingsUpperBody',
    tags: 'l_noweapon+waveSmall',
    femaleTags: 'l_noweapon+waveSmall',
  });
  // The nods take other tags on a woman's body (npc.playAnimation plays them).
  assert.equal(catalog[2].tags, 'l_noweapon+nod');
  assert.equal(catalog[2].femaleTags, 'nod');
  assert.deepEqual(
    catalog.map((entry) => entry.name),
    CATALOG.map((entry) => entry.name),
  );
  catalog[0].name = 'changed';
  assert.equal(wheel.getCatalog()[0].name, 'Wave');
});

test('the stock wheel is wave, big wave, nod, respectful nod, bow, cheer', () => {
  const wheel = new EmoteWheelState().facade();
  assert.equal(wheel.slotTotal(), 6);
  const names = new Map(wheel.getCatalog().map((entry) => [entry.id, entry.name]));
  assert.deepEqual(
    wheel.getSlots().map((id) => names.get(id)),
    ['Wave', 'Big wave', 'Nod', 'Respectful nod', 'Bow', 'Cheer'],
  );
  assert.deepEqual(wheel.getSlots(), [...STOCK_WHEEL]);
});

test('assignSlot and assignSlots change nothing on a bad slot, id or length', () => {
  const wheel = new EmoteWheelState().facade();
  assert.equal(wheel.assignSlot(0, 8), true);
  assert.equal(wheel.assignSlot(1, 8), true);
  assert.deepEqual(wheel.getSlots(), [8, 8, 3, 4, 5, 6]);
  assert.equal(wheel.assignSlot(6, 1), false);
  assert.equal(wheel.assignSlot(-1, 1), false);
  assert.equal(wheel.assignSlot(1.5, 1), false);
  assert.equal(wheel.assignSlot(0, 99), false);
  assert.equal(wheel.assignSlots([1, 2, 3]), false);
  assert.equal(wheel.assignSlots([1, 2, 3, 4, 5, 99]), false);
  assert.equal(wheel.assignSlots('123456'), false);
  assert.deepEqual(wheel.getSlots(), [8, 8, 3, 4, 5, 6]);
  assert.equal(wheel.assignSlots([8, 7, 6, 5, 4, 3]), true);
  assert.deepEqual(wheel.getSlots(), [8, 7, 6, 5, 4, 3]);
  const slots = wheel.getSlots();
  slots[0] = 1;
  assert.equal(wheel.getSlots()[0], 8);
  wheel.reset();
  assert.deepEqual(wheel.getSlots(), [...STOCK_WHEEL]);
});

test('a gesture names the game animation it plays', () => {
  const state = new EmoteWheelState();
  assert.deepEqual(state.animationOf(5), { fragment: 'Greetings', tags: 'bowBig' });
  assert.deepEqual(state.animationOf(3), {
    fragment: 'GreetingsUpperBody',
    tags: 'l_noweapon+nod',
  });
  assert.deepEqual(state.animationOf(3, 'female'), { fragment: 'GreetingsUpperBody', tags: 'nod' });
  assert.equal(state.animationOf(99), null);
});
