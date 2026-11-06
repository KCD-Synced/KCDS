'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { areasOfDoor, doorsOfArea } = require('./area_doors.cjs');

// A house of trosecko, with a door found in the game by this guid.
const AREA = '006588875df00335';
const DOOR = '4cb8c80af3380862';

test('the doors a level links to an area, and the areas of a door', () => {
  const rows = doorsOfArea('trosecko', AREA);
  assert.equal(rows.length, 4);
  const door = rows.find((row) => row.guid === DOOR);
  assert.equal(door.link, 'entrance');
  assert.ok(door.name.startsWith('AnimDoor'));
  assert.ok(Number.isFinite(door.position.x));
  assert.deepEqual(areasOfDoor('trosecko', DOOR), [AREA]);
});

test('an unknown area, door or level has none, and a caller cannot change the catalog', () => {
  assert.deepEqual(doorsOfArea('trosecko', 'ffffffffffffffff'), []);
  assert.deepEqual(doorsOfArea('nowhere', AREA), []);
  assert.deepEqual(doorsOfArea(null, AREA), []);
  assert.deepEqual(areasOfDoor('trosecko', 'ffffffffffffffff'), []);
  assert.deepEqual(areasOfDoor('nowhere', DOOR), []);
  const [first] = doorsOfArea('trosecko', AREA);
  first.position.x = 0;
  first.link = 'changed';
  const [again] = doorsOfArea('trosecko', AREA);
  assert.notEqual(again.position.x, 0);
  assert.notEqual(again.link, 'changed');
});
