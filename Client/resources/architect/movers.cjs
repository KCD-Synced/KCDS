'use strict';

// World Architect's doors and gates of your own: a prop of yours that opens and closes. Its closed
// pose is
// the prop's own place and turn; the open pose is turned about a hinge at one side of its box (or
// its middle), lifted and slid along its own width, as a door, a portcullis or a sliding gate.
//
// A mover is { hinge: 'left' | 'right' | 'middle', turn (degrees, + to the left), lift (metres
// up), slide (metres along its width), seconds (to open or close), closeAfter (seconds open
// before it closes by itself, 0: it stays open) }. The pose at a share t of the way (0 closed, 1
// open) is the hinged turn by turn * t with lift * t and slide * t.

const MOVER_DEFAULTS = Object.freeze({
  hinge: 'left',
  turn: 90,
  lift: 0,
  slide: 0,
  seconds: 1.5,
  closeAfter: 6,
});
const HINGES = Object.freeze(['left', 'right', 'middle']);
const LIMITS = Object.freeze({
  turn: [-180, 180],
  lift: [0, 10],
  slide: [-10, 10],
  seconds: [0.5, 10],
  closeAfter: [0, 600],
});
// A turn is walked in steps of at most this many degrees (a straight glide between its ends cuts
// across the arc a hinged door swings along).
const STEP_DEGREES = 30;
const MOST_STEPS = 6;

function clamp(value, [low, high]) {
  return Math.min(high, Math.max(low, value));
}

// A mover as kept in a project, every field known and in its range; null for none.
function checkedMover(given) {
  if (given === null || given === undefined || typeof given !== 'object') {
    return null;
  }
  const mover = { ...MOVER_DEFAULTS };
  if (HINGES.includes(given.hinge)) {
    mover.hinge = given.hinge;
  }
  for (const key of Object.keys(LIMITS)) {
    if (Number.isFinite(given[key])) {
      mover[key] = clamp(given[key], LIMITS[key]);
    }
  }
  return mover;
}

function turned(x, y, degrees) {
  const angle = (degrees * Math.PI) / 180;
  const cos = Math.cos(angle);
  const sin = Math.sin(angle);
  return { x: x * cos - y * sin, y: x * sin + y * cos };
}

// The pose of a moving thing at share t of the way open: { at, turn }. `at` and `turn` are its
// closed place and turn (degrees), `box` its box ({ min, max } in its own frame, already scaled)
// or null (the hinge is then at its place).
function moverPose(at, turn, box, mover, t) {
  let hingeX = 0;
  let hingeY = 0;
  if (box && mover.hinge !== 'middle') {
    hingeX = mover.hinge === 'left' ? box.min[0] : box.max[0];
    hingeY = (box.min[1] + box.max[1]) / 2;
  }
  const hinge = turned(hingeX, hingeY, turn);
  const pivot = { x: at.x + hinge.x, y: at.y + hinge.y };
  const angle = mover.turn * t;
  const swung = turned(at.x - pivot.x, at.y - pivot.y, angle);
  const width = turned(1, 0, turn);
  return {
    at: {
      x: pivot.x + swung.x + width.x * mover.slide * t,
      y: pivot.y + swung.y + width.y * mover.slide * t,
      z: at.z + mover.lift * t,
    },
    turn: turn + angle,
  };
}

// The poses from closed to open, without the closed one: one for a lift or a slide, more for a
// turn (STEP_DEGREES each, at most MOST_STEPS).
function moverPath(at, turn, box, mover) {
  const steps = Math.min(MOST_STEPS, Math.max(1, Math.ceil(Math.abs(mover.turn) / STEP_DEGREES)));
  const path = [];
  for (let step = 1; step <= steps; step += 1) {
    path.push(moverPose(at, turn, box, mover, step / steps));
  }
  return path;
}

// Where players use it: the foot of its box's middle at its closed pose, with the box's reach
// (the radius of its footprint) and height for World.addInteraction's target.
function moverUse(at, turn, box) {
  if (!box) {
    return { at: { ...at }, reach: 0.8, height: 2.2 };
  }
  const middle = turned((box.min[0] + box.max[0]) / 2, (box.min[1] + box.max[1]) / 2, turn);
  const reach = Math.hypot(box.max[0] - box.min[0], box.max[1] - box.min[1]) / 2;
  return {
    at: { x: at.x + middle.x, y: at.y + middle.y, z: at.z + box.min[2] },
    reach: clamp(reach, [0.3, 5]),
    height: clamp(box.max[2] - box.min[2], [0.5, 10]),
  };
}

module.exports = {
  MOVER_DEFAULTS,
  HINGES,
  LIMITS,
  checkedMover,
  moverPose,
  moverPath,
  moverUse,
};
