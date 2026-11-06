'use strict';

// World Architect's open_model (Dev/tools/architect-mcp/models/open_model.cjs): a closed building
// of the game copied without the physics boxes that fill its inside.
const test = require('node:test');
const assert = require('node:assert/strict');
const { writeCgf } = require('../../../../Dev/tools/architect-mcp/models/cgf_writer.cjs');
const {
  openBytes,
  boxProxies,
} = require('../../../../Dev/tools/architect-mcp/models/open_model.cjs');

// A one-triangle model with a big box filling it and a thin wall box beside it.
function closedHouse() {
  const vertex = (p) => ({ p, uv: [0, 0], t: [1, 0, 0], b: [0, 1, 0], w: 1 });
  const piece = {
    name: 'house',
    at: [0, 0, 0],
    vertices: [vertex([0, 0, 0]), vertex([10, 0, 0]), vertex([0, 10, 0])],
    indices: [0, 1, 2],
    subsets: [
      {
        firstIndex: 0,
        indices: 3,
        firstVertex: 0,
        vertices: 3,
        material: 1,
        radius: 7,
        centre: [3, 3, 0],
      },
    ],
    min: [0, 0, 0],
    max: [10, 10, 0],
  };
  return writeCgf({
    pieces: [piece],
    material: 'objects/test/house',
    subMaterials: ['proxy', 'walls'],
    boxes: [
      { centre: [5, 5, 4], half: [5, 5, 4] },
      { centre: [11, 5, 4], half: [0.5, 5, 4] },
    ],
  });
}

test('the boxes that fill a building go far under the ground, the walls stay', () => {
  const bytes = closedHouse();
  const before = boxProxies(bytes);
  assert.equal(before.length, 2);
  const { copy, removed, kept } = openBytes(bytes, { thinnest: 2.5 });
  assert.equal(copy.length, bytes.length);
  assert.equal(removed.length, 1);
  assert.deepEqual(removed[0].size, [10, 10, 8]);
  assert.equal(kept.length, 1);
  const after = boxProxies(copy);
  const gone = after.find((box) => box.name === removed[0].name);
  assert.ok(gone.centre[2] < -1000);
  assert.ok(gone.half.every((value) => value < 0.1));
  const wall = after.find((box) => box.name !== removed[0].name);
  assert.deepEqual(
    wall.half.map((value) => Math.round(value * 10) / 10),
    [0.5, 5, 4],
  );
});

test('named boxes only are removed when remove is given', () => {
  const bytes = closedHouse();
  const names = boxProxies(bytes).map((box) => box.name);
  const { removed } = openBytes(bytes, { remove: [names[1]] });
  assert.deepEqual(
    removed.map((box) => box.name),
    [names[1]],
  );
});

test('a turned box is as large along the model axes as the game turns it, and is kept turned', () => {
  const vertex = (p) => ({ p, uv: [0, 0], t: [1, 0, 0], b: [0, 1, 0], w: 1 });
  const piece = {
    name: 'porch',
    at: [0, 0, 0],
    vertices: [vertex([0, 0, 0]), vertex([1, 0, 0]), vertex([0, 1, 0])],
    indices: [0, 1, 2],
    subsets: [
      {
        firstIndex: 0,
        indices: 3,
        firstVertex: 0,
        vertices: 3,
        material: 1,
        radius: 1,
        centre: [0, 0, 0],
      },
    ],
    min: [0, 0, 0],
    max: [1, 1, 0],
  };
  // The game's basis turns the model's frame into the box's own: its rows are the box's axes.
  // These rows say the box's x is the model's z, its y the model's x and its z the model's y
  // (one of the church's porch walls).
  const basis = [0, 0, 1, 1, 0, 0, 0, 1, 0];
  const bytes = writeCgf({
    pieces: [piece],
    material: 'objects/test/porch',
    subMaterials: ['proxy', 'walls'],
    boxes: [{ centre: [0, 0, 5], half: [1.75, 3.75, 0.75], basis, extent: [3.75, 0.75, 1.75] }],
  });
  const [box] = boxProxies(bytes);
  assert.deepEqual(box.basis, basis);
  assert.deepEqual(
    box.extent.map((value) => Math.round(value * 100) / 100),
    [3.75, 0.75, 1.75],
  );
  const { kept, keptShapes } = openBytes(bytes, { thinnest: 2.5 });
  assert.deepEqual(kept[0].size, [7.5, 1.5, 3.5]);
  assert.deepEqual(keptShapes[0].basis, basis);
  assert.deepEqual(
    keptShapes[0].half.map((value) => Math.round(value * 100) / 100),
    [1.75, 3.75, 0.75],
  );
});
