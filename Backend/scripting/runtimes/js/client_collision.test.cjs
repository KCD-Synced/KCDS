'use strict';

// The client Collision (Client/resources/collision.cjs, World Architect's Collision layer): invisible
// solid boxes in this game only, as localCollision orders.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceCollision } = require('../../../../Client/resources/collision.cjs');
const { Vector3 } = require('./math.cjs');

function setup() {
  const sent = [];
  const registry = new ResourceCollision({ send: (message) => sent.push(message) });
  return { registry, sent, Collision: registry.facade('alpha') };
}

test('addBox sends the box with half sizes; set changes only what it names', () => {
  const { Collision, sent } = setup();
  const wall = Collision.addBox({
    center: new Vector3(10, 20, 31.5),
    size: new Vector3(6, 0.3, 3),
    rotation: { x: 0, y: 0, z: 90 },
  });
  assert.equal(sent[0].op, 'localCollision');
  assert.equal(sent[0].kind, 'set');
  assert.equal(sent[0].id, wall.id);
  assert.deepEqual(sent[0].center, { x: 10, y: 20, z: 31.5 });
  assert.deepEqual(sent[0].half, { x: 3, y: 0.15, z: 1.5 });
  assert.ok(Math.abs(sent[0].rotation.w - Math.SQRT1_2) < 1e-6);

  wall.set({ size: new Vector3(8, 0.3, 3) });
  assert.deepEqual(sent[1].half, { x: 4, y: 0.15, z: 1.5 });
  assert.deepEqual(sent[1].center, { x: 10, y: 20, z: 31.5 });
  assert.equal(wall.size.x, 8);
});

test('bad boxes are refused', () => {
  const { Collision } = setup();
  assert.throws(
    () => Collision.addBox({ center: { x: 1 }, size: new Vector3(1, 1, 1) }),
    TypeError,
  );
  assert.throws(
    () => Collision.addBox({ center: new Vector3(0, 0, 0), size: new Vector3(0, 1, 1) }),
    RangeError,
  );
  assert.throws(
    () => Collision.addBox({ center: new Vector3(0, 0, 0), size: new Vector3(1, 1, 500) }),
    RangeError,
  );
});

test('remove takes a box away once; a stopped resource takes its boxes', () => {
  const { registry, Collision, sent } = setup();
  const other = registry.facade('beta');
  const box = Collision.addBox({ center: new Vector3(0, 0, 0), size: new Vector3(1, 1, 1) });
  other.addBox({ center: new Vector3(5, 0, 0), size: new Vector3(1, 1, 1) });
  box.remove();
  box.remove();
  assert.equal(sent.filter((message) => message.kind === 'remove').length, 1);
  assert.equal(box.valid, false);
  assert.equal(box.size.x, 1);
  assert.equal(Collision.all().length, 0);
  assert.equal(other.all().length, 1);
  registry.removeOwner('beta');
  assert.equal(other.all().length, 0);
});
