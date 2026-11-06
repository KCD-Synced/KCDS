'use strict';

// The client Vegetation (Client/resources/vegetation.cjs, World Architect): plants of the level's
// kinds in this game only, through the sync channel.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceVegetation } = require('../../../../Client/resources/vegetation.cjs');

const KINDS = [
  'objects/natural/trees/oak/oak_a.cgf',
  '',
  'Objects/Natural/Bushes/hazel/hazel_b.cgf',
];

const stem = (path) => path.slice(path.lastIndexOf('/') + 1).replace(/\.cgf$/, '');

function kindNamed(name) {
  const wanted = name.toLowerCase();
  const paths = KINDS.map((path) => path.toLowerCase());
  const exact = paths.indexOf(wanted);
  if (exact >= 0) {
    return exact;
  }
  return paths.findIndex((path) => path !== '' && stem(path) === stem(wanted));
}

function setup({ inWorld = true } = {}) {
  const asked = [];
  let next = 7;
  const sync = {
    call(request) {
      asked.push(request);
      if (!inWorld) {
        return null;
      }
      if (request.op === 'localVegetationKinds') {
        return { kinds: KINDS };
      }
      if (request.op === 'localVegetationPlant') {
        // As the game: a mesh by its path or file name, else a kind by its index.
        const index = request.mesh === undefined ? request.kind : kindNamed(request.mesh);
        if (!(index >= 0) || KINDS[index] === '') {
          return null;
        }
        next += 1;
        return { id: next, kind: index, mesh: KINDS[index].toLowerCase() };
      }
      if (request.op === 'localVegetationRemove') {
        return true;
      }
      return null;
    },
  };
  const vegetation = new ResourceVegetation({ sync });
  return {
    vegetation,
    asked,
    Vegetation: vegetation.facade('editor'),
    Other: vegetation.facade('other'),
  };
}

test('kinds lists the level vegetation; plant takes an index, a path or a file name', () => {
  const { asked, Vegetation } = setup();
  assert.deepEqual(Vegetation.kinds(), [
    'objects/natural/trees/oak/oak_a.cgf',
    '',
    'objects/natural/bushes/hazel/hazel_b.cgf',
  ]);
  const oak = Vegetation.plant(0, { x: 1, y: 2, z: 3 }, { scale: 1.5, turn: 90 });
  assert.equal(oak.id, 8);
  assert.equal(oak.kind, 'objects/natural/trees/oak/oak_a.cgf');
  assert.deepEqual(asked.at(-1), {
    op: 'localVegetationPlant',
    kind: 0,
    x: 1,
    y: 2,
    z: 3,
    scale: 1.5,
    turn: 90,
  });
  // A kind by name is one question: the game finds the mesh (no kinds list first).
  const before = asked.length;
  const hazel = Vegetation.plant('hazel_b', { x: 0, y: 0, z: 0 });
  assert.equal(hazel.kind, 'objects/natural/bushes/hazel/hazel_b.cgf');
  assert.equal(asked.length, before + 1);
  assert.equal(asked.at(-1).mesh, 'hazel_b');
  assert.equal(
    Vegetation.plant('Objects\\Natural\\Trees\\oak\\oak_a.cgf', { x: 0, y: 0, z: 0 }).kind,
    oak.kind,
  );
  // A kind the level does not have, or an empty one, plants nothing.
  assert.equal(Vegetation.plant('birch_z', { x: 0, y: 0, z: 0 }), null);
  assert.equal(Vegetation.plant(1, { x: 0, y: 0, z: 0 }), null);
  assert.equal(Vegetation.plant(9, { x: 0, y: 0, z: 0 }), null);
  assert.deepEqual(Vegetation.planted().length, 3);
});

test('remove takes a plant away once; bad positions and options throw', () => {
  const { asked, Vegetation } = setup();
  const oak = Vegetation.plant(0, { x: 1, y: 2, z: 3 });
  assert.equal(oak.remove(), true);
  assert.equal(oak.remove(), false);
  assert.equal(oak.valid, false);
  assert.deepEqual(asked.at(-1), { op: 'localVegetationRemove', id: oak.id });
  assert.throws(() => Vegetation.plant(0, null), TypeError);
  assert.throws(() => Vegetation.plant(0, { x: 1e9, y: 0, z: 0 }), RangeError);
  assert.throws(() => Vegetation.plant(0, { x: 0, y: 0, z: 0 }, { scale: 9 }), RangeError);
  assert.throws(() => Vegetation.plant(0, { x: 0, y: 0, z: 0 }, { turn: 'north' }), TypeError);
  assert.throws(() => Vegetation.plant({}, { x: 0, y: 0, z: 0 }), TypeError);
});

test('outside a world nothing is planted; a stopped resource has its plants taken away', () => {
  const outside = setup({ inWorld: false });
  assert.deepEqual(outside.Vegetation.kinds(), []);
  assert.equal(outside.Vegetation.plant(0, { x: 0, y: 0, z: 0 }), null);

  const { vegetation, asked, Vegetation, Other } = setup();
  const mine = Vegetation.plant(0, { x: 0, y: 0, z: 0 });
  const theirs = Other.plant(2, { x: 5, y: 0, z: 0 });
  vegetation.removeOwner('editor');
  assert.equal(mine.valid, false);
  assert.equal(theirs.valid, true);
  assert.deepEqual(asked.at(-1), { op: 'localVegetationRemove', id: mine.id });
});
