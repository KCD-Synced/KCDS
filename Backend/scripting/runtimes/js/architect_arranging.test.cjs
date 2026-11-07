'use strict';

// World Architect's furniture layouts (Client/resources/architect/arranging.cjs) and catalogue
// tags (tags.cjs).
const test = require('node:test');
const assert = require('node:assert/strict');

const {
  rowSpots,
  gridSpots,
  aroundSpots,
  blockSpots,
  turnToward,
} = require('../../../../Client/resources/architect/arranging.cjs');
const {
  tagsOf,
  matchesSearch,
  TAG_NAMES,
  ROOMS,
} = require('../../../../Client/resources/architect/tags.cjs');
const {
  TAG_NAMES: TOOL_TAGS,
  ROOM_KINDS,
} = require('../../../../Dev/tools/architect-mcp/tools.cjs');

test("a prop's front (-y) is turned to face a direction", () => {
  assert.equal(turnToward(0, -1), 0);
  assert.equal(turnToward(1, 0), 90);
  assert.equal(turnToward(0, 1), 180);
  assert.equal(turnToward(-1, 0), 270);
});

test('a row runs evenly from one end to the other, or spacing apart along a direction', () => {
  const pews = rowSpots({
    from: { x: 0, y: 0, z: 5 },
    to: { x: 0, y: 9, z: 5 },
    count: 4,
    turn: 180,
  });
  assert.deepEqual(
    pews.map((spot) => [spot.at.x, spot.at.y, spot.at.z, spot.turn]),
    [
      [0, 0, 5, 180],
      [0, 3, 5, 180],
      [0, 6, 5, 180],
      [0, 9, 5, 180],
    ],
  );
  const torches = rowSpots({ from: { x: 1, y: 1 }, direction: 90, spacing: 2, count: 3 });
  assert.deepEqual(
    torches.map((spot) => [spot.at.x, spot.at.y]),
    [
      [1, 1],
      [1, 3],
      [1, 5],
    ],
  );
  assert.equal(torches[0].at.z, undefined, 'no height asked: the ground decides');
  // Each faces the altar when toward is given.
  const facing = rowSpots({
    from: { x: -2, y: 0 },
    to: { x: 2, y: 0 },
    count: 2,
    toward: { x: -2, y: -5 },
  });
  assert.equal(facing[0].turn, 0);
  assert.throws(() => rowSpots({ from: { x: 0, y: 0 }, count: 3 }), /spacing/);
  assert.throws(() => rowSpots({ from: { x: 0, y: 0 }, to: { x: 1, y: 0 }, count: 0 }), /count/);
});

test('a grid keeps an aisle free down its middle and turns as a whole', () => {
  const nave = gridSpots({ at: { x: 0, y: 0 }, rows: 2, columns: 4, spacing: [1, 2], aisle: 2 });
  const xs = [...new Set(nave.map((spot) => spot.at.x))].sort((a, b) => a - b);
  // Two columns either side, the aisle 2 m wider than the spacing between them.
  assert.deepEqual(xs, [-2.5, -1.5, 1.5, 2.5]);
  assert.deepEqual([...new Set(nave.map((spot) => spot.at.y))], [-1, 1]);
  const turned = gridSpots({ at: { x: 10, y: 0 }, rows: 1, columns: 2, spacing: 2, turn: 90 });
  assert.deepEqual(
    turned.map((spot) => [spot.at.x, spot.at.y, spot.turn]),
    [
      [10, -1, 90],
      [10, 1, 90],
    ],
  );
  assert.throws(() => gridSpots({ at: { x: 0, y: 0 }, rows: 9, columns: 9 }), /at most 64/);
});

test('a ring faces its middle (chairs at a table), or away from it', () => {
  const chairs = aroundSpots({ at: { x: 0, y: 0, z: 1 }, count: 4, radius: 1 });
  // The chair east of the table faces west (270), the one north of it faces south (0).
  assert.deepEqual(
    chairs.map((spot) => [Math.round(spot.at.x) + 0, Math.round(spot.at.y) + 0, spot.turn]),
    [
      [1, 0, 270],
      [0, 1, 0],
      [-1, 0, 90],
      [0, -1, 180],
    ],
  );
  const out = aroundSpots({ at: { x: 0, y: 0 }, count: 1, radius: 2, face: 'out' });
  assert.equal(out[0].turn, 90);
});

test('a block of copies starts at the thing and runs to its right, its rows behind it', () => {
  const block = blockSpots({
    at: { x: 0, y: 0, z: 2 },
    turn: 90,
    count: 4,
    columns: 2,
    spacing: 1,
  });
  assert.deepEqual(
    block.map((spot) => [Math.round(spot.at.x), Math.round(spot.at.y), spot.at.z]),
    [
      [0, 0, 2],
      [0, 1, 2],
      [-1, 0, 2],
      [-1, 1, 2],
    ],
  );
});

test("tags say what a model is for, from its path's words", () => {
  assert.deepEqual(tagsOf('objects/manmade/common_furniture/chairs/high/chair_a.cgf'), ['seating']);
  assert.deepEqual(tagsOf('objects/manmade/common_illumination/candlestick_a.cgf'), ['lighting']);
  assert.deepEqual(
    tagsOf('objects/manmade/task_specific_props/metal_industry/smithing/anvil.cgf'),
    ['tools', 'smithy'],
  );
  assert.deepEqual(tagsOf('objects/manmade/task_specific_props/religious/chalice_gold_a.cgf'), [
    'religious',
  ]);
  // A word inside another is no match: a pewter jug is no pew.
  assert.deepEqual(tagsOf('objects/manmade/household/jugs/jug_pewter.cgf'), ['kitchen']);
  assert.ok(matchesSearch('objects/a/chairs/chair_b.cgf', 'seating'));
  assert.ok(matchesSearch('objects/a/religious/chalice.cgf', 'church'));
  // A room takes a thing by its main tag: a bed with its pillows is no church cloth.
  assert.ok(!matchesSearch('objects/a/beds/high/bed_fancy_a_low_pillow.cgf', 'church'));
  assert.ok(matchesSearch('objects/a/beds/high/bed_fancy_a_low_pillow.cgf', 'bedroom'));
  assert.ok(!matchesSearch('objects/a/food/bread.cgf', 'church'));
  assert.ok(matchesSearch('objects/a/food/bread.cgf', 'bre'));
  for (const tags of Object.values(ROOMS)) {
    assert.ok(tags.every((tag) => TAG_NAMES.includes(tag)));
  }
});

test("the MCP server's tool lists the same tags and kinds of room as the editor", () => {
  assert.deepEqual(TOOL_TAGS, TAG_NAMES);
  assert.deepEqual(ROOM_KINDS, Object.keys(ROOMS));
});
