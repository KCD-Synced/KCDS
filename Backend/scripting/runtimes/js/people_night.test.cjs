'use strict';

// Where the level's people spend the night (server.json npcHouses,
// npcs/people_night.cjs).
const test = require('node:test');
const assert = require('node:assert/strict');
const { PeopleNight, isNight, settingsOf } = require('./npcs/people_night.cjs');
const EXITS = require('./npcs/level_exits.json');

function walker(id, x, y) {
  const orders = [];
  return {
    orders,
    npc: {
      id,
      position: { x, y, z: 0 },
      moveTo(position, options) {
        orders.push([position, options.speed]);
        return true;
      },
    },
  };
}

test('people night: the night runs from leaveHour over midnight to returnHour', () => {
  const settings = { leaveHour: 21, returnHour: 6 };
  assert.equal(isNight(22, settings), true);
  assert.equal(isNight(3.5, settings), true);
  assert.equal(isNight(6, settings), false);
  assert.equal(isNight(12, settings), false);
  assert.equal(isNight(2, { leaveHour: 1, returnHour: 4 }), true);
  assert.equal(isNight(5, { leaveHour: 1, returnHour: 4 }), false);
  assert.throws(() => settingsOf({ leaveHour: 24 }), RangeError);
});

test('people night: both maps have the exits their roads leave by', () => {
  assert.ok(EXITS.trosecko.length >= 4);
  assert.ok(EXITS.kutnohorsko.length >= 4);
  for (const exit of [...EXITS.trosecko, ...EXITS.kutnohorsko]) {
    assert.match(exit.name, /^levelExit_\d+$/);
    assert.ok(Number.isFinite(exit.position.x) && Number.isFinite(exit.position.y));
  }
});

test('people night: without houses they walk to the nearest exit at night and go there', () => {
  let hour = 22;
  let now = 0;
  const night = new PeopleNight({
    houses: false,
    hour: () => hour,
    level: () => 'trosecko',
    now: () => now,
  });
  const first = EXITS.trosecko[0].position;
  const near = walker(1, first.x + 50, first.y);
  const there = walker(2, first.x + 2, first.y);
  assert.equal(night.away(), true);
  const gone = night.tick([near, there]);
  assert.deepEqual(gone, [there.npc]);
  assert.deepEqual(near.orders, [[first, 'walk']]);
  // The walk is not asked again at once, but after a while (a body that changed games).
  night.tick([near]);
  assert.equal(near.orders.length, 1);
  now += 20000;
  night.tick([near]);
  assert.equal(near.orders.length, 2);
  // Morning: they stay.
  hour = 7;
  assert.equal(night.away(), false);
  assert.deepEqual(night.tick([near]), []);
});

test('people night: with houses (the default) nothing happens at night', () => {
  const night = new PeopleNight({ houses: true, hour: () => 23, level: () => 'trosecko' });
  const person = walker(1, 0, 0);
  assert.equal(night.away(), false);
  assert.deepEqual(night.tick([person]), []);
  assert.equal(person.orders.length, 0);
  // A level without exits keeps its people too.
  const nowhere = new PeopleNight({ houses: false, hour: () => 23, level: () => 'kcds_studio' });
  assert.equal(nowhere.away(), false);
});
