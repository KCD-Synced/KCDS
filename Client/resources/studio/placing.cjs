'use strict';

// Where things stand in the studio: where the prop is held (a hand, or free in
// the scene: studio/studio_hold.hpp) and where your mesh stands (studio/studio_mesh.hpp), and the
// gizmo's dragging: a move along a world axis, a turn about one, a size.
//
// Turns are degrees about the world's x, then y, then z (the turn z * y * x), as the game client
// reads them.

// Where the prop can be held: the character's hands, or free in the scene.
const HOLDS = Object.freeze([
  { joint: 'RightHand', label: 'Right hand' },
  { joint: 'LeftHand', label: 'Left hand' },
  { joint: '', label: 'Free in the scene' },
]);

// A free prop's first place: at the character's right side, at hip height.
const FREE_PLACE = Object.freeze([32.6, 31.4, 33.3]);
// The steps of the place and turn in Details, and the drags' speeds.
const PLACE_STEP = 0.05;
const TURN_STEP = 15;
const TURN_DEGREES_PER_PIXEL = 0.5;
// Dragging 200 pixels to the right doubles the size.
const SCALE_PIXELS_PER_DOUBLE = 200;
const SCALE_RANGE = Object.freeze([0.05, 20]);
// A place stays within this far of the stage's middle (metres).
const PLACE_RANGE = Object.freeze([0, 64]);

function round(value, digits = 1000) {
  return Math.round(value * digits) / digits;
}

function threeNumbers(given, fallback) {
  if (!Array.isArray(given) || given.length !== 3) {
    return [...fallback];
  }
  const numbers = given.map(Number);
  return numbers.every(Number.isFinite) ? numbers : [...fallback];
}

// A kept or given hold, else the right hand.
function holdOf(given) {
  const joint = HOLDS.some((one) => one.joint === given?.joint) ? given.joint : 'RightHand';
  return {
    joint,
    at: threeNumbers(given?.at, FREE_PLACE).map((part) => round(part)),
    turn: threeNumbers(given?.turn, [0, 0, 0]).map((part) => round(part, 10)),
  };
}

// A rope's end: free, or pinned to a world point; at first a little beyond the free
// place, so a pinned rope hangs between its two points.
const END_PLACE = Object.freeze([FREE_PLACE[0] + 0.8, FREE_PLACE[1], FREE_PLACE[2]]);

function endOf(given) {
  return {
    pinned: given?.pinned === true,
    at: threeNumbers(given?.at, END_PLACE).map((part) => round(part)),
  };
}

function holdLabel(hold) {
  return HOLDS.find((one) => one.joint === hold.joint)?.label ?? 'Right hand';
}

// The next hold of HOLDS (direction 1 or -1).
function nextHold(hold, direction) {
  const index = HOLDS.findIndex((one) => one.joint === hold.joint);
  const next = HOLDS[(index + direction + HOLDS.length) % HOLDS.length];
  return { ...hold, joint: next.joint };
}

// A joint's name as the game's skeletons have them ('' for none).
const JOINT_NAME = /^[A-Za-z0-9_]{0,64}$/;

// A kept or given place of your mesh, else `place`: at, turn, scale, and the joint of the
// character holding it ('' free in the scene; held, at and turn are its place and turn
// in the joint, at first the joint's own).
function meshPlaceOf(given, place) {
  const scale = Number(given?.scale);
  const joint = typeof given?.joint === 'string' && JOINT_NAME.test(given.joint) ? given.joint : '';
  return {
    at: threeNumbers(given?.at, joint ? [0, 0, 0] : place).map((part) => round(part)),
    turn: threeNumbers(given?.turn, [0, 0, 0]).map((part) => round(part, 10)),
    scale: Number.isFinite(scale)
      ? round(Math.min(SCALE_RANGE[1], Math.max(SCALE_RANGE[0], scale)), 100)
      : 1,
    joint,
  };
}

// ---- Turns ----

function quaternionOf(degrees) {
  const [hx, hy, hz] = degrees.map((part) => (part * Math.PI) / 360);
  const cx = Math.cos(hx);
  const sx = Math.sin(hx);
  const cy = Math.cos(hy);
  const sy = Math.sin(hy);
  const cz = Math.cos(hz);
  const sz = Math.sin(hz);
  return [
    sx * cy * cz - cx * sy * sz,
    cx * sy * cz + sx * cy * sz,
    cx * cy * sz - sx * sy * cz,
    cx * cy * cz + sx * sy * sz,
  ];
}

function degreesOf([x, y, z, w]) {
  const sinY = Math.min(1, Math.max(-1, 2 * (w * y - z * x)));
  return [
    (Math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)) * 180) / Math.PI,
    (Math.asin(sinY) * 180) / Math.PI,
    (Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)) * 180) / Math.PI,
  ];
}

function multiply([ax, ay, az, aw], [bx, by, bz, bw]) {
  return [
    aw * bx + ax * bw + ay * bz - az * by,
    aw * by - ax * bz + ay * bw + az * bx,
    aw * bz + ax * by - ay * bx + az * bw,
    aw * bw - ax * bx - ay * by - az * bz,
  ];
}

// `turn` (degrees) turned a further `angle` degrees about the world's axis (0 X, 1 Y, 2 Z).
function turnAbout(turn, axis, angle) {
  const half = (angle * Math.PI) / 360;
  const about = [0, 0, 0, Math.cos(half)];
  about[axis] = Math.sin(half);
  return degreesOf(multiply(about, quaternionOf(turn))).map((part) => round(part, 10));
}

// One step of the turn about an axis (Details), as a turn about the world's axis.
function stepTurn(turn, axis, direction) {
  return turnAbout(turn, axis, direction * TURN_STEP);
}

function stepPlace(at, axis, direction) {
  const next = [...at];
  next[axis] = round(
    Math.min(PLACE_RANGE[1], Math.max(PLACE_RANGE[0], at[axis] + direction * PLACE_STEP)),
  );
  return next;
}

// ---- Dragging ----

// How far along the line through `origin` along the world's `axis` the press's line passes
// nearest (from, direction: the press's line); null when they run side by side.
function alongAxis(origin, axis, from, direction) {
  const length = Math.hypot(...direction);
  if (!(length > 0)) {
    return null;
  }
  const d = direction.map((part) => part / length);
  const w = origin.map((part, index) => part - from[index]);
  const b = d[axis];
  const denominator = 1 - b * b;
  if (denominator < 1e-6) {
    return null;
  }
  const e = w[axis];
  const f = d[0] * w[0] + d[1] * w[1] + d[2] * w[2];
  return (b * f - e) / denominator;
}

// A drag of the gizmo: { tool, axis, at, turn, scale, startX, start (along the axis) }.
function startDrag(tool, axis, place, pointer, line) {
  const start = line ? alongAxis(place.at, axis, line.from, line.direction) : null;
  return {
    tool,
    axis,
    at: [...place.at],
    turn: [...place.turn],
    scale: place.scale ?? 1,
    startX: pointer.x,
    start,
  };
}

// The place the drag gives now (pointer: the pointer on the stage; line: the press's line
// through it, for a move).
function dragPlace(drag, pointer, line) {
  const place = { at: [...drag.at], turn: [...drag.turn], scale: drag.scale };
  if (drag.tool === 'rotate') {
    place.turn = turnAbout(
      drag.turn,
      drag.axis,
      Math.round((pointer.x - drag.startX) * TURN_DEGREES_PER_PIXEL),
    );
  } else if (drag.tool === 'scale') {
    const factor = 2 ** ((pointer.x - drag.startX) / SCALE_PIXELS_PER_DOUBLE);
    place.scale = round(
      Math.min(SCALE_RANGE[1], Math.max(SCALE_RANGE[0], drag.scale * factor)),
      100,
    );
  } else if (line && drag.start !== null) {
    const now = alongAxis(drag.at, drag.axis, line.from, line.direction);
    if (now !== null) {
      const moved = round(drag.at[drag.axis] + now - drag.start);
      place.at[drag.axis] = Math.min(PLACE_RANGE[1], Math.max(PLACE_RANGE[0], moved));
    }
  }
  return place;
}

module.exports = {
  HOLDS,
  FREE_PLACE,
  PLACE_STEP,
  TURN_STEP,
  holdOf,
  holdLabel,
  endOf,
  nextHold,
  meshPlaceOf,
  turnAbout,
  stepTurn,
  stepPlace,
  alongAxis,
  startDrag,
  dragPlace,
  quaternionOf,
  degreesOf,
};
