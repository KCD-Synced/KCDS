'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { landmarks } = require('./points_of_interest.cjs');

test('World.landmarks lists a level, and nothing for another', () => {
  const points = landmarks('trosecko');
  assert.ok(points.length > 10);
  for (const point of points) {
    assert.equal(point.level, 'trosecko');
    assert.equal(typeof point.name, 'string');
    assert.ok(point.position.x > 0 && point.position.y > 0);
  }
  assert.deepEqual(landmarks('klaster'), []);
  points[0].position.x = -1;
  assert.notEqual(landmarks('trosecko')[0].position.x, -1);
});
