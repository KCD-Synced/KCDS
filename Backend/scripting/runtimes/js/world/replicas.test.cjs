'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { GLOBAL_WORLD } = require('../npcs/npc_values.cjs');
const { replicasAround } = require('./replicas.cjs');

function kind(...entities) {
  return { all: () => entities };
}

function thing(name, x, dimension = GLOBAL_WORLD) {
  return { name, position: { x, y: 0, z: 0 }, dimension };
}

function api(overrides = {}) {
  return {
    Player: kind(),
    Horse: kind(),
    Npc: kind(),
    Dog: kind(),
    Prop: kind(),
    GroundItem: kind(),
    Door: kind(),
    Stash: kind(),
    ...overrides,
  };
}

test('World.replicasAround lists every kind, nearest first, in one dimension', () => {
  const near = thing('near', 1);
  const middle = thing('middle', 5);
  const far = thing('far', 50);
  const elsewhere = thing('elsewhere', 1, 7);
  const nowhere = { name: 'nowhere', position: null, dimension: GLOBAL_WORLD };
  const world = api({
    Player: kind(middle, nowhere),
    Horse: kind(far),
    Prop: kind(near, elsewhere),
  });
  const origin = { x: 0, y: 0, z: 0 };
  assert.deepEqual(replicasAround(world, origin, 10), [near, middle]);
  assert.deepEqual(replicasAround(world, origin, 10, 7), [elsewhere]);
  assert.deepEqual(replicasAround(world, origin, 0), []);
});

test('World.replicasAround refuses what it cannot use', () => {
  const world = api();
  const origin = { x: 0, y: 0, z: 0 };
  assert.throws(() => replicasAround(world, null, 5), TypeError);
  assert.throws(() => replicasAround(world, { x: NaN, y: 0, z: 0 }, 5), TypeError);
  assert.throws(() => replicasAround(world, origin, -1), TypeError);
  assert.throws(() => replicasAround(world, origin, 5, 0.5), TypeError);
});
