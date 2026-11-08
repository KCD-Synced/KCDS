'use strict';

// The client Terrain (Client/resources/terrain.cjs, World Architect terraforming): the ground
// shaped in this game only, every edit undoable.
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  ResourceTerrain,
  REACH_MAX,
  GROUND_MATERIALS,
} = require('../../../../Client/resources/terrain.cjs');

function setup() {
  const sent = [];
  const terrain = new ResourceTerrain({ send: (message) => sent.push(message) });
  return {
    terrain,
    sent,
    Terrain: terrain.facade('editor'),
    Other: terrain.facade('other'),
  };
}

test('flatten and raise send edits; undo reverts once', () => {
  const { sent, Terrain } = setup();
  const pad = Terrain.flatten({ x: 100, y: 200, z: 5 }, 6, 12.5);
  assert.deepEqual(sent[0], {
    op: 'localTerrain',
    kind: 'edit',
    id: 1,
    shape: 'flatten',
    x: 100,
    y: 200,
    radius: 6,
    falloff: 6,
    value: 12.5,
  });
  const hill = Terrain.raise({ x: 110, y: 200 }, 3, -2, 1);
  assert.equal(sent.at(-1).shape, 'raise');
  assert.equal(sent.at(-1).value, -2);
  assert.equal(sent.at(-1).falloff, 1);
  assert.deepEqual(Terrain.edits(), [pad, hill]);

  assert.equal(pad.undo(), true);
  assert.equal(pad.undo(), false);
  assert.equal(pad.valid, false);
  assert.deepEqual(sent.at(-1), { op: 'localTerrain', kind: 'revert', id: 1 });
  assert.deepEqual(Terrain.edits(), [hill]);
});

test('bad places, sizes and heights throw', () => {
  const { Terrain } = setup();
  assert.throws(() => Terrain.flatten(null, 5, 1), TypeError);
  assert.throws(() => Terrain.flatten({ x: 1e9, y: 0 }, 5, 1), RangeError);
  assert.throws(() => Terrain.flatten({ x: 0, y: 0 }, -1, 1), TypeError);
  assert.throws(() => Terrain.flatten({ x: 0, y: 0 }, 5, 1, -1), TypeError);
  assert.throws(() => Terrain.flatten({ x: 0, y: 0 }, REACH_MAX, 1, 1), RangeError);
  assert.throws(() => Terrain.raise({ x: 0, y: 0 }, 5, Number.NaN), TypeError);
});

test('a stopped resource has its edits undone, newest first; others keep theirs', () => {
  const { terrain, sent, Terrain, Other } = setup();
  Terrain.flatten({ x: 0, y: 0 }, 2, 1);
  Terrain.raise({ x: 0, y: 0 }, 2, 1);
  const kept = Other.raise({ x: 50, y: 0 }, 2, 1);
  terrain.removeOwner('editor');
  assert.deepEqual(
    sent.slice(-2).map((message) => [message.kind, message.id]),
    [
      ['revert', 2],
      ['revert', 1],
    ],
  );
  assert.equal(kept.valid, true);
  assert.deepEqual(Terrain.edits(), []);
});

// The ground's look (World Architect texturing): a material by its name or the level's surface
// type index, blended at the edge like the other edits.
test('paint sends the material as the surface type index; bad materials throw', () => {
  const { sent, Terrain } = setup();
  const road = Terrain.paint({ x: 10, y: 20 }, 2, 'cobblestones', 2);
  assert.deepEqual(sent[0], {
    op: 'localTerrain',
    kind: 'edit',
    id: 1,
    shape: 'paint',
    x: 10,
    y: 20,
    radius: 2,
    falloff: 2,
    value: GROUND_MATERIALS.cobblestones,
    // A game level finds its own surface type by these names (its numbers are its own).
    material: 'cobblestones|cobble_stones_a|cobble',
  });
  assert.equal(GROUND_MATERIALS.cobblestones, 37);
  Terrain.paint({ x: 10, y: 20 }, 3, 21);
  assert.equal(sent.at(-1).value, 21);
  assert.equal(sent.at(-1).material, undefined);
  assert.equal(sent.at(-1).falloff, 3);
  assert.equal(Terrain.materials().grass, 18);
  assert.throws(() => Terrain.paint({ x: 0, y: 0 }, 2, 'lava'), RangeError);
  assert.throws(() => Terrain.paint({ x: 0, y: 0 }, 2, 127), TypeError);
  assert.throws(() => Terrain.paint({ x: 0, y: 0 }, 2, 1.5), TypeError);
  assert.equal(road.undo(), true);
  assert.deepEqual(sent.at(-1), { op: 'localTerrain', kind: 'revert', id: 1 });
});

test("hideGrass takes the level's grass away in a circle; undo and the resource's stop bring it back", () => {
  const { terrain, sent, Terrain } = setup();
  const yard = Terrain.hideGrass({ x: 2675, y: 1562, z: 40 }, 12);
  assert.equal(yard.shape, 'grass');
  // Its id is apart from the editor's own grass ids, which count from 1.
  assert.deepEqual(sent.at(-1), {
    op: 'architectGrass',
    kind: 'hide',
    id: 0x40000000 + yard.id,
    x: 2675,
    y: 1562,
    radius: 12,
  });
  assert.deepEqual(Terrain.edits(), [yard]);
  assert.equal(yard.undo(), true);
  assert.deepEqual(sent.at(-1), { op: 'architectGrass', kind: 'show', id: 0x40000000 + yard.id });
  assert.equal(yard.undo(), false);
  const floor = Terrain.hideGrass({ x: 10, y: 10 }, 4);
  terrain.removeOwner('editor');
  assert.deepEqual(sent.at(-1), { op: 'architectGrass', kind: 'show', id: 0x40000000 + floor.id });
  assert.throws(() => Terrain.hideGrass({ x: 0, y: 0 }, 0), RangeError);
  assert.throws(() => Terrain.hideGrass({ x: 0, y: 0 }, REACH_MAX + 1), RangeError);
  assert.throws(() => Terrain.hideGrass({ y: 0 }, 3), TypeError);
});

test("blockNavmesh closes the game's navmesh in a box until undone or the resource stops", () => {
  const { terrain, sent, Terrain } = setup();
  const house = Terrain.blockNavmesh({ x: 100, y: 200, z: 30 }, { x: 108, y: 206, z: 40 });
  assert.equal(house.shape, 'navmesh');
  assert.deepEqual(sent.at(-1), {
    op: 'localNavmesh',
    kind: 'block',
    id: house.id,
    min: [100, 200, 30],
    max: [108, 206, 40],
  });
  assert.equal(house.undo(), true);
  assert.deepEqual(sent.at(-1), { op: 'localNavmesh', kind: 'unblock', id: house.id });
  // Without heights it reaches from far under to far over the ground.
  const yard = Terrain.blockNavmesh({ x: 0, y: 0 }, { x: 10, y: 10 });
  assert.deepEqual(sent.at(-1).min, [0, 0, -10000]);
  terrain.removeOwner('editor');
  assert.deepEqual(sent.at(-1), { op: 'localNavmesh', kind: 'unblock', id: yard.id });
  assert.throws(() => Terrain.blockNavmesh({ x: 0, y: 0 }, { x: 500, y: 0 }), RangeError);
  assert.throws(() => Terrain.blockNavmesh({ y: 0 }, { x: 1, y: 1 }), TypeError);
});
