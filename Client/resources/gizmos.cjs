'use strict';

// The client-only Gizmos global: debug drawings a resource puts in this player's own
// world, with no replication. A drawing is a named primitive the resource owns; it is built here
// into points, lines (index pairs) and triangles (index triples) and goes down as
//
//   { op: 'gizmoSet', owner, id, points, lines, triangles, lineColors, thinLines, color, fill,
//     width, depthTest, durationMs }
//   { op: 'gizmoTransform', owner, id, position, rotation }
//   { op: 'gizmoRemove', owner, id }
//   { op: 'gizmoRemoveOwner', owner }
//
// `points` are x, y, z in a row; `lineColors` (axes only) gives each line its own colour and
// `thinLines` the lines drawn at 60 % width (a polygon area's vertical edges). Colours are
// unsigned 0xAARRGGBB, `fill` 0 for none. The game client (Client/runtime/gizmos/gizmos.cpp) draws
// every point p at position + rotation * p each frame with the renderer's aux geometry. A stopped
// resource, a disconnect and another level clear the drawings.

const MAX_COORDINATE = 16384;
const MAX_ID_BYTES = 128;
const MAX_IDS = 128;
const MAX_UNITS = 16384;
const MAX_DURATION_MS = 3600000;
const DEFAULT_COLOR = 0xff6ee678;
const DEFAULT_WIDTH = 1.5;
const MIN_WIDTH = 0.5;
const MAX_WIDTH = 8;
const DEFAULT_SEGMENTS = 24;
const MIN_SEGMENTS = 8;
const MAX_SEGMENTS = 64;
const MIN_RADIUS = 0.001;
const SPHERE_RINGS = 12;
const ARROW_SEGMENTS = 12;
const AXIS_COLORS = [0xffff0000, 0xff00ff00, 0xff0000ff];
const UNFILLED = new Set(['line', 'polyline', 'grid', 'axes', 'shape']);
const IDENTITY = Object.freeze({ x: 0, y: 0, z: 0, w: 1 });

const encoder = new TextEncoder();

function finite(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function fail(message) {
  throw new TypeError(`Gizmos: ${message}`);
}

function idOf(id) {
  if (typeof id !== 'string') {
    fail('an ID is a string');
  }
  const bytes = encoder.encode(id).length;
  if (bytes < 1 || bytes > MAX_ID_BYTES) {
    fail(`an ID is 1 to ${MAX_ID_BYTES} UTF-8 bytes`);
  }
  return id;
}

function pointOf(value, name) {
  const valid =
    value !== null &&
    typeof value === 'object' &&
    finite(value.x) &&
    finite(value.y) &&
    finite(value.z) &&
    Math.abs(value.x) <= MAX_COORDINATE &&
    Math.abs(value.y) <= MAX_COORDINATE &&
    Math.abs(value.z) <= MAX_COORDINATE;
  if (!valid) {
    fail(`${name} must be a point { x, y, z } within ${MAX_COORDINATE} metres`);
  }
  return [value.x, value.y, value.z];
}

function rotationOf(value, name) {
  if (value === undefined) {
    return [0, 0, 0, 1];
  }
  const valid =
    value !== null &&
    typeof value === 'object' &&
    finite(value.x) &&
    finite(value.y) &&
    finite(value.z) &&
    finite(value.w);
  const length = valid ? Math.hypot(value.x, value.y, value.z, value.w) : 0;
  if (!(length > 1e-6)) {
    fail(`${name} must be a quaternion { x, y, z, w }`);
  }
  return [value.x / length, value.y / length, value.z / length, value.w / length];
}

function numberIn(value, fallback, min, max, name, { integer = false, above = false } = {}) {
  const amount = value === undefined ? fallback : value;
  const inRange = finite(amount) && (above ? amount > min : amount >= min) && amount <= max;
  if (amount === undefined || !inRange || (integer && !Number.isInteger(amount))) {
    const kind = integer ? 'a whole number' : 'a number';
    fail(`${name} must be ${kind} ${above ? 'above' : 'from'} ${min} to ${max}`);
  }
  return amount;
}

function colorOf(value, fallback, name) {
  const color = value === undefined ? fallback : value;
  if (!Number.isInteger(color) || color < 0 || color > 0xffffffff) {
    fail(`${name} must be an unsigned 0xAARRGGBB colour`);
  }
  return color;
}

function rotate(q, p) {
  const [x, y, z, w] = q;
  const tx = 2 * (y * p[2] - z * p[1]);
  const ty = 2 * (z * p[0] - x * p[2]);
  const tz = 2 * (x * p[1] - y * p[0]);
  return [
    p[0] + w * tx + (y * tz - z * ty),
    p[1] + w * ty + (z * tx - x * tz),
    p[2] + w * tz + (x * ty - y * tx),
  ];
}

function add(a, b) {
  return [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
}

function sub(a, b) {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function scale(a, s) {
  return [a[0] * s, a[1] * s, a[2] * s];
}

function cross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

function length(a) {
  return Math.hypot(a[0], a[1], a[2]);
}

function unit(a) {
  return scale(a, 1 / length(a));
}

// Two unit vectors that span the plane square to the unit `axis`.
function planeOf(axis) {
  const helper = Math.abs(axis[2]) < 0.9 ? [0, 0, 1] : [1, 0, 0];
  const u = unit(cross(helper, axis));
  return [u, cross(axis, u)];
}

// The geometry a drawing is built into.
class Shape {
  constructor() {
    this.points = [];
    this.lines = [];
    this.triangles = [];
    this.lineColors = null;
    this.thinLines = [];
  }
  point(p) {
    this.points.push(p);
    return this.points.length - 1;
  }
  line(a, b) {
    this.lines.push(a, b);
    return this.lines.length / 2 - 1;
  }
  triangle(a, b, c) {
    this.triangles.push(a, b, c);
  }
  // A ring of `segments` points about `center` in the plane of `u` and `v`; their indices.
  ring(center, u, v, radius, segments, { outline = true } = {}) {
    const indices = [];
    for (let i = 0; i < segments; i += 1) {
      const angle = (2 * Math.PI * i) / segments;
      const offset = add(scale(u, Math.cos(angle) * radius), scale(v, Math.sin(angle) * radius));
      indices.push(this.point(add(center, offset)));
    }
    if (outline) {
      for (let i = 0; i < segments; i += 1) {
        this.line(indices[i], indices[(i + 1) % segments]);
      }
    }
    return indices;
  }
  // A flat disc's faces from its centre to a ring.
  fan(center, ring) {
    const middle = this.point(center);
    for (let i = 0; i < ring.length; i += 1) {
      this.triangle(middle, ring[i], ring[(i + 1) % ring.length]);
    }
  }
  // The faces between two rings of the same count.
  band(lower, upper) {
    for (let i = 0; i < lower.length; i += 1) {
      const next = (i + 1) % lower.length;
      this.triangle(lower[i], lower[next], upper[next]);
      this.triangle(lower[i], upper[next], upper[i]);
    }
  }
  units() {
    return this.lines.length / 2 + this.triangles.length;
  }
}

function boxShape(shape, center, rotation, min, max, filled) {
  const corners = [];
  for (let i = 0; i < 8; i += 1) {
    const local = [i & 1 ? max[0] : min[0], i & 2 ? max[1] : min[1], i & 4 ? max[2] : min[2]];
    corners.push(shape.point(add(center, rotate(rotation, local))));
  }
  // Each edge joins two corners that differ in one axis bit.
  for (let i = 0; i < 8; i += 1) {
    for (let bit = 1; bit < 8; bit <<= 1) {
      if ((i & bit) === 0) {
        shape.line(corners[i], corners[i | bit]);
      }
    }
  }
  if (filled) {
    const faces = [
      [0, 2, 3, 1],
      [4, 5, 7, 6],
      [0, 1, 5, 4],
      [2, 6, 7, 3],
      [0, 4, 6, 2],
      [1, 3, 7, 5],
    ];
    for (const [a, b, c, d] of faces) {
      shape.triangle(corners[a], corners[b], corners[c]);
      shape.triangle(corners[a], corners[c], corners[d]);
    }
  }
}

// A sphere's outline is three great circles; its faces a globe of rings.
function sphereShape(shape, center, rotation, radius, segments, filled) {
  const x = rotate(rotation, [1, 0, 0]);
  const y = rotate(rotation, [0, 1, 0]);
  const z = rotate(rotation, [0, 0, 1]);
  shape.ring(center, x, y, radius, segments);
  shape.ring(center, x, z, radius, segments);
  shape.ring(center, y, z, radius, segments);
  if (filled) {
    hemisphereFaces(shape, center, x, y, z, radius, segments);
    hemisphereFaces(shape, center, x, y, scale(z, -1), radius, segments);
  }
}

// The faces of the half globe about `center` toward `pole`, from its equator up.
function hemisphereFaces(shape, center, u, v, pole, radius, segments) {
  const steps = SPHERE_RINGS / 2;
  let lower = shape.ring(center, u, v, radius, segments, { outline: false });
  for (let step = 1; step < steps; step += 1) {
    const angle = (Math.PI / 2) * (step / steps);
    const ringCenter = add(center, scale(pole, Math.sin(angle) * radius));
    const upper = shape.ring(ringCenter, u, v, Math.cos(angle) * radius, segments, {
      outline: false,
    });
    shape.band(lower, upper);
    lower = upper;
  }
  shape.fan(add(center, scale(pole, radius)), lower);
}

// The local polygon `points` turned and moved; a ring at their own heights, and with `height` a
// second ring that high above the lowest of them, joined by thin vertical edges.
function polygonAreaShape(shape, center, rotation, points, height) {
  const placed = points.map((point) => add(center, rotate(rotation, point)));
  const lower = placed.map((point) => shape.point(point));
  for (let i = 0; i < lower.length; i += 1) {
    shape.line(lower[i], lower[(i + 1) % lower.length]);
  }
  if (height === 0) {
    return;
  }
  const top = Math.min(...placed.map((point) => point[2])) + height;
  const upper = placed.map((point) => shape.point([point[0], point[1], top]));
  for (let i = 0; i < upper.length; i += 1) {
    shape.line(upper[i], upper[(i + 1) % upper.length]);
    shape.thinLines.push(shape.line(lower[i], upper[i]));
  }
}

function areaShape(shape, area, filled) {
  const center = area.position === undefined ? [0, 0, 0] : pointOf(area.position, 'position');
  const rotation = rotationOf(area.rotation, 'rotation');
  if (area.type === 'box') {
    const min = area.min === undefined ? [-1, -1, -1] : pointOf(area.min, 'min');
    const max = area.max === undefined ? [1, 1, 1] : pointOf(area.max, 'max');
    if (!(min[0] < max[0] && min[1] < max[1] && min[2] < max[2])) {
      fail('a box area needs min below max on every axis');
    }
    boxShape(shape, center, rotation, min, max, filled);
  } else if (area.type === 'sphere') {
    const radius = numberIn(area.radius, undefined, 0, MAX_COORDINATE, 'radius', { above: true });
    if (radius >= MAX_COORDINATE) {
      fail(`radius must be below ${MAX_COORDINATE}`);
    }
    sphereShape(shape, center, rotation, radius, DEFAULT_SEGMENTS, filled);
  } else {
    if (!Array.isArray(area.points) || area.points.length < 3 || area.points.length > 256) {
      fail('a shape area needs 3 to 256 points');
    }
    const points = area.points.map((point, index) => pointOf(point, `points[${index}]`));
    const height = numberIn(area.height, 0, 0, MAX_COORDINATE, 'height');
    if (height >= MAX_COORDINATE) {
      fail(`height must be below ${MAX_COORDINATE}`);
    }
    polygonAreaShape(shape, center, rotation, points, height);
  }
}

function segmentsOf(value) {
  return numberIn(value, DEFAULT_SEGMENTS, MIN_SEGMENTS, MAX_SEGMENTS, 'segments', {
    integer: true,
  });
}

function radiusOf(value) {
  return numberIn(value, undefined, MIN_RADIUS, MAX_COORDINATE, 'radius');
}

function normalOf(value) {
  const normal = value === undefined ? [0, 0, 1] : pointOf(value, 'normal');
  if (!(length(normal) > 1e-6)) {
    fail('normal must not be zero');
  }
  return unit(normal);
}

// A cone from a base ring to a tip: the ring, four lines to the tip, and its faces.
function coneShape(shape, base, tip, radius, segments, filled) {
  const axis = unit(sub(tip, base));
  const [u, v] = planeOf(axis);
  const ring = shape.ring(base, u, v, radius, segments);
  const apex = shape.point(tip);
  for (let i = 0; i < 4; i += 1) {
    shape.line(ring[Math.floor((i * segments) / 4)], apex);
  }
  if (filled) {
    for (let i = 0; i < segments; i += 1) {
      shape.triangle(ring[i], ring[(i + 1) % segments], apex);
    }
    shape.fan(base, ring);
  }
}

function roundVolumeShape(shape, volume, filled) {
  const from = pointOf(volume.from, 'from');
  const to = pointOf(volume.to, 'to');
  const radius = radiusOf(volume.radius);
  const segments = segmentsOf(volume.segments);
  const span = length(sub(to, from));
  if (volume.type === 'capsule' && span < 1e-6) {
    sphereShape(shape, from, [0, 0, 0, 1], radius, segments, filled);
    return;
  }
  if (span < 1e-6) {
    fail(`a ${volume.type} needs from and to apart`);
  }
  if (volume.type === 'cone') {
    coneShape(shape, from, to, radius, segments, filled);
    return;
  }
  const axis = unit(sub(to, from));
  const [u, v] = planeOf(axis);
  const lower = shape.ring(from, u, v, radius, segments);
  const upper = shape.ring(to, u, v, radius, segments);
  for (let i = 0; i < 4; i += 1) {
    const at = Math.floor((i * segments) / 4);
    shape.line(lower[at], upper[at]);
  }
  if (volume.type === 'cylinder') {
    if (filled) {
      shape.band(lower, upper);
      shape.fan(from, lower);
      shape.fan(to, upper);
    }
    return;
  }
  // A capsule's ends: half circles in the two planes through its axis, and half globes.
  for (const [end, pole] of [
    [from, scale(axis, -1)],
    [to, axis],
  ]) {
    for (const side of [u, v]) {
      let previous = null;
      for (let i = 0; i <= segments / 2; i += 1) {
        const angle = (2 * Math.PI * i) / segments;
        const offset = add(
          scale(side, Math.cos(angle) * radius),
          scale(pole, Math.sin(angle) * radius),
        );
        const index = shape.point(add(end, offset));
        if (previous !== null) {
          shape.line(previous, index);
        }
        previous = index;
      }
    }
    if (filled) {
      hemisphereFaces(shape, end, u, v, pole, radius, segments);
    }
  }
  if (filled) {
    shape.band(lower, upper);
  }
}

function arrowShape(shape, arrow, filled) {
  const from = pointOf(arrow.from, 'from');
  const to = pointOf(arrow.to, 'to');
  const span = length(sub(to, from));
  if (span < 1e-6) {
    fail('an arrow needs from and to apart');
  }
  const head = numberIn(arrow.headLength, Math.min(0.5, span / 4), 0, span, 'headLength', {
    above: true,
  });
  const base = add(to, scale(unit(sub(from, to)), head));
  shape.line(shape.point(from), shape.point(base));
  coneShape(shape, base, to, head / 3, ARROW_SEGMENTS, filled);
}

function circleShape(shape, circle, filled) {
  const center = pointOf(circle.position, 'position');
  const [u, v] = planeOf(normalOf(circle.normal));
  const ring = shape.ring(center, u, v, radiusOf(circle.radius), segmentsOf(circle.segments));
  if (filled) {
    shape.fan(center, ring);
  }
}

function gridShape(shape, grid) {
  const center = pointOf(grid.position, 'position');
  const [u, v] = planeOf(normalOf(grid.normal));
  const size = numberIn(grid.size, 10, MIN_RADIUS, MAX_COORDINATE, 'size');
  const divisions = numberIn(grid.divisions, 10, 1, 64, 'divisions', { integer: true });
  const half = size / 2;
  for (let i = 0; i <= divisions; i += 1) {
    const at = -half + (size * i) / divisions;
    for (const [along, across] of [
      [u, v],
      [v, u],
    ]) {
      const start = add(center, add(scale(across, at), scale(along, -half)));
      const end = add(center, add(scale(across, at), scale(along, half)));
      shape.line(shape.point(start), shape.point(end));
    }
  }
}

function axesShape(shape, axes) {
  const origin = pointOf(axes.position, 'position');
  const rotation = rotationOf(axes.rotation, 'rotation');
  const size = numberIn(axes.size, 1, MIN_RADIUS, MAX_COORDINATE, 'size');
  const center = shape.point(origin);
  for (const direction of [
    [1, 0, 0],
    [0, 1, 0],
    [0, 0, 1],
  ]) {
    shape.line(center, shape.point(add(origin, scale(rotate(rotation, direction), size))));
  }
  shape.lineColors = AXIS_COLORS;
}

function polylineShape(shape, polyline) {
  const { points } = polyline;
  if (!Array.isArray(points) || points.length < 2 || points.length > 256) {
    fail('a polyline needs 2 to 256 points');
  }
  if (polyline.closed !== undefined && typeof polyline.closed !== 'boolean') {
    fail('closed must be a boolean');
  }
  const indices = points.map((point, index) => shape.point(pointOf(point, `points[${index}]`)));
  for (let i = 1; i < indices.length; i += 1) {
    shape.line(indices[i - 1], indices[i]);
  }
  if (polyline.closed && indices.length > 2) {
    shape.line(indices[indices.length - 1], indices[0]);
  }
}

function meshShape(shape, mesh, filled) {
  const { points, indices } = mesh;
  if (!Array.isArray(points) || points.length < 3 || points.length > 1024) {
    fail('a mesh needs 3 to 1024 points');
  }
  const valid =
    Array.isArray(indices) &&
    indices.length >= 3 &&
    indices.length <= 3072 &&
    indices.length % 3 === 0 &&
    indices.every((index) => Number.isInteger(index) && index >= 0 && index < points.length);
  if (!valid) {
    fail('mesh indices are 3 to 3072 point indices in triangle triples');
  }
  if (mesh.wireframe !== undefined && typeof mesh.wireframe !== 'boolean') {
    fail('wireframe must be a boolean');
  }
  points.forEach((point, index) => shape.point(pointOf(point, `points[${index}]`)));
  const wireframe = mesh.wireframe ?? true;
  for (let i = 0; i < indices.length; i += 3) {
    const [a, b, c] = [indices[i], indices[i + 1], indices[i + 2]];
    if (wireframe) {
      shape.line(a, b);
      shape.line(b, c);
      shape.line(c, a);
    }
    if (filled) {
      shape.triangle(a, b, c);
    }
  }
}

function build(primitive, filled) {
  if (primitive === null || typeof primitive !== 'object') {
    fail('a primitive is an object with a type');
  }
  const shape = new Shape();
  const { type } = primitive;
  if (filled && UNFILLED.has(type)) {
    fail(`fillColor does not apply to a ${type === 'shape' ? 'polygon area' : type}`);
  }
  if (type === 'box' || type === 'sphere' || type === 'shape') {
    areaShape(shape, primitive, filled);
  } else if (type === 'line') {
    shape.line(
      shape.point(pointOf(primitive.from, 'from')),
      shape.point(pointOf(primitive.to, 'to')),
    );
  } else if (type === 'arrow') {
    arrowShape(shape, primitive, filled);
  } else if (type === 'capsule' || type === 'cone' || type === 'cylinder') {
    roundVolumeShape(shape, primitive, filled);
  } else if (type === 'circle') {
    circleShape(shape, primitive, filled);
  } else if (type === 'grid') {
    gridShape(shape, primitive);
  } else if (type === 'axes') {
    axesShape(shape, primitive);
  } else if (type === 'polyline') {
    polylineShape(shape, primitive);
  } else if (type === 'mesh') {
    meshShape(shape, primitive, filled);
  } else {
    fail(`unknown primitive type ${String(type)}`);
  }
  return shape;
}

function styleOf(style) {
  if (style !== undefined && (style === null || typeof style !== 'object')) {
    fail('a style is an object');
  }
  const given = style ?? {};
  if (given.depthTest !== undefined && typeof given.depthTest !== 'boolean') {
    fail('depthTest must be a boolean');
  }
  return {
    color: colorOf(given.color, DEFAULT_COLOR, 'color'),
    fill: given.fillColor === undefined ? 0 : colorOf(given.fillColor, 0, 'fillColor'),
    width: numberIn(given.width, DEFAULT_WIDTH, MIN_WIDTH, MAX_WIDTH, 'width'),
    depthTest: given.depthTest ?? true,
    durationMs: numberIn(given.durationMs, 0, 0, MAX_DURATION_MS, 'durationMs', { integer: true }),
  };
}

class ResourceGizmos {
  constructor({ send, clock = () => performance.now() } = {}) {
    this.send = send;
    this.clock = clock;
    // resource -> id -> { units, until }; `until` 0 lasts until removed
    this.owners = new Map();
  }

  // The resource's live drawings, the ones whose time is over forgotten.
  drawings(owner) {
    const drawings = this.owners.get(owner) ?? new Map();
    const now = this.clock();
    for (const [id, drawing] of drawings) {
      if (drawing.until !== 0 && now >= drawing.until) {
        drawings.delete(id);
      }
    }
    return drawings;
  }

  unitsDrawn() {
    let units = 0;
    for (const owner of this.owners.keys()) {
      for (const drawing of this.drawings(owner).values()) {
        units += drawing.units;
      }
    }
    return units;
  }

  set(owner, id, primitive, style) {
    const name = idOf(id);
    const look = styleOf(style);
    const shape = build(primitive, look.fill !== 0);
    const drawings = this.drawings(owner);
    const previous = drawings.get(name);
    if (!previous && drawings.size >= MAX_IDS) {
      throw new RangeError(`Gizmos: a resource draws at most ${MAX_IDS} IDs`);
    }
    const units = shape.units();
    if (this.unitsDrawn() - (previous?.units ?? 0) + units > MAX_UNITS) {
      throw new RangeError(`Gizmos: drawings use at most ${MAX_UNITS} geometry units`);
    }
    const message = {
      op: 'gizmoSet',
      owner,
      id: name,
      points: shape.points.flat(),
      lines: shape.lines,
      triangles: shape.triangles,
      color: look.color,
      fill: look.fill,
      width: look.width,
      depthTest: look.depthTest,
      durationMs: look.durationMs,
    };
    if (shape.lineColors) {
      message.lineColors = shape.lineColors;
    }
    if (shape.thinLines.length > 0) {
      message.thinLines = shape.thinLines;
    }
    this.send(message);
    const until = look.durationMs === 0 ? 0 : this.clock() + look.durationMs;
    drawings.set(name, { units, until });
    this.owners.set(owner, drawings);
  }

  setTransform(owner, id, position, rotation) {
    const name = idOf(id);
    const [x, y, z] = pointOf(position, 'position');
    const [qx, qy, qz, qw] = rotationOf(rotation ?? IDENTITY, 'rotation');
    if (!this.drawings(owner).has(name)) {
      return false;
    }
    this.send({
      op: 'gizmoTransform',
      owner,
      id: name,
      position: { x, y, z },
      rotation: { x: qx, y: qy, z: qz, w: qw },
    });
    return true;
  }

  remove(owner, id) {
    const name = idOf(id);
    const drawings = this.drawings(owner);
    if (!drawings.delete(name)) {
      return false;
    }
    this.send({ op: 'gizmoRemove', owner, id: name });
    return true;
  }

  removeOwner(owner) {
    if (this.owners.delete(owner)) {
      this.send({ op: 'gizmoRemoveOwner', owner });
    }
  }

  // Another level: the game client cleared the drawings itself.
  levelChanged() {
    this.owners.clear();
  }

  facade(owner) {
    if (typeof owner !== 'string' || owner.length === 0) {
      fail("the caller's resource cannot be identified");
    }
    return Object.freeze({
      set: (id, primitive, style) => {
        this.set(owner, id, primitive, style);
      },
      setTransform: (id, position, rotation) => this.setTransform(owner, id, position, rotation),
      remove: (id) => this.remove(owner, id),
      clear: () => this.removeOwner(owner),
    });
  }
}

module.exports = { ResourceGizmos };
