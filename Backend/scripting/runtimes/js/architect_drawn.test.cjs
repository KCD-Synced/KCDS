'use strict';

// World Architect's MCP server meets the drawn faces of the author's own models itself
// (Dev/tools/architect-mcp/look/model_mesh.cjs): an opened building keeps no collision inside, so
// no game ray meets its vault.
const test = require('node:test');
const assert = require('node:assert/strict');

const { writeCgf } = require('../../../../Dev/tools/architect-mcp/models/cgf_writer.cjs');
const {
  readTriangles,
  rayMeshes,
  fromHalf,
} = require('../../../../Dev/tools/architect-mcp/look/model_mesh.cjs');
const { nearer } = require('../../../../Dev/tools/architect-mcp/look/drawn_hits.cjs');

// A room's floor (z 0) and ceiling (z 3), 4 x 4 m around the origin, as the importer writes a
// model: the floor drawn with sub-material 1, the ceiling with 2.
function roomModel() {
  const vertex = (x, y, z) => ({ p: [x, y, z], uv: [0, 0], t: [1, 0, 0], b: [0, 1, 0], w: 1 });
  const vertices = [
    vertex(-2, -2, 0),
    vertex(2, -2, 0),
    vertex(2, 2, 0),
    vertex(-2, 2, 0),
    vertex(-2, -2, 3),
    vertex(2, -2, 3),
    vertex(2, 2, 3),
    vertex(-2, 2, 3),
  ];
  const indices = [0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6];
  const subset = (firstIndex, material) => ({
    firstIndex,
    indices: 6,
    firstVertex: 0,
    vertices: 8,
    material,
    radius: 3,
    centre: [0, 0, 1.5],
  });
  return writeCgf({
    pieces: [
      {
        name: 'room',
        at: [0, 0, 0],
        vertices,
        indices,
        subsets: [subset(0, 1), subset(6, 2)],
        min: [-2, -2, 0],
        max: [2, 2, 3],
      },
    ],
    material: 'objects/kcds/test/room',
    subMaterials: ['proxy', 'floor', 'ceiling'],
    boxes: [],
  });
}

test("a model's drawn triangles are read with their sub-materials", () => {
  const mesh = readTriangles(roomModel());
  assert.equal(mesh.materials.length, 4);
  assert.deepEqual([...mesh.materials], [1, 1, 2, 2]);
  assert.deepEqual(mesh.min, [-2, -2, 0]);
  assert.deepEqual(mesh.max, [2, 2, 3]);
  assert.equal(fromHalf(0x3c00), 1);
  assert.equal(fromHalf(0xc000), -2);
});

test('a ray meets the drawn ceiling of a placed, turned model, its normal facing the ray', () => {
  const placed = { mesh: readTriangles(roomModel()), at: { x: 100, y: 50, z: 10 }, turn: 45 };
  const up = rayMeshes([placed], { x: 100.5, y: 50, z: 11.7 }, { x: 100.5, y: 50, z: 40 });
  assert.deepEqual(up.position, { x: 100.5, y: 50, z: 13 });
  assert.deepEqual(up.normal, { x: 0, y: 0, z: -1 });
  assert.equal(up.material, 2);
  const down = rayMeshes([placed], { x: 100, y: 50, z: 11 }, { x: 100, y: 50, z: 0 });
  assert.equal(down.position.z, 10);
  assert.equal(down.normal.z, 1);
  // Outside the room's turned square (its corner reaches 2.83 m along x): nothing.
  assert.equal(rayMeshes([placed], { x: 103, y: 50, z: 11 }, { x: 103, y: 50, z: 0 }), null);
  // Twice its size: the ceiling at 6 m.
  const large = { ...placed, scale: 2 };
  assert.equal(
    rayMeshes([large], { x: 100, y: 50, z: 11 }, { x: 100, y: 50, z: 40 }).position.z,
    16,
  );
});

test("the nearer of the game's hit and the drawn one is taken", () => {
  const from = { x: 0, y: 0, z: 0 };
  const game = { position: { x: 0, y: 0, z: 5 } };
  const drawn = { position: { x: 0, y: 0, z: 3 } };
  assert.equal(nearer(from, game, drawn), drawn);
  assert.equal(nearer(from, null, drawn), drawn);
  assert.equal(nearer(from, game, null), game);
});
