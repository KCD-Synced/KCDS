'use strict';

// World Architect's picks of many things (Client/resources/architect/box_pick.cjs).

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  groundSpan,
  insideStageBox,
  insideTurnedBox,
  sameThing,
  mergeGroup,
} = require('../../../../Client/resources/architect/box_pick.cjs');

test('the ground a box covers: the middle of what the camera sees and a radius reaching it all', () => {
  const span = groundSpan(
    [{ x: 0, y: 0, z: 10 }, { x: 20, y: 0, z: 10 }, null, { x: 10, y: 0, z: 12 }],
    2,
    500,
  );
  assert.equal(span.centre.x, 10);
  assert.equal(span.centre.y, 0);
  assert.ok(Math.abs(span.centre.z - 32 / 3) < 1e-9);
  assert.equal(span.radius, 12);
  // Capped, and none when the camera sees no ground in the box (sky only).
  assert.equal(
    groundSpan(
      [
        { x: 0, y: 0, z: 0 },
        { x: 900, y: 0, z: 0 },
      ],
      2,
      200,
    ).radius,
    200,
  );
  assert.equal(groundSpan([null, null], 2, 200), null);
});

test('a stage point in the box; one behind the camera never', () => {
  const box = { minX: 10, maxX: 100, minY: 20, maxY: 80 };
  assert.ok(insideStageBox({ x: 10, y: 80 }, box));
  assert.ok(!insideStageBox({ x: 9, y: 50 }, box));
  assert.ok(!insideStageBox(null, box));
});

test('inside a turned building box, its own frame and margin', () => {
  // 10 m along its own x, 4 m along its own y, turned 90 degrees: x of the world is its y.
  const box = { at: { x: 100, y: 100, z: 50 }, turn: 90, min: [-5, -2, 0], max: [5, 2, 6] };
  assert.ok(insideTurnedBox(box, { x: 101, y: 104, z: 51 }));
  assert.ok(!insideTurnedBox(box, { x: 104, y: 101, z: 51 }));
  assert.ok(insideTurnedBox(box, { x: 102.4, y: 100, z: 51 }, 0.5));
  // Under the floor or over the roof: not in it.
  assert.ok(!insideTurnedBox(box, { x: 100, y: 100, z: 57 }));
  assert.ok(!insideTurnedBox(box, { x: 100, y: 100, z: 49 }));
});

test("the level's same object found twice is one thing; the group holds each once", () => {
  const object = { kind: 'brush', name: 'objects/a.cgf' };
  const first = { level: true, object, at: { x: 1, y: 2, z: 3 } };
  const again = { level: true, object: { ...object }, at: { x: 1.01, y: 2, z: 3 } };
  const other = { level: true, object: { kind: 'brush', name: 'objects/b.cgf' }, at: first.at };
  const yours = { entry: { source: 'prop' }, at: first.at };
  assert.ok(sameThing(first, again));
  assert.ok(!sameThing(first, other));
  assert.ok(!sameThing(yours, { entry: { source: 'prop' }, at: first.at }));
  assert.deepEqual(mergeGroup([first], [again, other, yours], true), [first, other, yours]);
  assert.deepEqual(mergeGroup([first], [other], false), [other]);
});
