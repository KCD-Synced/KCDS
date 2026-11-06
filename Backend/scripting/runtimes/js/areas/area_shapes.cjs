'use strict';

// The shapes of script areas: a box, a sphere and a closed polygon with a height. A
// shape is built once from validated fields and then only asked two things: its bounds, for the
// grid, and whether a point lies inside. Both are plain arithmetic on numbers, because the
// containment test runs for every area a moving body is near.
//
// Z is up. A polygon lies in the XY plane; its floor is its lowest corner and it reaches `height`
// above that, or without a ceiling or a floor when the height is 0. A box and a polygon are
// placed from `position` and turned by `rotation`; a sphere only needs `position`.

const { Quaternion, Vector3, rotation } = require('../math.cjs');

// Where the bounds of an axis without an end are drawn.
const NO_LIMIT = 1e6;
const MIN_CORNERS = 3;
const MAX_CORNERS = 256;
const TYPES = ['box', 'shape', 'sphere'];

function isFinite3(value) {
  return (
    value !== null &&
    typeof value === 'object' &&
    [value.x, value.y, value.z].every((c) => typeof c === 'number' && Number.isFinite(c))
  );
}

function vectorOf(value, name) {
  if (!isFinite3(value)) {
    throw new TypeError(`Area ${name} takes a position with finite x, y and z`);
  }
  return new Vector3(value.x, value.y, value.z);
}

// A unit quaternion from a Quaternion, an {x, y, z, w} or Euler angles in degrees.
function rotationOf(value) {
  const components = value !== null && typeof value === 'object' ? [value.x, value.y, value.z] : [];
  if (value !== null && typeof value === 'object' && 'w' in value) {
    components.push(value.w);
  }
  if (
    components.length < 3 ||
    !components.every((c) => typeof c === 'number' && Number.isFinite(c))
  ) {
    throw new TypeError('Area rotation takes a quaternion or finite Euler angles');
  }
  const q = rotation(value);
  const length = Math.hypot(q.w, q.x, q.y, q.z);
  if (!Number.isFinite(length) || length < 1e-6) {
    throw new RangeError('Area rotation must not be a zero quaternion');
  }
  return new Quaternion(q.w / length, q.x / length, q.y / length, q.z / length);
}

// The rotation matrix of a unit quaternion, row by row.
function matrixOf(q) {
  const { w, x, y, z } = q;
  return [
    1 - 2 * (y * y + z * z),
    2 * (x * y - w * z),
    2 * (x * z + w * y),
    2 * (x * y + w * z),
    1 - 2 * (x * x + z * z),
    2 * (y * z - w * x),
    2 * (x * z - w * y),
    2 * (y * z + w * x),
    1 - 2 * (x * x + y * y),
  ];
}

// Validated geometry fields from a definition, the fields it leaves out taken from `base` (the
// geometry of the area being reshaped, or nothing). Throws for what no shape can be built from.
function geometryOf(definition, base = {}) {
  const pick = (name) => (definition[name] !== undefined ? definition[name] : base[name]);
  const type = definition.type !== undefined ? definition.type : base.type;
  if (!TYPES.includes(type)) {
    throw new TypeError(`Area type must be one of ${TYPES.join(', ')}`);
  }
  const geometry = {
    type,
    position:
      pick('position') === undefined
        ? new Vector3(0, 0, 0)
        : vectorOf(pick('position'), 'position'),
    rotation:
      pick('rotation') === undefined ? new Quaternion(1, 0, 0, 0) : rotationOf(pick('rotation')),
  };
  if (type === 'box') {
    geometry.min = vectorOf(pick('min'), 'min');
    geometry.max = vectorOf(pick('max'), 'max');
    if (
      geometry.max.x <= geometry.min.x ||
      geometry.max.y <= geometry.min.y ||
      geometry.max.z <= geometry.min.z
    ) {
      throw new RangeError('A box area needs every component of max above min');
    }
  } else if (type === 'sphere') {
    const radius = pick('radius');
    if (typeof radius !== 'number' || !Number.isFinite(radius) || radius <= 0) {
      throw new RangeError('A sphere area needs a radius above 0');
    }
    geometry.radius = radius;
  } else {
    const points = pick('points');
    if (!Array.isArray(points) || points.length < MIN_CORNERS || points.length > MAX_CORNERS) {
      throw new RangeError(`A shape area needs ${MIN_CORNERS} to ${MAX_CORNERS} points`);
    }
    geometry.points = points.map((point) => vectorOf(point, 'points'));
    const height = pick('height') === undefined ? 0 : pick('height');
    if (typeof height !== 'number' || !Number.isFinite(height) || height < 0) {
      throw new RangeError('A shape area needs a height of 0 or more');
    }
    geometry.height = height;
  }
  return geometry;
}

// The corners of a polygon in the world, and its lowest Z.
function worldCorners(geometry, matrix) {
  const { position, points } = geometry;
  return points.map(
    (p) =>
      new Vector3(
        position.x + matrix[0] * p.x + matrix[1] * p.y + matrix[2] * p.z,
        position.y + matrix[3] * p.x + matrix[4] * p.y + matrix[5] * p.z,
        position.z + matrix[6] * p.x + matrix[7] * p.y + matrix[8] * p.z,
      ),
  );
}

// Whether (x, y) lies inside the outline, by the even-odd rule.
function insideOutline(corners, x, y) {
  let inside = false;
  for (let i = 0, j = corners.length - 1; i < corners.length; j = i++) {
    const a = corners[i];
    const b = corners[j];
    if (a.y > y !== b.y > y && x < ((b.x - a.x) * (y - a.y)) / (b.y - a.y) + a.x) {
      inside = !inside;
    }
  }
  return inside;
}

// A corner of the bounds as a vector, an unbounded axis drawn at NO_LIMIT.
function boundVector(corner) {
  const limit = (value) => Math.max(-NO_LIMIT, Math.min(NO_LIMIT, value));
  return new Vector3(limit(corner.x), limit(corner.y), limit(corner.z));
}

// The built shape: { bounds: { min, max } (world, Vector3), contains(x, y, z) }.
function buildShape(geometry) {
  const { position } = geometry;
  const matrix = matrixOf(geometry.rotation);
  if (geometry.type === 'sphere') {
    const r = geometry.radius;
    return {
      bounds: {
        min: new Vector3(position.x - r, position.y - r, position.z - r),
        max: new Vector3(position.x + r, position.y + r, position.z + r),
      },
      contains(x, y, z) {
        const dx = x - position.x;
        const dy = y - position.y;
        const dz = z - position.z;
        return dx * dx + dy * dy + dz * dz <= r * r;
      },
    };
  }
  if (geometry.type === 'box') {
    const { min, max } = geometry;
    const low = { x: Infinity, y: Infinity, z: Infinity };
    const high = { x: -Infinity, y: -Infinity, z: -Infinity };
    for (const cx of [min.x, max.x]) {
      for (const cy of [min.y, max.y]) {
        for (const cz of [min.z, max.z]) {
          const wx = position.x + matrix[0] * cx + matrix[1] * cy + matrix[2] * cz;
          const wy = position.y + matrix[3] * cx + matrix[4] * cy + matrix[5] * cz;
          const wz = position.z + matrix[6] * cx + matrix[7] * cy + matrix[8] * cz;
          low.x = Math.min(low.x, wx);
          low.y = Math.min(low.y, wy);
          low.z = Math.min(low.z, wz);
          high.x = Math.max(high.x, wx);
          high.y = Math.max(high.y, wy);
          high.z = Math.max(high.z, wz);
        }
      }
    }
    return {
      bounds: { min: boundVector(low), max: boundVector(high) },
      contains(x, y, z) {
        // The point in the box's own frame: the transpose of the rotation matrix undoes it.
        const dx = x - position.x;
        const dy = y - position.y;
        const dz = z - position.z;
        const lx = matrix[0] * dx + matrix[3] * dy + matrix[6] * dz;
        const ly = matrix[1] * dx + matrix[4] * dy + matrix[7] * dz;
        const lz = matrix[2] * dx + matrix[5] * dy + matrix[8] * dz;
        return (
          lx >= min.x && lx <= max.x && ly >= min.y && ly <= max.y && lz >= min.z && lz <= max.z
        );
      },
    };
  }
  const corners = worldCorners(geometry, matrix);
  let floor = Infinity;
  const low = { x: Infinity, y: Infinity, z: 0 };
  const high = { x: -Infinity, y: -Infinity, z: 0 };
  for (const c of corners) {
    floor = Math.min(floor, c.z);
    low.x = Math.min(low.x, c.x);
    low.y = Math.min(low.y, c.y);
    high.x = Math.max(high.x, c.x);
    high.y = Math.max(high.y, c.y);
  }
  const ceiling = geometry.height > 0 ? floor + geometry.height : Infinity;
  const bottom = geometry.height > 0 ? floor : -Infinity;
  low.z = geometry.height > 0 ? floor : -NO_LIMIT;
  high.z = geometry.height > 0 ? ceiling : NO_LIMIT;
  return {
    bounds: { min: boundVector(low), max: boundVector(high) },
    contains(x, y, z) {
      return z >= bottom && z <= ceiling && insideOutline(corners, x, y);
    },
  };
}

module.exports = { buildShape, geometryOf, rotationOf, NO_LIMIT, TYPES };
