'use strict';

// An area's collision walls: the pieces an area's invisible boundary is built of.
// Each piece is a box: its centre, its half sizes along its own axes and its rotation, in the
// world. The props module (props/props.cjs, spawnWall) shows each as the game's 2 m primitive
// cube, scaled, drawn with a material that draws nothing; every player's game collides with it.
//
//   box      four side walls on its outline (in the box's own frame, so a turned box turns its
//            walls), and a floor and a ceiling plate across the whole interior when asked;
//   shape    a wall on each edge of the outline, standing from its lowest corner up the height,
//            and floor and ceiling plates as strips across the interior (one per strip row of
//            the outline, each from where a row enters the outline to where it leaves it);
//   sphere   a shell of panels in latitude bands, each panel tangent to the sphere, and a cap
//            at each pole.
//
// The walls are centred on the outline, `thickness` metres thick (0.2 by default, 0.05 to 2).
// A piece longer than the cube scales to is cut into equal parts.

const { Vector3 } = require('../math.cjs');

const DEFAULT_THICKNESS = 0.2;
const MIN_THICKNESS = 0.05;
const MAX_THICKNESS = 2;
// The longest half size of one piece: the props' largest scale of the 2 m cube, with a margin.
const MAX_HALF = 95;
// The width of the strips a polygon's floor and ceiling are laid in, and the most rows.
const STRIP = 1;
const MAX_ROWS = 256;
// The most pieces one area's walls may take.
const MAX_PIECES = 1024;
// Sphere panels overlap their neighbours by this share, so the shell has no gaps.
const OVERLAP = 1.1;

function thicknessOf(value) {
  if (value === undefined) {
    return DEFAULT_THICKNESS;
  }
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError('Area wallThickness takes a number of metres');
  }
  if (value < MIN_THICKNESS || value > MAX_THICKNESS) {
    throw new RangeError(`Area wallThickness must be ${MIN_THICKNESS} to ${MAX_THICKNESS} metres`);
  }
  return value;
}

// The rotation matrix of a quaternion { w, x, y, z }, rows first.
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

function turn(m, v) {
  return {
    x: m[0] * v.x + m[1] * v.y + m[2] * v.z,
    y: m[3] * v.x + m[4] * v.y + m[5] * v.z,
    z: m[6] * v.x + m[7] * v.y + m[8] * v.z,
  };
}

// The quaternion { w, x, y, z } of the rotation whose columns are the axes x, y and z.
function quaternionOfAxes(ax, ay, az) {
  const trace = ax.x + ay.y + az.z;
  if (trace > 0) {
    const s = Math.sqrt(trace + 1) * 2;
    return { w: s / 4, x: (ay.z - az.y) / s, y: (az.x - ax.z) / s, z: (ax.y - ay.x) / s };
  }
  if (ax.x > ay.y && ax.x > az.z) {
    const s = Math.sqrt(1 + ax.x - ay.y - az.z) * 2;
    return { w: (ay.z - az.y) / s, x: s / 4, y: (ay.x + ax.y) / s, z: (az.x + ax.z) / s };
  }
  if (ay.y > az.z) {
    const s = Math.sqrt(1 + ay.y - ax.x - az.z) * 2;
    return { w: (az.x - ax.z) / s, x: (ay.x + ax.y) / s, y: s / 4, z: (az.y + ay.z) / s };
  }
  const s = Math.sqrt(1 + az.z - ax.x - ay.y) * 2;
  return { w: (ax.y - ay.x) / s, x: (az.x + ax.z) / s, y: (az.y + ay.z) / s, z: s / 4 };
}

function yawQuaternion(angle) {
  return { w: Math.cos(angle / 2), x: 0, y: 0, z: Math.sin(angle / 2) };
}

// Pieces of one box given in a frame (origin, rotation): cut into equal parts where too long.
function framedPieces(origin, rotation, center, half) {
  const m = matrixOf(rotation);
  const cuts = {
    x: Math.ceil(half.x / MAX_HALF),
    y: Math.ceil(half.y / MAX_HALF),
    z: Math.ceil(half.z / MAX_HALF),
  };
  const part = { x: half.x / cuts.x, y: half.y / cuts.y, z: half.z / cuts.z };
  const pieces = [];
  for (let i = 0; i < cuts.x; i++) {
    for (let j = 0; j < cuts.y; j++) {
      for (let k = 0; k < cuts.z; k++) {
        const local = {
          x: center.x - half.x + part.x * (2 * i + 1),
          y: center.y - half.y + part.y * (2 * j + 1),
          z: center.z - half.z + part.z * (2 * k + 1),
        };
        const offset = turn(m, local);
        pieces.push({
          center: { x: origin.x + offset.x, y: origin.y + offset.y, z: origin.z + offset.z },
          half: { ...part },
          rotation: { w: rotation.w, x: rotation.x, y: rotation.y, z: rotation.z },
        });
      }
    }
  }
  return pieces;
}

function boxPieces(geometry, options) {
  const { min, max, position, rotation } = geometry;
  const t = options.thickness;
  const size = { x: max.x - min.x, y: max.y - min.y, z: max.z - min.z };
  if (size.x <= t || size.y <= t || size.z <= t) {
    throw new RangeError('The box area is too small for walls this thick');
  }
  const mid = { x: (min.x + max.x) / 2, y: (min.y + max.y) / 2, z: (min.z + max.z) / 2 };
  const pieces = [];
  const add = (center, half) => pieces.push(...framedPieces(position, rotation, center, half));
  for (const x of [min.x, max.x]) {
    add({ x, y: mid.y, z: mid.z }, { x: t / 2, y: size.y / 2 + t / 2, z: size.z / 2 });
  }
  for (const y of [min.y, max.y]) {
    add({ x: mid.x, y, z: mid.z }, { x: size.x / 2 + t / 2, y: t / 2, z: size.z / 2 });
  }
  const plate = { x: size.x / 2 + t / 2, y: size.y / 2 + t / 2, z: t / 2 };
  if (options.floor) {
    add({ x: mid.x, y: mid.y, z: min.z }, plate);
  }
  if (options.ceiling) {
    add({ x: mid.x, y: mid.y, z: max.z }, plate);
  }
  return pieces;
}

// Whether segments ab and cd cross (touching ends do not count).
function crossing(a, b, c, d) {
  const side = (p, q, r) => (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
  const d1 = side(c, d, a);
  const d2 = side(c, d, b);
  const d3 = side(a, b, c);
  const d4 = side(a, b, d);
  return d1 * d2 < 0 && d3 * d4 < 0;
}

function simpleOutline(corners) {
  const n = corners.length;
  for (let i = 0; i < n; i++) {
    for (let j = i + 1; j < n; j++) {
      const neighbours = j === i + 1 || (i === 0 && j === n - 1);
      if (!neighbours) {
        const a = corners[i];
        const b = corners[(i + 1) % n];
        if (crossing(a, b, corners[j], corners[(j + 1) % n])) {
          return false;
        }
      }
    }
  }
  return true;
}

// Where a row at y crosses the outline, in order along x.
function rowCrossings(corners, y) {
  const xs = [];
  for (let i = 0, j = corners.length - 1; i < corners.length; j = i++) {
    const a = corners[i];
    const b = corners[j];
    if (a.y > y !== b.y > y) {
      xs.push(a.x + ((y - a.y) * (b.x - a.x)) / (b.y - a.y));
    }
  }
  return xs.sort((p, q) => p - q);
}

function shapePieces(geometry, corners, options) {
  const t = options.thickness;
  if (!(geometry.height > t)) {
    throw new RangeError('A shape area needs a height above the wall thickness for collision');
  }
  if (!simpleOutline(corners)) {
    throw new RangeError('A shape area needs a simple outline (no crossing edges) for collision');
  }
  const floor = Math.min(...corners.map((c) => c.z));
  const height = geometry.height;
  const pieces = [];
  for (let i = 0; i < corners.length; i++) {
    const a = corners[i];
    const b = corners[(i + 1) % corners.length];
    const length = Math.hypot(b.x - a.x, b.y - a.y);
    if (length === 0) {
      continue;
    }
    const origin = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2, z: floor + height / 2 };
    const facing = yawQuaternion(Math.atan2(b.y - a.y, b.x - a.x));
    pieces.push(
      ...framedPieces(
        origin,
        facing,
        { x: 0, y: 0, z: 0 },
        { x: length / 2 + t / 2, y: t / 2, z: height / 2 },
      ),
    );
  }
  const levels = [];
  if (options.floor) {
    levels.push(floor);
  }
  if (options.ceiling) {
    levels.push(floor + height);
  }
  if (levels.length) {
    const low = Math.min(...corners.map((c) => c.y));
    const high = Math.max(...corners.map((c) => c.y));
    const strip = Math.max(STRIP, (high - low) / MAX_ROWS);
    const identity = { w: 1, x: 0, y: 0, z: 0 };
    for (let y = low + strip / 2; y < high; y += strip) {
      const xs = rowCrossings(corners, y);
      for (let k = 0; k + 1 < xs.length; k += 2) {
        const width = xs[k + 1] - xs[k];
        for (const z of levels) {
          pieces.push(
            ...framedPieces(
              { x: (xs[k] + xs[k + 1]) / 2, y, z },
              identity,
              { x: 0, y: 0, z: 0 },
              { x: width / 2 + t / 2, y: strip / 2 + t / 2, z: t / 2 },
            ),
          );
        }
      }
    }
  }
  return pieces;
}

function spherePieces(geometry, options) {
  const t = options.thickness;
  const r = geometry.radius;
  if (r <= t * 2) {
    throw new RangeError('The sphere area is too small for walls this thick');
  }
  const { position } = geometry;
  const bands = Math.min(12, Math.max(4, Math.ceil((Math.PI * r) / 2.5)));
  const pieces = [];
  for (let band = 0; band < bands; band++) {
    const latitude = -Math.PI / 2 + (Math.PI * (band + 0.5)) / bands;
    const ring = r * Math.cos(latitude);
    const count = Math.min(24, Math.max(4, Math.ceil((2 * Math.PI * ring) / 2.5)));
    const width = ((2 * Math.PI * ring) / count) * OVERLAP + t;
    const tall = ((Math.PI * r) / bands) * OVERLAP + t;
    for (let i = 0; i < count; i++) {
      const longitude = (2 * Math.PI * (i + 0.5)) / count;
      const out = {
        x: Math.cos(latitude) * Math.cos(longitude),
        y: Math.cos(latitude) * Math.sin(longitude),
        z: Math.sin(latitude),
      };
      const east = { x: -Math.sin(longitude), y: Math.cos(longitude), z: 0 };
      const north = {
        x: out.y * east.z - out.z * east.y,
        y: out.z * east.x - out.x * east.z,
        z: out.x * east.y - out.y * east.x,
      };
      pieces.push({
        center: { x: position.x + out.x * r, y: position.y + out.y * r, z: position.z + out.z * r },
        half: { x: width / 2, y: tall / 2, z: t / 2 },
        rotation: quaternionOfAxes(east, north, out),
      });
    }
  }
  // The poles: what the narrowest bands leave open.
  const cap = r * Math.sin(Math.PI / bands) + t;
  for (const sign of [-1, 1]) {
    pieces.push({
      center: { x: position.x, y: position.y, z: position.z + sign * r },
      half: { x: cap, y: cap, z: t / 2 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  }
  return pieces;
}

// The world corners of a shape area's outline (the area's rotation applied to its points).
function cornersOf(geometry) {
  const m = matrixOf(geometry.rotation);
  return geometry.points.map((p) => {
    const offset = turn(m, p);
    return new Vector3(
      geometry.position.x + offset.x,
      geometry.position.y + offset.y,
      geometry.position.z + offset.z,
    );
  });
}

// The pieces of an area's walls; throws a RangeError for a shape that cannot have them.
function wallPieces(geometry, options) {
  let pieces;
  if (geometry.type === 'box') {
    pieces = boxPieces(geometry, options);
  } else if (geometry.type === 'sphere') {
    pieces = spherePieces(geometry, options);
  } else {
    pieces = shapePieces(geometry, cornersOf(geometry), options);
  }
  if (pieces.length > MAX_PIECES) {
    throw new RangeError(`The area's walls would take more than ${MAX_PIECES} pieces`);
  }
  return pieces;
}

module.exports = { wallPieces, thicknessOf, DEFAULT_THICKNESS };
