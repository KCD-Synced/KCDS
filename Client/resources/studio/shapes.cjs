'use strict';

// The studio's simple props made in the game: a box, a cylinder, a sphere or a plane
// as the game's own static model, written byte for byte as Dev/tools/assets/kcds_studio/
// studio_shapes.py and Dev/tools/assets/blender/kcds_cgf.py write them (the same chunks, the same
// numbers), so the prop menu needs no Python. The game client writes the files into the meshes
// folder (studio/studio_mesh.hpp studioMeshWrite).

const SHAPES = Object.freeze(['box', 'cylinder', 'sphere', 'plane']);
// The game's surface types a prop may have (studio_shapes.py SURFACES).
const SURFACES = Object.freeze([
  'mat_wood',
  'mat_wood_soft',
  'mat_metal',
  'mat_rock',
  'mat_plaster',
  'mat_ceramics',
  'mat_glass',
  'mat_fabric',
  'mat_leather',
  'mat_sack',
  'mat_thatch',
  'mat_wicker',
  'mat_soil',
  'mat_default',
]);
const SMALLEST = 0.01;
const LARGEST = 50;
const SIDES = Object.freeze([3, 128]);
const RINGS = Object.freeze([2, 64]);
const MAX_VERTICES = 65535;
const PHYSICALIZE_DEFAULT = 0x1000;

function cross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

// ---- The shapes (studio_shapes.py) ----

class Builder {
  constructor(material) {
    this.mesh = { positions: [], uvs: [], tangents: [], indices: [], material };
  }

  vertex(position, uv, tangent, bitangent) {
    this.mesh.positions.push([...position]);
    this.mesh.uvs.push([...uv]);
    this.mesh.tangents.push([[...tangent], [...bitangent]]);
    return this.mesh.positions.length - 1;
  }

  triangle(a, b, c) {
    this.mesh.indices.push(a, b, c);
  }

  quad(lowLeft, lowRight, highRight, highLeft) {
    this.triangle(lowLeft, lowRight, highRight);
    this.triangle(lowLeft, highRight, highLeft);
  }
}

function box(width, depth, height, material) {
  const builder = new Builder(material);
  const half = [width / 2, depth / 2, height / 2];
  const sides = [
    [
      [1, 0, 0],
      [0, 1, 0],
    ],
    [
      [-1, 0, 0],
      [0, -1, 0],
    ],
    [
      [0, 1, 0],
      [-1, 0, 0],
    ],
    [
      [0, -1, 0],
      [1, 0, 0],
    ],
    [
      [0, 0, 1],
      [1, 0, 0],
    ],
    [
      [0, 0, -1],
      [-1, 0, 0],
    ],
  ];
  for (const [normal, across] of sides) {
    const up = cross(normal, across);
    const corners = [];
    for (const [su, sv] of [
      [-1, -1],
      [1, -1],
      [1, 1],
      [-1, 1],
    ]) {
      const point = [];
      for (let axis = 0; axis < 3; axis += 1) {
        const offset = normal[axis] + across[axis] * su + up[axis] * sv;
        point.push(offset * half[axis] + half[2] * (axis === 2 ? 1 : 0));
      }
      corners.push(builder.vertex(point, [(su + 1) / 2, 1 - (sv + 1) / 2], across, up));
    }
    builder.quad(...corners);
  }
  return builder.mesh;
}

function cylinder(radius, height, sides, material) {
  const builder = new Builder(material);
  const ringLow = [];
  const ringHigh = [];
  for (let step = 0; step <= sides; step += 1) {
    const angle = (2 * Math.PI * step) / sides;
    const cos = Math.cos(angle);
    const sin = Math.sin(angle);
    const tangent = [-sin, cos, 0];
    const u = step / sides;
    ringLow.push(builder.vertex([radius * cos, radius * sin, 0], [u, 1], tangent, [0, 0, 1]));
    ringHigh.push(builder.vertex([radius * cos, radius * sin, height], [u, 0], tangent, [0, 0, 1]));
  }
  for (let step = 0; step < sides; step += 1) {
    builder.quad(ringLow[step], ringLow[step + 1], ringHigh[step + 1], ringHigh[step]);
  }
  for (const [z, bitangent, facingUp] of [
    [height, [0, 1, 0], true],
    [0, [0, -1, 0], false],
  ]) {
    const flip = facingUp ? -1 : 1;
    const middle = builder.vertex([0, 0, z], [0.5, 0.5], [1, 0, 0], bitangent);
    const rim = [];
    for (let step = 0; step < sides; step += 1) {
      const angle = (2 * Math.PI * step) / sides;
      const cos = Math.cos(angle);
      const sin = Math.sin(angle);
      const uv = [0.5 + 0.5 * cos, 0.5 + 0.5 * sin * flip];
      rim.push(builder.vertex([radius * cos, radius * sin, z], uv, [1, 0, 0], bitangent));
    }
    for (let step = 0; step < sides; step += 1) {
      const following = rim[(step + 1) % sides];
      if (facingUp) {
        builder.triangle(middle, rim[step], following);
      } else {
        builder.triangle(middle, following, rim[step]);
      }
    }
  }
  return builder.mesh;
}

function sphere(radius, rings, sides, material) {
  const builder = new Builder(material);
  const grid = [];
  for (let ring = 0; ring <= rings; ring += 1) {
    const down = (Math.PI * ring) / rings;
    const row = [];
    for (let step = 0; step <= sides; step += 1) {
      const around = (2 * Math.PI * step) / sides;
      const normal = [
        Math.sin(down) * Math.cos(around),
        Math.sin(down) * Math.sin(around),
        Math.cos(down),
      ];
      const tangent = [-Math.sin(around), Math.cos(around), 0];
      const position = [radius * normal[0], radius * normal[1], radius + radius * normal[2]];
      row.push(
        builder.vertex(position, [step / sides, ring / rings], tangent, cross(normal, tangent)),
      );
    }
    grid.push(row);
  }
  for (let ring = 0; ring < rings; ring += 1) {
    const high = grid[ring];
    const low = grid[ring + 1];
    for (let step = 0; step < sides; step += 1) {
      if (ring > 0) {
        builder.triangle(low[step], high[step + 1], high[step]);
      }
      if (ring < rings - 1) {
        builder.triangle(low[step], low[step + 1], high[step + 1]);
      }
    }
  }
  return builder.mesh;
}

function plane(width, depth, material) {
  const builder = new Builder(material);
  const corners = [];
  for (const [su, sv] of [
    [-1, -1],
    [1, -1],
    [1, 1],
    [-1, 1],
  ]) {
    const point = [(su * width) / 2, (sv * depth) / 2, 0];
    corners.push(builder.vertex(point, [(su + 1) / 2, 1 - (sv + 1) / 2], [1, 0, 0], [0, 1, 0]));
  }
  builder.quad(...corners);
  return builder.mesh;
}

// ---- The options (studio_shapes.py make) ----

function length(options, key, first) {
  const value = options[key] ?? first;
  if (typeof value !== 'number' || !(value >= SMALLEST && value <= LARGEST)) {
    throw new Error(`${key} is from ${SMALLEST} to ${LARGEST} metres`);
  }
  return value;
}

function count(options, key, first, [low, high]) {
  const value = options[key] ?? first;
  if (!Number.isInteger(value) || value < low || value > high) {
    throw new Error(`${key} is a whole number from ${low} to ${high}`);
  }
  return value;
}

function size(options, parts, first) {
  const given = options.size ?? Array(parts).fill(first);
  if (!Array.isArray(given) || given.length !== parts) {
    throw new Error(`size is ${parts} lengths in metres`);
  }
  return given.map((part) => length({ part }, 'part', first));
}

// The shape's mesh and its material's text for <name> (options as studio_shapes.py takes them);
// throws what is wrong.
function make(name, options) {
  if (!/^[A-Za-z0-9_-]{1,48}$/.test(name)) {
    throw new Error("the name is letters, digits, '_' and '-', at most 48");
  }
  const known = new Set([
    'shape',
    'size',
    'radius',
    'height',
    'sides',
    'rings',
    'colour',
    'surface',
  ]);
  const unknown = Object.keys(options)
    .filter((key) => !known.has(key))
    .sort();
  if (!SHAPES.includes(options.shape)) {
    throw new Error(`shape is one of ${SHAPES.join(', ')}`);
  }
  if (unknown.length > 0) {
    throw new Error(`unknown ${unknown.join(', ')}`);
  }
  let mesh;
  if (options.shape === 'box') {
    mesh = box(...size(options, 3, 0.5), name);
  } else if (options.shape === 'cylinder') {
    mesh = cylinder(
      length(options, 'radius', 0.25),
      length(options, 'height', 0.5),
      count(options, 'sides', 24, SIDES),
      name,
    );
  } else if (options.shape === 'sphere') {
    const radius = length(options, 'radius', 0.25);
    mesh = sphere(
      radius,
      count(options, 'rings', 12, RINGS),
      count(options, 'sides', 24, SIDES),
      name,
    );
  } else {
    mesh = plane(...size(options, 2, 1), name);
  }
  const colour = options.colour ?? [0.8, 0.8, 0.8];
  const partsOk =
    Array.isArray(colour) &&
    colour.every((part) => typeof part === 'number' && part >= 0 && part <= 1);
  if (!partsOk || colour.length !== 3) {
    throw new Error('colour is [r, g, b], each from 0 to 1');
  }
  const surface = options.surface ?? 'mat_wood';
  if (!SURFACES.includes(surface)) {
    throw new Error(`surface is one of ${SURFACES.join(', ')}`);
  }
  if (mesh.positions.length > MAX_VERTICES) {
    throw new Error(`the mesh has ${mesh.positions.length} vertices, at most ${MAX_VERTICES}`);
  }
  return { mesh, material: materialText(name, colour, surface) };
}

// ---- The .mtl (kcds_cgf.write_mtl, one material without a texture) ----

function materialText(name, colour, surface) {
  const [r, g, b] = colour.map((part) => part.toFixed(4));
  return (
    [
      '<Material MtlFlags="524544" vertModifType="0">',
      '  <SubMaterials>',
      `    <Material Name="${name}" MtlFlags="524416" Shader="Illum" ` +
        `GenMask="20000000000000" StringGenMask="" SurfaceType="${surface}" ` +
        `Diffuse="${r},${g},${b}" Specular="0.04,0.04,0.04" Opacity="1" ` +
        'Shininess="10" vertModifType="0" LayerAct="1">',
      '      <Textures />',
      '    </Material>',
      '  </SubMaterials>',
      '</Material>',
    ].join('\n') + '\n'
  );
}

// ---- The .cgf (kcds_cgf.write_cgf) ----

// Python's round(): halves to the even neighbour.
function roundHalfEven(value) {
  const floor = Math.floor(value);
  const rest = value - floor;
  if (rest > 0.5) {
    return floor + 1;
  }
  if (rest < 0.5) {
    return floor;
  }
  return floor % 2 === 0 ? floor : floor + 1;
}

// frexp: value = fraction * 2 ** exponent, 0.5 <= |fraction| < 1.
function frexp(value) {
  const bits = new DataView(new ArrayBuffer(8));
  bits.setFloat64(0, value);
  const high = bits.getUint32(0);
  let exponent = (high >>> 20) & 0x7ff;
  if (exponent === 0) {
    // A subnormal double: scaled into the normal range first.
    const [fraction, inner] = frexp(value * 2 ** 64);
    return [fraction, inner - 64];
  }
  exponent -= 1022;
  bits.setUint32(0, (high & 0x800fffff) | (1022 << 20));
  return [bits.getFloat64(0), exponent];
}

// A half-precision float's 16 bits as Python's struct 'e' packs a double (PyFloat_Pack2).
function halfBits(value) {
  if (!Number.isFinite(value)) {
    throw new Error('a position or uv is not a finite number');
  }
  const sign = value < 0 || Object.is(value, -0) ? 1 : 0;
  let x = Math.abs(value);
  if (x === 0) {
    return sign << 15;
  }
  let [fraction, exponent] = frexp(x);
  fraction *= 2;
  exponent -= 1;
  if (exponent >= 16) {
    throw new Error('too large for a half-precision float');
  } else if (exponent < -25) {
    fraction = 0;
    exponent = 0;
  } else if (exponent < -14) {
    fraction = fraction * 2 ** (14 + exponent);
    exponent = 0;
  } else {
    exponent += 15;
    fraction -= 1;
  }
  fraction *= 1024;
  let bits = Math.trunc(fraction);
  const rest = fraction - bits;
  if (rest > 0.5 || (rest === 0.5 && bits % 2 === 1)) {
    bits += 1;
    if (bits === 1024) {
      bits = 0;
      exponent += 1;
      if (exponent === 31) {
        throw new Error('too large for a half-precision float');
      }
    }
  }
  x = bits | (exponent << 10) | (sign << 15);
  return x;
}

class Bytes {
  constructor() {
    this.parts = [];
    this.length = 0;
  }

  add(size, write) {
    const view = new DataView(new ArrayBuffer(size));
    write(view);
    this.parts.push(new Uint8Array(view.buffer));
    this.length += size;
  }

  ints(...values) {
    this.add(values.length * 4, (view) =>
      values.forEach((value, at) => view.setInt32(at * 4, value, true)),
    );
  }

  floats(...values) {
    this.add(values.length * 4, (view) =>
      values.forEach((value, at) => view.setFloat32(at * 4, value, true)),
    );
  }

  raw(bytes) {
    this.parts.push(Uint8Array.from(bytes));
    this.length += bytes.length;
  }

  pad4() {
    const missing = (4 - (this.length % 4)) % 4;
    if (missing > 0) {
      this.raw(new Array(missing).fill(0));
    }
  }

  bytes() {
    const out = new Uint8Array(this.length);
    let at = 0;
    for (const part of this.parts) {
      out.set(part, at);
      at += part.length;
    }
    return out;
  }
}

function ascii(text, size) {
  const out = new Array(size).fill(0);
  for (let at = 0; at < Math.min(text.length, size - 1); at += 1) {
    out[at] = text.charCodeAt(at) & 0x7f;
  }
  return out;
}

function bounds(points) {
  const low = [0, 1, 2].map((axis) => Math.min(...points.map((point) => point[axis])));
  const high = [0, 1, 2].map((axis) => Math.max(...points.map((point) => point[axis])));
  return [low, high];
}

// The texel density (space per uv area) and the mean face area (geometric), as kcds_cgf.
function densities(mesh) {
  let space = 0;
  let flat = 0;
  let logs = 0;
  let logCount = 0;
  for (let at = 0; at < mesh.indices.length; at += 3) {
    const [a, b, c] = [mesh.indices[at], mesh.indices[at + 1], mesh.indices[at + 2]];
    const [pa, pb, pc] = [mesh.positions[a], mesh.positions[b], mesh.positions[c]];
    const u = [0, 1, 2].map((k) => pb[k] - pa[k]);
    const v = [0, 1, 2].map((k) => pc[k] - pa[k]);
    const across = cross(u, v);
    const area = 0.5 * Math.sqrt(across.reduce((sum, part) => sum + part * part, 0));
    const [ta, tb, tc] = [mesh.uvs[a], mesh.uvs[b], mesh.uvs[c]];
    const uvArea =
      0.5 * Math.abs((tb[0] - ta[0]) * (tc[1] - ta[1]) - (tc[0] - ta[0]) * (tb[1] - ta[1]));
    space += area;
    flat += uvArea;
    if (area > 0) {
      logs += Math.log(area);
      logCount += 1;
    }
  }
  const density = flat > 0 ? Math.sqrt(space / flat) : 1;
  const mean = logCount > 0 ? Math.exp(logs / logCount) : 0;
  return [density, mean];
}

function unit(value) {
  return Math.max(-32767, Math.min(32767, roundHalfEven(value * 32767)));
}

// The .cgf's bytes of `mesh` (one subset, one material) with its material at
// `materialFile` (objects/kcds_studio/<name>), its node named `nodeName`.
function writeCgf(mesh, materialFile, nodeName) {
  const [density, mean] = densities(mesh);
  const chunks = [];
  const add = (type, version, body) => {
    const size = body.length;
    body.pad4();
    chunks.push({ type, version, id: chunks.length + 1, body: body.bytes(), size });
    return chunks.length;
  };
  // MtlName 0x1014 v0x802.
  const names = new Bytes();
  names.raw(ascii(materialFile, 128));
  names.ints(1, PHYSICALIZE_DEFAULT);
  names.raw([...ascii(mesh.material, mesh.material.length + 1)]);
  const materialId = add(0x1014, 0x802, names);
  // MeshSubsets 0x1017 v0x800: one subset, its density.
  const subsets = new Bytes();
  subsets.ints(4, 1, 0, 0);
  const [low, high] = bounds(mesh.positions);
  const centre = [0, 1, 2].map((k) => (low[k] + high[k]) / 2);
  const radius = Math.sqrt([0, 1, 2].reduce((sum, k) => sum + (high[k] - centre[k]) ** 2, 0));
  subsets.ints(0, mesh.indices.length, 0, mesh.positions.length, 0);
  subsets.floats(radius, ...centre);
  subsets.floats(density);
  const subsetsId = add(0x1017, 0x800, subsets);
  // DataStream 0x1016 v0x800: P3S_C4B_T2S (15), indices (5), tangents (6).
  const vertices = new Bytes();
  vertices.ints(0, 15, mesh.positions.length, 16, 0, 0);
  mesh.positions.forEach(([x, y, z], at) => {
    const [u, v] = mesh.uvs[at];
    vertices.add(16, (view) => {
      [x, y, z, 1].forEach((part, index) => view.setUint16(index * 2, halfBits(part), true));
      [255, 255, 255, 255].forEach((part, index) => view.setUint8(8 + index, part));
      view.setUint16(12, halfBits(u), true);
      view.setUint16(14, halfBits(v), true);
    });
  });
  const verticesId = add(0x1016, 0x800, vertices);
  const indices = new Bytes();
  indices.ints(0, 5, mesh.indices.length, 2, 0, 0);
  indices.add(mesh.indices.length * 2, (view) =>
    mesh.indices.forEach((index, at) => view.setUint16(at * 2, index, true)),
  );
  const indicesId = add(0x1016, 0x800, indices);
  const tangents = new Bytes();
  tangents.ints(0, 6, mesh.tangents.length, 16, 0, 0);
  for (const [tangent, bitangent] of mesh.tangents) {
    tangents.add(16, (view) => {
      [...tangent.map(unit), 32767, ...bitangent.map(unit), 32767].forEach((part, index) =>
        view.setInt16(index * 2, part, true),
      );
    });
  }
  const tangentsId = add(0x1016, 0x800, tangents);
  // Mesh 0x1000 v0x801.
  const streams = new Array(16).fill(0);
  streams[5] = indicesId;
  streams[6] = tangentsId;
  streams[15] = verticesId;
  const meshBody = new Bytes();
  meshBody.ints(10, 0, mesh.positions.length, mesh.indices.length, 1, subsetsId, 0);
  meshBody.ints(...streams);
  meshBody.ints(0, 0, 0, 0);
  meshBody.floats(...low, ...high);
  meshBody.floats(density, mean);
  meshBody.raw(new Array(31 * 4).fill(0));
  const meshId = add(0x1000, 0x801, meshBody);
  // Node 0x100B v0x824.
  const node = new Bytes();
  node.raw(ascii(nodeName, 64));
  node.ints(meshId, -1, 0, materialId);
  node.raw([0, 0, 0, 0]);
  node.floats(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0);
  node.raw(new Array(12 + 16 + 12).fill(0));
  node.ints(-1, -1, -1, 0);
  add(0x100b, 0x824, node);
  // The header, the chunk table, the chunks.
  const file = new Bytes();
  file.raw([0x43, 0x72, 0x43, 0x68]);
  const tableOffset = 16;
  let dataOffset = tableOffset + 16 * chunks.length;
  file.ints(0x746, chunks.length, tableOffset);
  for (const chunk of chunks) {
    file.add(16, (view) => {
      view.setUint16(0, chunk.type, true);
      view.setUint16(2, chunk.version, true);
      view.setUint32(4, chunk.id, true);
      view.setUint32(8, chunk.size, true);
      view.setUint32(12, dataOffset, true);
    });
    dataOffset += chunk.body.length;
  }
  for (const chunk of chunks) {
    file.raw(chunk.body);
  }
  return file.bytes();
}

function base64(bytes) {
  const letters = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  let out = '';
  for (let at = 0; at < bytes.length; at += 3) {
    const [a, b, c] = [bytes[at], bytes[at + 1] ?? 0, bytes[at + 2] ?? 0];
    const word = (a << 16) | (b << 8) | c;
    out += letters[(word >> 18) & 63] + letters[(word >> 12) & 63];
    out += at + 1 < bytes.length ? letters[(word >> 6) & 63] : '=';
    out += at + 2 < bytes.length ? letters[word & 63] : '=';
  }
  return out;
}

// The studioMeshWrite message of a shape (the game client writes its files), and what it is:
// { message, vertices, triangles }; throws what is wrong with the options.
function shapeMessage(name, options) {
  const { mesh, material } = make(name, options);
  const model = writeCgf(mesh, `objects/kcds_studio/${name}`, name);
  return {
    message: { op: 'studioMeshWrite', name, model: base64(model), material },
    vertices: mesh.positions.length,
    triangles: mesh.indices.length / 3,
    model,
  };
}

module.exports = {
  SHAPES,
  SURFACES,
  make,
  writeCgf,
  halfBits,
  roundHalfEven,
  base64,
  shapeMessage,
};
