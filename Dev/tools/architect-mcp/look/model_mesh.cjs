'use strict';

// The drawn triangles of a model (.cgf), and the first of them a ray meets: the walls, vaults and
// floors of an opened building as the player sees them. The game's own rays meet only collision,
// and an opened building keeps none of its insides (open_model.cjs), so its vault is invisible to
// them: a chandelier "snapped" to its ceiling found the roof, or nothing.
//
// The layout is the one cgf_writer.cjs writes and the game's own models have: data streams
// (0x1016: flags, type, count, element size, 2 reserved, then the data) of type 15 (16 bytes a
// vertex: half x, y, z, ...) or 0 (three floats), and of type 5 (indices, 2 or 4 bytes); subsets
// (0x1017: flags, count, 2 reserved, then a record each: first index, index count, first vertex,
// vertex count, sub-material id, ...).

const CHUNK_MESH = 0x1000;
const CHUNK_STREAM = 0x1016;
const CHUNK_SUBSETS = 0x1017;
const MESH_STREAMS = 28;
const MESH_SUBSETS = 20;
const STREAM_POSITIONS = 0;
const STREAM_INDICES = 5;
const STREAM_PACKED = 15;
// A ray's hit is this far past its start at least (metres): the face it starts on is not met.
const RAY_START = 0.001;
const SUB_MATERIALS_MOST = 256;

// A half float's value.
function fromHalf(bits) {
  const sign = bits & 0x8000 ? -1 : 1;
  const exponent = (bits >> 10) & 0x1f;
  const fraction = bits & 0x3ff;
  if (exponent === 0) {
    return sign * 2 ** -14 * (fraction / 1024);
  }
  if (exponent === 31) {
    return fraction ? NaN : sign * Infinity;
  }
  return sign * 2 ** (exponent - 15) * (1 + fraction / 1024);
}

// The chunks of a CGF by id.
function chunksOf(bytes) {
  if (bytes.toString('latin1', 0, 4) !== 'CrCh') {
    throw new Error('not a CGF file');
  }
  const count = bytes.readUInt32LE(8);
  const table = bytes.readUInt32LE(12);
  const byId = new Map();
  const list = [];
  for (let index = 0; index < count; index += 1) {
    const at = table + index * 16;
    const chunk = {
      kind: bytes.readUInt16LE(at),
      id: bytes.readUInt32LE(at + 4),
      size: bytes.readUInt32LE(at + 8),
      offset: bytes.readUInt32LE(at + 12),
    };
    list.push(chunk);
    byId.set(chunk.id, chunk);
  }
  return { list, byId };
}

// A stream chunk's type, count, element size and data.
function streamOf(bytes, chunk) {
  const at = chunk.offset;
  return {
    type: bytes.readInt32LE(at + 4),
    count: bytes.readInt32LE(at + 8),
    size: bytes.readInt32LE(at + 12),
    data: at + 24,
  };
}

// Every drawn triangle of the model, in its own frame: { positions: Float32Array (9 numbers a
// triangle), materials: Int32Array (each triangle's sub-material id), min, max }.
function readTriangles(bytes) {
  const { list, byId } = chunksOf(bytes);
  const positions = [];
  const materials = [];
  for (const mesh of list.filter((chunk) => chunk.kind === CHUNK_MESH)) {
    const streams = new Map();
    for (let k = 0; k < 16; k += 1) {
      const chunk = byId.get(bytes.readInt32LE(mesh.offset + MESH_STREAMS + k * 4));
      if (chunk?.kind === CHUNK_STREAM) {
        const stream = streamOf(bytes, chunk);
        streams.set(stream.type, stream);
      }
    }
    const indices = streams.get(STREAM_INDICES);
    const packed = streams.get(STREAM_PACKED);
    const plain = streams.get(STREAM_POSITIONS);
    if (!indices || (!packed && !plain)) {
      continue;
    }
    const vertex = (index) => {
      if (packed) {
        const at = packed.data + index * packed.size;
        return [
          fromHalf(bytes.readUInt16LE(at)),
          fromHalf(bytes.readUInt16LE(at + 2)),
          fromHalf(bytes.readUInt16LE(at + 4)),
        ];
      }
      const at = plain.data + index * plain.size;
      return [bytes.readFloatLE(at), bytes.readFloatLE(at + 4), bytes.readFloatLE(at + 8)];
    };
    const indexAt = (k) =>
      indices.size === 4
        ? bytes.readUInt32LE(indices.data + k * 4)
        : bytes.readUInt16LE(indices.data + k * 2);
    // Each triangle's sub-material from the subsets (the first index of each range).
    const ranges = [];
    const subsets = byId.get(bytes.readInt32LE(mesh.offset + MESH_SUBSETS));
    if (subsets?.kind === CHUNK_SUBSETS) {
      const count = bytes.readInt32LE(subsets.offset + 4);
      const stride = count > 0 ? (subsets.size - 16) / count : 0;
      for (let s = 0; s < count; s += 1) {
        const at = subsets.offset + 16 + s * stride;
        const material = bytes.readInt32LE(at + 16);
        ranges.push({
          first: bytes.readInt32LE(at),
          count: bytes.readInt32LE(at + 4),
          // A sub-material id is small; anything else is a record laid out otherwise.
          material: material >= 0 && material < SUB_MATERIALS_MOST ? material : -1,
        });
      }
    }
    const materialOf = (k) => ranges.find((r) => k >= r.first && k < r.first + r.count)?.material;
    for (let k = 0; k + 2 < indices.count; k += 3) {
      const a = vertex(indexAt(k));
      const b = vertex(indexAt(k + 1));
      const c = vertex(indexAt(k + 2));
      positions.push(...a, ...b, ...c);
      materials.push(materialOf(k) ?? -1);
    }
  }
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < positions.length; i += 3) {
    for (let k = 0; k < 3; k += 1) {
      min[k] = Math.min(min[k], positions[i + k]);
      max[k] = Math.max(max[k], positions[i + k]);
    }
  }
  return {
    positions: Float32Array.from(positions),
    materials: Int32Array.from(materials),
    min,
    max,
  };
}

// A point or direction from the world into a placed model's frame (turned `turn` degrees about
// the up axis, `scale` times its size, standing at `at`), and a normal back out.
function intoModel(point, placed, direction = false) {
  const t = (-(placed.turn ?? 0) * Math.PI) / 180;
  const s = placed.scale ?? 1;
  const x = direction ? point.x : point.x - placed.at.x;
  const y = direction ? point.y : point.y - placed.at.y;
  const z = direction ? point.z : point.z - placed.at.z;
  return {
    x: (x * Math.cos(t) - y * Math.sin(t)) / s,
    y: (x * Math.sin(t) + y * Math.cos(t)) / s,
    z: z / s,
  };
}

function outOfModel(vector, placed) {
  const t = ((placed.turn ?? 0) * Math.PI) / 180;
  return {
    x: vector.x * Math.cos(t) - vector.y * Math.sin(t),
    y: vector.x * Math.sin(t) + vector.y * Math.cos(t),
    z: vector.z,
  };
}

// Whether the ray (from, dir, up to `length`) passes the box (min, max) at all.
function passesBox(from, dir, length, min, max) {
  let near = 0;
  let far = length;
  for (const axis of ['x', 'y', 'z']) {
    const k = { x: 0, y: 1, z: 2 }[axis];
    if (Math.abs(dir[axis]) < 1e-12) {
      if (from[axis] < min[k] || from[axis] > max[k]) {
        return false;
      }
      continue;
    }
    let a = (min[k] - from[axis]) / dir[axis];
    let b = (max[k] - from[axis]) / dir[axis];
    if (a > b) {
      [a, b] = [b, a];
    }
    near = Math.max(near, a);
    far = Math.min(far, b);
    if (near > far) {
      return false;
    }
  }
  return true;
}

// The first drawn triangle the ray from `from` to `to` (world metres) meets among the placed
// models ([{ mesh: readTriangles' answer, at, turn, scale, name }]): { position, normal (facing
// the ray), distance, name, material } or null. Both faces of a triangle are met.
function rayMeshes(placedModels, from, to) {
  const span = { x: to.x - from.x, y: to.y - from.y, z: to.z - from.z };
  const length = Math.hypot(span.x, span.y, span.z);
  if (!(length > 0)) {
    return null;
  }
  const dir = { x: span.x / length, y: span.y / length, z: span.z / length };
  let best = null;
  for (const placed of placedModels) {
    // The direction comes into the model's frame shortened by its scale, so a point `t` along it
    // there is `t` world metres from `from`.
    const start = intoModel(from, placed);
    const way = intoModel(dir, placed, true);
    const reach = length;
    const { positions, materials, min, max } = placed.mesh;
    if (!passesBox(start, way, reach, min, max)) {
      continue;
    }
    for (let tri = 0; tri < materials.length; tri += 1) {
      const i = tri * 9;
      const ax = positions[i];
      const ay = positions[i + 1];
      const az = positions[i + 2];
      const e1x = positions[i + 3] - ax;
      const e1y = positions[i + 4] - ay;
      const e1z = positions[i + 5] - az;
      const e2x = positions[i + 6] - ax;
      const e2y = positions[i + 7] - ay;
      const e2z = positions[i + 8] - az;
      // Möller–Trumbore.
      const px = way.y * e2z - way.z * e2y;
      const py = way.z * e2x - way.x * e2z;
      const pz = way.x * e2y - way.y * e2x;
      const det = e1x * px + e1y * py + e1z * pz;
      if (Math.abs(det) < 1e-12) {
        continue;
      }
      const inv = 1 / det;
      const tx = start.x - ax;
      const ty = start.y - ay;
      const tz = start.z - az;
      const u = (tx * px + ty * py + tz * pz) * inv;
      if (u < 0 || u > 1) {
        continue;
      }
      const qx = ty * e1z - tz * e1y;
      const qy = tz * e1x - tx * e1z;
      const qz = tx * e1y - ty * e1x;
      const v = (way.x * qx + way.y * qy + way.z * qz) * inv;
      if (v < 0 || u + v > 1) {
        continue;
      }
      const t = (e2x * qx + e2y * qy + e2z * qz) * inv;
      const distance = t;
      if (t <= RAY_START || t > reach || (best && distance >= best.distance)) {
        continue;
      }
      // The face's normal, turned toward where the ray came from.
      let nx = e1y * e2z - e1z * e2y;
      let ny = e1z * e2x - e1x * e2z;
      let nz = e1x * e2y - e1y * e2x;
      const n = Math.hypot(nx, ny, nz) || 1;
      nx /= n;
      ny /= n;
      nz /= n;
      if (nx * way.x + ny * way.y + nz * way.z > 0) {
        nx = -nx;
        ny = -ny;
        nz = -nz;
      }
      best = {
        distance,
        normal: outOfModel({ x: nx, y: ny, z: nz }, placed),
        name: placed.name,
        material: materials[tri],
      };
    }
  }
  if (!best) {
    return null;
  }
  const round = (value) => Math.round(value * 1000) / 1000;
  return {
    position: {
      x: round(from.x + dir.x * best.distance),
      y: round(from.y + dir.y * best.distance),
      z: round(from.z + dir.z * best.distance),
    },
    normal: {
      x: round(best.normal.x),
      y: round(best.normal.y),
      z: round(best.normal.z),
    },
    distance: round(best.distance),
    name: best.name,
    material: best.material,
  };
}

module.exports = { readTriangles, rayMeshes, fromHalf };
