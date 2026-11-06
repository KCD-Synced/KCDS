'use strict';

// World Architect's marks in the world: the selection's box and its move gizmo (the
// X, Y and Z arrows at its foot, red, green and blue as the design's axis colours), and a thinner
// box around what the cursor is over, the target a press would select.
//
// The marks are lines in the world. The editor puts them on the stage each poll (NoClip.toScreen)
// and the shell draws them in the layer above the world view and below the panels, cut to the
// view, so no mark lies over a panel (the game's gizmos are drawn over everything, live).

const AXES = Object.freeze([
  { axis: 'x', color: 0xf06b5f, to: [1, 0, 0] },
  { axis: 'y', color: 0x6cc46a, to: [0, 1, 0] },
  { axis: 'z', color: 0x5ea2f2, to: [0, 0, 1] },
]);
const SELECTION_COLOR = 0xf0a93b;
// The Rotate tool's ring around the selection's foot, and how many strokes draw it.
const RING_COLOR = 0xf6c46a;
const RING_STEPS = 48;
const TARGET_COLOR = 0xf6c46a;
// The arrows' length: a share of the box, at least this many metres; their heads' size.
const AXIS_LENGTH = 2.5;
const HEAD = 0.5;

// The point of a ring at angle t (radians).
function ringPoint(ring, t) {
  const { center, radius, u, v } = ring;
  const c = Math.cos(t) * radius;
  const s = Math.sin(t) * radius;
  return {
    x: center.x + u.x * c + v.x * s,
    y: center.y + u.y * c + v.y * s,
    z: center.z + u.z * c + v.z * s,
  };
}

// A turn about the up axis in degrees, applied to (x, y).
function turned(degrees, x, y) {
  const turn = (degrees * Math.PI) / 180;
  return [x * Math.cos(turn) - y * Math.sin(turn), x * Math.sin(turn) + y * Math.cos(turn)];
}

// A box's twelve edges as world lines.
function boxLines(box, color, width) {
  const corners = [];
  for (const bx of [box.min[0], box.max[0]]) {
    for (const by of [box.min[1], box.max[1]]) {
      for (const bz of [box.min[2], box.max[2]]) {
        const [dx, dy] = turned(box.turn ?? 0, bx, by);
        corners.push({ x: box.at.x + dx, y: box.at.y + dy, z: box.at.z + bz });
      }
    }
  }
  const lines = [];
  for (let index = 0; index < 8; index += 1) {
    for (const bit of [1, 2, 4]) {
      if ((index & bit) === 0) {
        lines.push({ from: corners[index], to: corners[index | bit], color, width });
      }
    }
  }
  return lines;
}

class Marks {
  constructor() {
    this.shown = { selection: null, target: null };
    // The move arrows now: { axis, from, direction, length } each (for dragging them).
    this.arrows = [];
    // With ringMode (the Rotate tool) rings instead of the arrows: { axis, center, radius, u, v },
    // the circle center + (u cos t + v sin t) radius, t counterclockwise about the ring's axis.
    // The Z ring (turn) always; the X and Y rings (tilt about the thing's own axes) when the
    // selection's box says it tilts. `ring` is the Z ring.
    this.ringMode = false;
    this.ring = null;
    this.rings = [];
  }

  // A thing's box: { at, turn, min, max } (min and max around its foot, turned with it), or null.
  static boxOf(thing, bounds) {
    if (!thing || !bounds) {
      return null;
    }
    return { at: thing.at, turn: thing.turn ?? 0, min: bounds.min, max: bounds.max };
  }

  // The selection's box and move arrows; null takes them away.
  select(box) {
    this.shown.selection = box;
    this.arrows = [];
    this.ring = null;
    this.rings = [];
    if (!box) {
      return;
    }
    const size = Math.max(box.max[0] - box.min[0], box.max[1] - box.min[1]);
    const length = Math.max(AXIS_LENGTH, size * 0.6);
    const foot = { x: box.at.x, y: box.at.y, z: box.at.z + 0.05 };
    if (this.ringMode) {
      this.ring = {
        axis: 'z',
        center: foot,
        radius: length,
        u: { x: 1, y: 0, z: 0 },
        v: { x: 0, y: 1, z: 0 },
      };
      this.rings.push(this.ring);
      if (box.tilts) {
        const height = box.max[2] - box.min[2];
        const middle = { x: box.at.x, y: box.at.y, z: box.at.z + (box.max[2] + box.min[2]) / 2 };
        const radius = Math.max(length * 0.8, height * 0.6);
        const [ax, ay] = turned(box.turn ?? 0, 1, 0);
        const [bx, by] = turned(box.turn ?? 0, 0, 1);
        const up = { x: 0, y: 0, z: 1 };
        // About its own X: from its Y up to Z; about its own Y: from Z over to its X.
        this.rings.push({ axis: 'x', center: middle, radius, u: { x: bx, y: by, z: 0 }, v: up });
        this.rings.push({ axis: 'y', center: middle, radius, u: up, v: { x: ax, y: ay, z: 0 } });
      }
      return;
    }
    for (const { axis, to } of AXES) {
      // The X and Y arrows turn with the thing.
      const [dx, dy] = turned(box.turn ?? 0, to[0], to[1]);
      this.arrows.push({ axis, from: { ...foot }, direction: { x: dx, y: dy, z: to[2] }, length });
    }
  }

  // The box around what the cursor is over; null takes it away.
  target(box) {
    this.shown.target = box;
  }

  clear() {
    this.select(null);
    this.target(null);
  }

  // Every mark as world lines { from, to, color, width }.
  lines() {
    const lines = [];
    if (this.shown.target) {
      lines.push(...boxLines(this.shown.target, TARGET_COLOR, 1.5));
    }
    if (this.shown.selection) {
      lines.push(...boxLines(this.shown.selection, SELECTION_COLOR, 2));
    }
    for (const ring of this.rings) {
      // Each ring in its axis's colour (the Z ring, the turn, in the ring colour).
      const color =
        ring.axis === 'z' ? RING_COLOR : AXES.find((entry) => entry.axis === ring.axis).color;
      for (let step = 0; step < RING_STEPS; step += 1) {
        const a = (step / RING_STEPS) * Math.PI * 2;
        const b = ((step + 1) / RING_STEPS) * Math.PI * 2;
        lines.push({ from: ringPoint(ring, a), to: ringPoint(ring, b), color, width: 3 });
      }
    }
    if (this.ring) {
      // A short spoke at the thing's own front so its turn shows.
      const { center, radius } = this.ring;
      const point = (angle, reach) => ({
        x: center.x + Math.cos(angle) * reach,
        y: center.y + Math.sin(angle) * reach,
        z: center.z,
      });
      const front = (((this.shown.selection?.turn ?? 0) + 90) * Math.PI) / 180;
      lines.push({
        from: point(front, radius * 0.75),
        to: point(front, radius * 1.15),
        color: RING_COLOR,
        width: 3,
      });
    }
    for (const arrow of this.arrows) {
      const color = AXES.find((entry) => entry.axis === arrow.axis).color;
      const { from, direction: d, length } = arrow;
      const tip = { x: from.x + d.x * length, y: from.y + d.y * length, z: from.z + d.z * length };
      lines.push({ from, to: tip, color, width: 3 });
      // The head: two short strokes back from the tip, across the arrow.
      const across = d.z === 0 ? { x: -d.y, y: d.x, z: 0 } : { x: 1, y: 0, z: 0 };
      for (const side of [1, -1]) {
        const back = {
          x: tip.x - d.x * HEAD + across.x * HEAD * 0.5 * side,
          y: tip.y - d.y * HEAD + across.y * HEAD * 0.5 * side,
          z: tip.z - d.z * HEAD + across.z * HEAD * 0.5 * side,
        };
        lines.push({ from: tip, to: back, color, width: 3 });
      }
    }
    return lines;
  }
}

module.exports = { Marks, ringPoint };
