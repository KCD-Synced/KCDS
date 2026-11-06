'use strict';

// A whip's lash as frames (Lash to keys): the studio records its lash as physics
// moves it along an animation (studio/studio_rope.hpp, with the handle's frame), and each key of
// the animation keeps the lash's shape at its time: LASH_POINTS points along it, a share of its
// length apart, from the handle's tip in the handle's own frame ([along, right, up] metres).
// Played on players, the lash takes exactly those shapes, blended key to key, with no physics,
// the same on every client (kcd2s/integration/pose_animation.hpp, a key's "~" lash).

const LASH_POINTS = 12;

function dot(a, b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function cross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

function unit(v) {
  const length = Math.hypot(v[0], v[1], v[2]);
  return length > 1e-6 ? v.map((part) => part / length) : null;
}

// The handle's frame from its long side and up side: along, right (along × up) and up made
// square to along; null when they lie along each other.
function frameOf(handle) {
  const along = unit(handle.slice(0, 3));
  if (!along) {
    return null;
  }
  const rawUp = handle.slice(3, 6);
  const up = unit(rawUp.map((part, axis) => part - along[axis] * dot(along, rawUp)));
  return up ? { along, right: cross(along, up), up } : null;
}

// The lash's shape: LASH_POINTS points along its line (world points, its start the handle's
// tip), evenly by length, in the handle's frame, to the millimetre; null when it cannot be.
function lashShapeOf(points, handle) {
  const frame = frameOf(handle);
  const count = Math.floor(points.length / 3);
  if (!frame || count < 2) {
    return null;
  }
  const at = (index) => points.slice(index * 3, index * 3 + 3);
  const lengths = [0];
  for (let index = 1; index < count; index += 1) {
    const a = at(index - 1);
    const b = at(index);
    lengths.push(lengths[index - 1] + Math.hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]));
  }
  const total = lengths[count - 1];
  if (!(total > 0)) {
    return null;
  }
  const start = at(0);
  const shape = [];
  for (let point = 1; point <= LASH_POINTS; point += 1) {
    const wanted = (total * point) / LASH_POINTS;
    let piece = 1;
    while (piece < count - 1 && lengths[piece] < wanted) {
      piece += 1;
    }
    const a = at(piece - 1);
    const b = at(piece);
    const span = lengths[piece] - lengths[piece - 1];
    const share = span > 0 ? (wanted - lengths[piece - 1]) / span : 0;
    const world = a.map((part, axis) => part + (b[axis] - part) * share);
    const way = world.map((part, axis) => part - start[axis]);
    shape.push(
      [dot(way, frame.along), dot(way, frame.right), dot(way, frame.up)].map(
        (part) => Math.round(part * 1000) / 1000 || 0,
      ),
    );
  }
  return shape;
}

// The lash's shape for each of `times` (seconds) from a recording ({ rate, frames, handles })
// that started at the animation's start: a shape, or null for a time it has none at.
function lashKeys(recording, times) {
  const { rate, frames, handles } = recording;
  if (!Array.isArray(handles) || handles.length !== frames.length || !(rate > 0)) {
    return times.map(() => null);
  }
  return times.map((time) => {
    const index = Math.min(frames.length - 1, Math.max(0, Math.round(time * rate)));
    return lashShapeOf(frames[index], handles[index]);
  });
}

module.exports = { LASH_POINTS, frameOf, lashShapeOf, lashKeys };
