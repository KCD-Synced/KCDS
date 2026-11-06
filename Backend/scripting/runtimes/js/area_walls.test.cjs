'use strict';

// Area collision walls: the pieces of each shape, and the wall props an area puts up,
// moves and takes down with it.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { geometryOf } = require('./areas/area_shapes.cjs');
const { wallPieces } = require('./areas/area_walls.cjs');

const options = (more = {}) => ({ floor: false, ceiling: false, thickness: 0.2, ...more });

test('a box gets four side walls, and a floor and a ceiling when asked', () => {
  const box = geometryOf({ type: 'box', min: { x: -5, y: -2, z: 0 }, max: { x: 5, y: 2, z: 3 } });
  const walls = wallPieces(box, options());
  assert.equal(walls.length, 4);
  const east = walls.find((piece) => piece.center.x === 5);
  assert.deepEqual(east.center, { x: 5, y: 0, z: 1.5 });
  assert.deepEqual(east.half, { x: 0.1, y: 2.1, z: 1.5 });
  const closed = wallPieces(box, options({ floor: true, ceiling: true }));
  assert.equal(closed.length, 6);
  const floor = closed.find((piece) => piece.center.z === 0);
  assert.deepEqual(floor.half, { x: 5.1, y: 2.1, z: 0.1 });
  assert.throws(() => wallPieces(box, options({ thickness: 4 })), RangeError);
});

test('a long wall is cut into parts the cube can be scaled to', () => {
  const box = geometryOf({ type: 'box', min: { x: 0, y: 0, z: 0 }, max: { x: 400, y: 10, z: 5 } });
  const south = wallPieces(box, options()).filter((piece) => piece.center.y === 0);
  assert.equal(south.length, 3);
  assert.ok(south.every((piece) => piece.half.x <= 95));
});

test('a shape gets a wall on each edge and strips across its floor', () => {
  const shape = geometryOf({
    type: 'shape',
    points: [
      { x: 0, y: 0, z: 0 },
      { x: 10, y: 0, z: 0 },
      { x: 10, y: 4, z: 0 },
      { x: 0, y: 4, z: 0 },
    ],
    height: 3,
  });
  const walls = wallPieces(shape, options());
  assert.equal(walls.length, 4);
  const bottom = walls.find((piece) => Math.abs(piece.center.y) < 1e-9);
  assert.equal(bottom.center.z, 1.5);
  assert.ok(Math.abs(bottom.half.x - 5.1) < 1e-9);
  const floored = wallPieces(shape, options({ floor: true }));
  assert.equal(floored.length - walls.length, 4, 'one strip per metre of its depth');
  const crossed = geometryOf({
    type: 'shape',
    points: [
      { x: 0, y: 0, z: 0 },
      { x: 10, y: 10, z: 0 },
      { x: 10, y: 0, z: 0 },
      { x: 0, y: 10, z: 0 },
    ],
    height: 3,
  });
  assert.throws(() => wallPieces(crossed, options()), /simple outline/);
  const flat = geometryOf({ type: 'shape', points: shape.points, height: 0 });
  assert.throws(() => wallPieces(flat, options()), /height/);
});

test('a sphere gets a closed shell of tangent panels', () => {
  const sphere = geometryOf({ type: 'sphere', position: { x: 100, y: 0, z: 50 }, radius: 6 });
  const shell = wallPieces(sphere, options());
  assert.ok(shell.length > 20);
  for (const piece of shell) {
    const away = Math.hypot(piece.center.x - 100, piece.center.y, piece.center.z - 50);
    assert.ok(Math.abs(away - 6) < 1e-6, 'every panel sits on the sphere');
    const { w, x, y, z } = piece.rotation;
    assert.ok(Math.abs(w * w + x * x + y * y + z * z - 1) < 1e-9);
  }
});

test('an area puts its wall props up, moves them with its shape and takes them down', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-walls-'));
  const sent = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (message) => sent.push(message),
    log: () => {},
  });
  await host.startAll();
  const { Area, Prop } = host.api;
  const walls = () => sent.filter((m) => m.op === 'propSpawn' && m.physics === 'wall');
  const area = Area.create({
    id: 'yard',
    type: 'box',
    min: { x: -5, y: -5, z: 0 },
    max: { x: 5, y: 5, z: 3 },
    collision: true,
  });
  assert.equal(area.collision, true);
  assert.equal(area.wallThickness, 0.2);
  assert.equal(walls().length, 4);
  const first = walls()[0];
  assert.equal(first.model, 'objects/special/primitive_cube.cgf');
  assert.equal(first.dimension, 0xffffffff);
  assert.deepEqual(Prop.all(), [], 'walls are no props of the scripts');
  sent.length = 0;
  area.wallTop = true;
  assert.equal(sent.filter((m) => m.op === 'propDestroy').length, 4);
  assert.equal(walls().length, 5);
  assert.deepEqual(area.toJSON().wallTop, true);
  sent.length = 0;
  area.enabled = false;
  assert.equal(sent.filter((m) => m.op === 'propDestroy').length, 5);
  assert.equal(walls().length, 0);
  area.enabled = true;
  assert.equal(walls().length, 5);
  assert.throws(() => (area.wallThickness = 9), RangeError);
  assert.equal(area.setShape({ max: { x: -4.9, y: 5, z: 3 } }), false, 'too small for its walls');
  sent.length = 0;
  area.destroy();
  assert.equal(sent.filter((m) => m.op === 'propDestroy').length, 5);
  const level = Area.all('level')[0];
  if (level) {
    assert.throws(() => (level.collision = true), TypeError);
  }
});
