'use strict';

// World Architect's copy of several things at once (a whole castle): the pieces of a group, as
// they stand around the group's centre, and where each goes when the group is put down at another
// place and turned. Each piece is placed as your own copy (a prop of its mesh, or a plant), so the
// level's originals stay where they are.

// A piece: { source: 'prop' | 'vegetation', name, dx, dy, dz, turn, tilt, scale }, its offset from
// the group's centre on the ground (the lowest foot) and its own turn in degrees.
function piecesOf(things, describe) {
  const described = things.map(describe).filter(Boolean);
  if (described.length === 0) {
    return [];
  }
  const centre = {
    x: described.reduce((sum, piece) => sum + piece.at.x, 0) / described.length,
    y: described.reduce((sum, piece) => sum + piece.at.y, 0) / described.length,
    z: Math.min(...described.map((piece) => piece.at.z)),
  };
  return described.map((piece) => ({
    source: piece.source,
    name: piece.name,
    dx: piece.at.x - centre.x,
    dy: piece.at.y - centre.y,
    dz: piece.at.z - centre.z,
    turn: piece.turn ?? 0,
    tilt: piece.tilt ?? null,
    scale: piece.scale ?? 1,
  }));
}

// The quaternion of a turn by `degrees` about the up axis, before `tilt` (x, y, z, w).
function turnedTilt(tilt, degrees) {
  if (!tilt) {
    return null;
  }
  const half = (degrees * Math.PI) / 360;
  const [s, c] = [Math.sin(half), Math.cos(half)];
  return {
    x: c * tilt.x - s * tilt.y,
    y: c * tilt.y + s * tilt.x,
    z: c * tilt.z + s * tilt.w,
    w: c * tilt.w - s * tilt.z,
  };
}

// Where each piece goes with the group's centre at `at`, turned by `turn` degrees about it:
// [{ piece, at, turn, tilt }].
function placementsOf(pieces, at, turn = 0) {
  const radians = (turn * Math.PI) / 180;
  const [s, c] = [Math.sin(radians), Math.cos(radians)];
  return pieces.map((piece) => ({
    piece,
    at: {
      x: at.x + piece.dx * c - piece.dy * s,
      y: at.y + piece.dx * s + piece.dy * c,
      z: at.z + piece.dz,
    },
    turn: (((piece.turn + turn) % 360) + 360) % 360,
    tilt: turnedTilt(piece.tilt, turn),
  }));
}

module.exports = { piecesOf, placementsOf, turnedTilt };
