'use strict';

// World Architect's Collision layer: the author's own invisible solid shapes, as the
// approved design draws them. A wall keeps players off a cliff, a floor closes a gap, a ramp
// leads up to a door, a box blocks a way. Each is a box of the client Collision
// (Client/resources/collision.cjs). While the Collision tool is chosen they are drawn in orange
// with Gizmos, the chosen one brighter. The editor runs every change as an undo step
// (editor.cjs); this module keeps the shapes and builds them.

// The kinds of shape and the size each starts with (width x, depth y, height z), metres; a ramp
// leans up along its depth by `pitch` degrees.
//
// Zones are boxes of the same kind that block nothing: areas
// with labels, as a server's kcds.world.Area.create makes them (KCDS-DOCS/areas.md), drawn in
// blue. `zone` marks them; `labels` are the area's labels.
const KINDS = Object.freeze({
  wall: { label: 'Invisible wall', size: { x: 6, y: 0.3, z: 3 }, pitch: 0 },
  floor: { label: 'Floor', size: { x: 4, y: 4, z: 0.2 }, pitch: 0 },
  ramp: { label: 'Ramp', size: { x: 3, y: 5, z: 0.2 }, pitch: 20 },
  box: { label: 'Box', size: { x: 2, y: 2, z: 2 }, pitch: 0 },
  safe: {
    label: 'Safe zone',
    size: { x: 12, y: 12, z: 5 },
    pitch: 0,
    zone: true,
    labels: ['safe'],
  },
  spawn: {
    label: 'Spawn area',
    size: { x: 6, y: 6, z: 3 },
    pitch: 0,
    zone: true,
    labels: ['spawn'],
  },
  trigger: {
    label: 'Trigger area',
    size: { x: 4, y: 4, z: 3 },
    pitch: 0,
    zone: true,
    labels: ['trigger'],
  },
  private: {
    label: 'Private ground',
    size: { x: 14, y: 14, z: 6 },
    pitch: 0,
    zone: true,
    labels: ['private'],
  },
});
// The solid shapes (Collision) and the zones (Zones), each in its own content folder.
const KIND_KEYS = Object.freeze(Object.keys(KINDS).filter((key) => !KINDS[key].zone));
const ZONE_KEYS = Object.freeze(Object.keys(KINDS).filter((key) => KINDS[key].zone));
// A size step, and the sizes a shape may have, metres.
const SIZE_STEP = 0.5;
const MIN_SIZE = 0.1;
const MAX_SIZE = 60;
const THIN_STEP = 0.1;
// Gizmo colours (0xAARRGGBB): the design's orange for the author's own shapes.
const OUTLINE = 0xc0f0a43a;
const OUTLINE_CHOSEN = 0xfff6c46a;
const FILL = 0x30f0a43a;
// A zone's blue.
const ZONE_OUTLINE = 0xc05ea2f2;
const ZONE_OUTLINE_CHOSEN = 0xff8cc0ff;
const ZONE_FILL = 0x205ea2f2;
const GIZMO_PREFIX = 'architect-collision-';

// A zone's stand-in for a body: only the turn its drawing needs (no collision).
function zoneBox(turn) {
  const half = (turn * Math.PI) / 360;
  return { rotation: { x: 0, y: 0, z: Math.sin(half), w: Math.cos(half) }, set() {}, remove() {} };
}

function clampSize(value) {
  return Math.min(MAX_SIZE, Math.max(MIN_SIZE, Math.round(value * 100) / 100));
}

class CollisionLayer {
  // api: { Collision, Gizmos, Vector3 }.
  constructor(api) {
    this.api = api;
    this.shapes = [];
    this.kind = 'wall';
    this.selected = null;
    this.shown = false;
    this.serial = 0;
    // The editor's world view: { inView(point), clip(line) } (editor.cjs); null draws boxes whole.
    this.view = null;
    // What each drawing was last sent as, so a redraw sends only what changed.
    this.sent = new Map();
  }

  // A new shape of the chosen kind standing on the ground at `ground`, turned `turn` degrees.
  make(ground, turn) {
    const kind = KINDS[this.kind];
    this.serial += 1;
    return {
      serial: this.serial,
      kind: this.kind,
      ground: { ...ground },
      size: { ...kind.size },
      turn,
      pitch: kind.pitch,
      box: null,
    };
  }

  // Its middle: its bottom on the ground; a ramp's low edge on the ground.
  centerOf(shape) {
    const lean = (shape.pitch * Math.PI) / 180;
    const lift = (shape.size.y / 2) * Math.sin(lean) + (shape.size.z / 2) * Math.cos(lean);
    return { x: shape.ground.x, y: shape.ground.y, z: shape.ground.z + lift };
  }

  poseOf(shape) {
    const { Vector3 } = this.api;
    const center = this.centerOf(shape);
    return {
      center: new Vector3(center.x, center.y, center.z),
      size: new Vector3(shape.size.x, shape.size.y, shape.size.z),
      rotation: { x: shape.pitch, y: 0, z: shape.turn },
    };
  }

  // Builds it in the game; false when the game would not. A zone is no body: only its drawing.
  build(shape) {
    try {
      shape.box = KINDS[shape.kind].zone
        ? zoneBox(shape.turn)
        : this.api.Collision.addBox(this.poseOf(shape));
    } catch {
      shape.box = null;
    }
    if (!shape.box) {
      return false;
    }
    this.shapes.push(shape);
    this.selected = shape;
    this.draw();
    return true;
  }

  unbuild(shape) {
    shape.box?.remove();
    shape.box = null;
    this.shapes = this.shapes.filter((item) => item !== shape);
    if (this.selected === shape) {
      this.selected = null;
    }
    this.api.Gizmos.remove(GIZMO_PREFIX + shape.serial);
    this.sent.delete(GIZMO_PREFIX + shape.serial);
    this.draw();
  }

  // Sends its new size, turn or place to the game.
  update(shape) {
    if (KINDS[shape.kind].zone && shape.box) {
      shape.box = zoneBox(shape.turn);
    } else {
      shape.box?.set(this.poseOf(shape));
    }
    this.draw();
  }

  // A zone as a server's Area.create definition (KCDS-DOCS/areas.md): a box around its middle.
  areaOf(shape) {
    const center = this.centerOf(shape);
    const half = { x: shape.size.x / 2, y: shape.size.y / 2, z: shape.size.z / 2 };
    return {
      id: `architect-zone-${shape.serial}`,
      type: 'box',
      position: { x: center.x, y: center.y, z: center.z },
      rotation: { x: 0, y: 0, z: shape.turn },
      min: { x: -half.x, y: -half.y, z: -half.z },
      max: half,
      labels: [...KINDS[shape.kind].labels],
    };
  }

  // The shape whose footprint holds the point, the nearest first; null for none.
  at(point) {
    let best = null;
    let bestDistance = Infinity;
    for (const shape of this.shapes) {
      const reach = Math.max(shape.size.x, shape.size.y) / 2 + 0.5;
      const distance = Math.hypot(shape.ground.x - point.x, shape.ground.y - point.y);
      if (distance <= reach && distance < bestDistance) {
        best = shape;
        bestDistance = distance;
      }
    }
    return best;
  }

  // The size of `shape` with one side changed by `step` (in steps of that side).
  resized(shape, axis, direction) {
    const thin = shape.size[axis] <= 1;
    const step = (thin ? THIN_STEP : SIZE_STEP) * direction;
    return { ...shape.size, [axis]: clampSize(shape.size[axis] + step) };
  }

  show() {
    this.shown = true;
    this.draw();
  }

  // Takes its drawings away (only its own: the editor's selection marks stay).
  hide() {
    this.shown = false;
    for (const shape of this.shapes) {
      this.api.Gizmos.remove(GIZMO_PREFIX + shape.serial);
    }
    this.sent.clear();
  }

  // The eight corners of a shape's box in the world, and its twelve edges between them.
  cornersOf(shape) {
    const center = this.centerOf(shape);
    const q = shape.box.rotation;
    const turn = (v) => {
      // v turned by the quaternion q (q v q*).
      const ix = q.w * v.x + q.y * v.z - q.z * v.y;
      const iy = q.w * v.y + q.z * v.x - q.x * v.z;
      const iz = q.w * v.z + q.x * v.y - q.y * v.x;
      const iw = -q.x * v.x - q.y * v.y - q.z * v.z;
      return {
        x: ix * q.w + iw * -q.x + iy * -q.z - iz * -q.y,
        y: iy * q.w + iw * -q.y + iz * -q.x - ix * -q.z,
        z: iz * q.w + iw * -q.z + ix * -q.y - iy * -q.x,
      };
    };
    const corners = [];
    for (const sx of [-1, 1]) {
      for (const sy of [-1, 1]) {
        for (const sz of [-1, 1]) {
          const local = turn({
            x: (sx * shape.size.x) / 2,
            y: (sy * shape.size.y) / 2,
            z: (sz * shape.size.z) / 2,
          });
          corners.push({ x: center.x + local.x, y: center.y + local.y, z: center.z + local.z });
        }
      }
    }
    return corners;
  }

  draw() {
    if (!this.shown) {
      return;
    }
    for (const shape of this.shapes) {
      if (!shape.box) {
        continue;
      }
      const chosen = shape === this.selected;
      const zone = KINDS[shape.kind].zone === true;
      const outline = zone ? ZONE_OUTLINE : OUTLINE;
      const brighter = zone ? ZONE_OUTLINE_CHOSEN : OUTLINE_CHOSEN;
      const half = { x: shape.size.x / 2, y: shape.size.y / 2, z: shape.size.z / 2 };
      const turn = shape.box.rotation;
      const id = GIZMO_PREFIX + shape.serial;
      const color = chosen ? brighter : outline;
      const width = chosen ? 3 : 2;
      const corners = this.cornersOf(shape);
      // Whole in the world view: the box with its see-through fill. Reaching past it (the game
      // draws over the panels too): only its edges, cut at the view's edge.
      if (!this.view || corners.every((corner) => this.view.inView(corner))) {
        this.send(
          id,
          {
            type: 'box',
            position: this.centerOf(shape),
            rotation: { x: turn.x, y: turn.y, z: turn.z, w: turn.w },
            min: { x: -half.x, y: -half.y, z: -half.z },
            max: half,
          },
          { color, fillColor: zone ? ZONE_FILL : FILL, width },
        );
        continue;
      }
      const points = [];
      const indices = [];
      for (let a = 0; a < 8; a += 1) {
        for (let b = a + 1; b < 8; b += 1) {
          // An edge joins corners that differ in one axis (one bit of their index).
          const differ = a ^ b;
          if (differ !== 1 && differ !== 2 && differ !== 4) {
            continue;
          }
          for (const part of this.view.clip({ from: corners[a], to: corners[b] })) {
            const first = points.length;
            points.push(part.from, part.to, part.to);
            indices.push(first, first + 1, first + 2);
          }
        }
      }
      if (points.length === 0) {
        this.api.Gizmos.remove(id);
        this.sent.delete(id);
        continue;
      }
      this.send(id, { type: 'mesh', points, indices, wireframe: true }, { color, width });
    }
  }

  // A drawing to the game, unless it was sent just so already.
  send(id, primitive, style) {
    const signature = JSON.stringify([primitive, style], (key, value) =>
      typeof value === 'number' ? Math.round(value * 1000) / 1000 : value,
    );
    if (this.sent.get(id) === signature) {
      return;
    }
    this.sent.set(id, signature);
    this.api.Gizmos.set(id, primitive, style);
  }

  describe(shape) {
    const { x, y, z } = shape.size;
    return [
      `${KINDS[shape.kind].label} · yours`,
      `Width ${x.toFixed(1)}  Depth ${y.toFixed(1)}  Height ${z.toFixed(1)} m`,
      `Turn ${shape.turn}°${shape.pitch ? `, leaning ${shape.pitch}°` : ''}`,
      KINDS[shape.kind].zone
        ? `An area: ${KINDS[shape.kind].labels.join(', ')}`
        : 'Blocks players and horses',
    ].join('\n');
  }
}

module.exports = { CollisionLayer, KINDS, KIND_KEYS, ZONE_KEYS };
