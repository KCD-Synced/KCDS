'use strict';

// World Architect's copy of several things (Client/resources/architect/group_copy.cjs).

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  piecesOf,
  placementsOf,
  turnedTilt,
} = require('../../../../Client/resources/architect/group_copy.cjs');

test('pieces keep their places around the group centre; what cannot be copied is left out', () => {
  const things = [
    { at: { x: 0, y: 0, z: 2 }, turn: 0 },
    { at: { x: 10, y: 0, z: 5 }, turn: 90 },
    { skip: true },
  ];
  const pieces = piecesOf(things, (thing) =>
    thing.skip ? null : { source: 'prop', name: 'm.cgf', at: thing.at, turn: thing.turn },
  );
  assert.deepEqual(
    pieces.map((piece) => [piece.dx, piece.dy, piece.dz, piece.turn]),
    [
      [-5, 0, 0, 0],
      [5, 0, 3, 90],
    ],
  );
  assert.deepEqual(
    piecesOf([], () => null),
    [],
  );
});

test('placements turn about the centre, and each piece turns with them', () => {
  const pieces = [
    { source: 'prop', name: 'm.cgf', dx: 5, dy: 0, dz: 1, turn: 300, tilt: null, scale: 1 },
  ];
  const [placed] = placementsOf(pieces, { x: 100, y: 200, z: 10 }, 90);
  assert.ok(Math.abs(placed.at.x - 100) < 1e-9);
  assert.ok(Math.abs(placed.at.y - 205) < 1e-9);
  assert.equal(placed.at.z, 11);
  assert.equal(placed.turn, 30);
  // A tilted piece's rotation gets the same quarter turn about the up axis.
  const tilt = turnedTilt({ x: 0, y: 0, z: 0, w: 1 }, 90);
  assert.ok(Math.abs(tilt.z - Math.SQRT1_2) < 1e-9 && Math.abs(tilt.w - Math.SQRT1_2) < 1e-9);
});
