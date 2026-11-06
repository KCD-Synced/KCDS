'use strict';

// A KCD2 static model (.cgf, CryEngine chunk file 0x746) written from triangles: the importer's
// output (import_model.cjs). The layout is the game's own, read from its models (crate_box_a.cgf
// and .cgfm, Dev/tools/props/build_collision.py) and checked live (a cube written
// here drew with a material of its own and its box stopped a ray):
//
//   0x1015 v1     export flags            0x1014 v0x802  material name (the .mtl without .mtl)
//   0x1016 v0x800 a data stream: flags, type, count, element size, 2 reserved, then the data.
//                 Types: 5 indices (u16), 6 tangents (2 x 4 int16), 15 P3S_C4B_T2S (16 bytes:
//                 half x, y, z, 1; colour; half u, v)
//   0x1017 v0x800 subsets: flags, count, 2 reserved; each first index, index count, first
//                 vertex, vertex count, material id (the sub-material), radius, centre
//   0x1000 v0x801 a mesh (264 bytes): flags (2 texture density, 8 face area; 1 no data),
//                 flags2, vertices, indices, subsets, subsets chunk, 0, 16 stream chunks by type,
//                 4 physics chunks (+92), box min and max (+108), density, face area
//   0x1018 v0x800 physics: data size, 5 reserved, then a serialized box: type 1, 0, inertia,
//                 rotation (0, 0, 0, 1), centre of mass, volume, 1, 4 x 0, basis (3 x 3), 0,
//                 centre, half size, 1
//   0x100b v0x824 a node: name (64), object (mesh chunk), parent (-1), children, material chunk,
//                 0, matrix (4 x 4, the translation in its last row), position, rotation and
//                 scale (unused, 0), three controllers (-1), properties' length, properties
//                 ("box\r\n" under a $physics_proxy node)
//
// Positions are half floats in metres from the model's origin, as the game's own buildings keep
// them (stjames_church.cgfm: 6 nodes, -30 to 67 m): exact to about 3 cm at 64 m. The game takes
// no node offset, so every node stands at the origin.

const VERTEX_SIZE = 16;
const MOST_VERTICES = 65535;
const STREAM_INDICES = 5;
const STREAM_TANGENTS = 6;
const STREAM_PACKED = 15;
const MESH_DRAWN = 2 | 8;
const MESH_EMPTY = 1;
const PROXY_FLAGS2 = 12;
const MESH_CHUNK_SIZE = 264;
const MATERIAL_NAME_SIZE = 128;
// How a sub-material is physicalized (the material chunk's list): by proxies, or not at all.
const PHYS_DEFAULT_PROXY = 0x1100;
const PHYS_NONE = -1;

function half(value) {
  const bits = new Uint32Array(new Float32Array([value]).buffer)[0];
  const sign = (bits >>> 16) & 0x8000;
  const exponent = ((bits >>> 23) & 0xff) - 127 + 15;
  const mantissa = bits & 0x7fffff;
  if (exponent <= 0) {
    return sign;
  }
  if (exponent >= 31) {
    return sign | 0x7bff;
  }
  // Rounded to the nearest.
  const rounded = (exponent << 10) | (mantissa >>> 13);
  return sign | (rounded + ((mantissa >>> 12) & 1));
}

function int32s(values) {
  const buffer = Buffer.alloc(values.length * 4);
  values.forEach((value, index) => buffer.writeInt32LE(value, index * 4));
  return buffer;
}

function float32s(values) {
  const buffer = Buffer.alloc(values.length * 4);
  values.forEach((value, index) => buffer.writeFloatLE(value, index * 4));
  return buffer;
}

function text(value, size) {
  const buffer = Buffer.alloc(size);
  buffer.write(value.slice(0, size - 1), 0, 'latin1');
  return buffer;
}

class ChunkFile {
  constructor() {
    this.chunks = [];
  }

  add(type, version, body) {
    const id = this.chunks.length + 1;
    this.chunks.push({ type, version, id, body });
    return id;
  }

  bytes() {
    const tableAt = 16;
    let at = tableAt + this.chunks.length * 16;
    const table = Buffer.alloc(this.chunks.length * 16);
    const bodies = [];
    this.chunks.forEach((chunk, index) => {
      const pad = (4 - (chunk.body.length % 4)) % 4;
      table.writeUInt16LE(chunk.type, index * 16);
      table.writeUInt16LE(chunk.version, index * 16 + 2);
      table.writeUInt32LE(chunk.id, index * 16 + 4);
      table.writeUInt32LE(chunk.body.length, index * 16 + 8);
      table.writeUInt32LE(at, index * 16 + 12);
      bodies.push(chunk.body, Buffer.alloc(pad));
      at += chunk.body.length + pad;
    });
    const header = Buffer.alloc(16);
    header.write('CrCh', 0, 'latin1');
    header.writeUInt32LE(0x746, 4);
    header.writeUInt32LE(this.chunks.length, 8);
    header.writeUInt32LE(tableAt, 12);
    return Buffer.concat([header, table, ...bodies]);
  }
}

function meshBody({ flags, vertices, indices, subsets, subsetsChunk, streams, physics, min, max }) {
  const body = Buffer.alloc(MESH_CHUNK_SIZE);
  int32s([
    flags,
    flags === MESH_EMPTY ? PROXY_FLAGS2 : 0,
    vertices,
    indices,
    subsets,
    subsetsChunk,
    0,
  ]).copy(body, 0);
  const slots = new Array(16).fill(0);
  for (const [type, chunk] of Object.entries(streams)) {
    slots[Number(type)] = chunk;
  }
  int32s(slots).copy(body, 28);
  int32s(physics).copy(body, 92);
  float32s([...min, ...max, 1, 1]).copy(body, 108);
  return body;
}

function nodeBody({ name, object, parent, children, material, at, properties = '' }) {
  const matrix = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, at[0], at[1], at[2], 1];
  return Buffer.concat([
    text(name, 64),
    int32s([object, parent, children, material, 0]),
    float32s(matrix),
    float32s(new Array(10).fill(0)),
    int32s([-1, -1, -1, properties.length]),
    Buffer.from(properties, 'latin1'),
  ]);
}

// The rows of a box's basis that does not turn it.
const UNTURNED = Object.freeze([1, 0, 0, 0, 1, 0, 0, 0, 1]);

// The serialized physics box (a 0x1018 chunk's body), its centre and half size in its node's frame.
// `basis` (9 numbers, row by row) turns the node's frame into the box's own, as the game keeps it:
// the box's own axes are its rows.
function boxBody(centre, halfSize, basis = UNTURNED) {
  const [hx, hy, hz] = halfSize;
  const volume = 8 * hx * hy * hz;
  const values = [
    ['i', 1],
    ['i', 0],
    ['f', (volume / 3) * (hy * hy + hz * hz)],
    ['f', (volume / 3) * (hx * hx + hz * hz)],
    ['f', (volume / 3) * (hx * hx + hy * hy)],
    ['f', 0],
    ['f', 0],
    ['f', 0],
    ['f', 1],
    ['f', centre[0]],
    ['f', centre[1]],
    ['f', centre[2]],
    ['f', volume],
    ['i', 1],
    ['f', 0],
    ['f', 0],
    ['f', 0],
    ['f', 0],
    ...basis.map((value) => ['f', value]),
    ['i', 0],
    ['f', centre[0]],
    ['f', centre[1]],
    ['f', centre[2]],
    ['f', hx],
    ['f', hy],
    ['f', hz],
    ['i', 1],
  ];
  const body = Buffer.alloc(values.length * 4);
  values.forEach(([kind, value], index) => {
    if (kind === 'i') {
      body.writeInt32LE(value, index * 4);
    } else {
      body.writeFloatLE(value, index * 4);
    }
  });
  return Buffer.concat([int32s([body.length, 0, 0, 0, 0, 0]), body]);
}

// The streams of a piece: packed vertices, tangents, indices.
function pieceStreams(piece) {
  const packed = Buffer.alloc(piece.vertices.length * VERTEX_SIZE);
  const tangents = Buffer.alloc(piece.vertices.length * VERTEX_SIZE);
  const quantize = (value) => Math.max(-32767, Math.min(32767, Math.round(value * 32767)));
  piece.vertices.forEach((vertex, index) => {
    const at = index * VERTEX_SIZE;
    packed.writeUInt16LE(half(vertex.p[0] - piece.at[0]), at);
    packed.writeUInt16LE(half(vertex.p[1] - piece.at[1]), at + 2);
    packed.writeUInt16LE(half(vertex.p[2] - piece.at[2]), at + 4);
    packed.writeUInt16LE(half(1), at + 6);
    packed.writeUInt32LE(0xffffffff, at + 8);
    packed.writeUInt16LE(half(vertex.uv[0]), at + 12);
    packed.writeUInt16LE(half(1 - vertex.uv[1]), at + 14);
    const sign = vertex.w < 0 ? -32767 : 32767;
    const frame = [...vertex.t.map(quantize), sign, ...vertex.b.map(quantize), sign];
    frame.forEach((value, k) => tangents.writeInt16LE(value, at + k * 2));
  });
  const indices = Buffer.alloc(piece.indices.length * 2);
  piece.indices.forEach((value, index) => indices.writeUInt16LE(value, index * 2));
  return { packed, tangents, indices };
}

// The .cgf of pieces ({ name, at, vertices, indices, subsets, min, max }, vertices in model space)
// drawn with `material` (subMaterials: its sub-materials' names, the first the collision boxes'
// proxy material, the rest by the subsets' ids), and boxes ({ centre, half }) that collide;
// answers its bytes.
function writeCgf({ pieces, material, subMaterials, boxes }) {
  const file = new ChunkFile();
  file.add(0x1015, 0x0001, Buffer.concat([int32s([2, 6, 8, 1, 1]), text('RCVer:1.8 ', 144)]));
  // The sub-materials and how each is physicalized: the first (the proxy material, drawn by no
  // one) by the collision boxes, as the game's crate_box.mtl's proxy_mat_wood; none of the
  // others (their meshes have no physics).
  const names = Buffer.from(`${subMaterials.join('\0')}\0`, 'latin1');
  const types = subMaterials.map((_, index) => (index === 0 ? PHYS_DEFAULT_PROXY : PHYS_NONE));
  const materialChunk = file.add(
    0x1014,
    0x802,
    Buffer.concat([
      text(material, MATERIAL_NAME_SIZE),
      int32s([subMaterials.length, ...types]),
      names,
    ]),
  );
  const nodes = [];
  for (const piece of pieces) {
    if (piece.vertices.length > MOST_VERTICES) {
      throw new Error(`${piece.name} has more than ${MOST_VERTICES} vertices`);
    }
    const subsetsBody = Buffer.concat([
      int32s([0, piece.subsets.length, 0, 0]),
      ...piece.subsets.map((subset) =>
        Buffer.concat([
          int32s([
            subset.firstIndex,
            subset.indices,
            subset.firstVertex,
            subset.vertices,
            subset.material,
          ]),
          float32s([subset.radius, ...subset.centre.map((value, k) => value - piece.at[k])]),
        ]),
      ),
    ]);
    const subsetsChunk = file.add(0x1017, 0x800, subsetsBody);
    const streams = pieceStreams(piece);
    const stream = (type, count, size, data) =>
      file.add(0x1016, 0x800, Buffer.concat([int32s([0, type, count, size, 0, 0]), data]));
    const packedChunk = stream(STREAM_PACKED, piece.vertices.length, VERTEX_SIZE, streams.packed);
    const indicesChunk = stream(STREAM_INDICES, piece.indices.length, 2, streams.indices);
    const tangentsChunk = stream(
      STREAM_TANGENTS,
      piece.vertices.length,
      VERTEX_SIZE,
      streams.tangents,
    );
    const local = (point) => point.map((value, k) => value - piece.at[k]);
    const mesh = file.add(
      0x1000,
      0x801,
      meshBody({
        flags: MESH_DRAWN,
        vertices: piece.vertices.length,
        indices: piece.indices.length,
        subsets: piece.subsets.length,
        subsetsChunk,
        streams: {
          [STREAM_INDICES]: indicesChunk,
          [STREAM_TANGENTS]: tangentsChunk,
          [STREAM_PACKED]: packedChunk,
        },
        physics: [0, 0, 0, 0],
        min: local(piece.min),
        max: local(piece.max),
      }),
    );
    nodes.push({ piece, mesh });
  }
  // Each box is a physics proxy of the piece its centre is in (else the nearest), as the game's
  // church gives each of its nodes its own proxies: a model of several pieces whose boxes all
  // hung under the first collided nowhere.
  const distance = (node, point) => {
    let sum = 0;
    for (let k = 0; k < 3; k += 1) {
      const outside = Math.max(node.piece.min[k] - point[k], 0, point[k] - node.piece.max[k]);
      sum += outside * outside;
    }
    return sum;
  };
  const owned = nodes.map(() => []);
  boxes.forEach((box) => {
    let best = 0;
    nodes.forEach((node, index) => {
      if (distance(node, box.centre) < distance(nodes[best], box.centre)) {
        best = index;
      }
    });
    owned[best].push(box);
  });
  let proxyCount = 0;
  nodes.forEach((node, index) => {
    const proxies = owned[index].map((box) => {
      const centre = box.centre.map((value, k) => value - node.piece.at[k]);
      const physics = file.add(0x1018, 0x800, boxBody(centre, box.half, box.basis));
      // The box's size along the node's axes (as large as its half size when it is not turned).
      const extent = box.extent ?? box.half;
      const mesh = file.add(
        0x1000,
        0x801,
        meshBody({
          flags: MESH_EMPTY,
          vertices: 0,
          indices: 0,
          subsets: 0,
          subsetsChunk: 0,
          streams: {},
          physics: [physics, 0, 0, 0],
          min: centre.map((value, k) => value - extent[k]),
          max: centre.map((value, k) => value + extent[k]),
        }),
      );
      proxyCount += 1;
      return { name: `$physics_proxy_box${proxyCount}`, mesh };
    });
    const parent = file.add(
      0x100b,
      0x824,
      nodeBody({
        name: node.piece.name,
        object: node.mesh,
        parent: -1,
        children: proxies.length,
        material: materialChunk,
        at: node.piece.at,
      }),
    );
    for (const proxy of proxies) {
      file.add(
        0x100b,
        0x824,
        nodeBody({
          name: proxy.name,
          object: proxy.mesh,
          parent,
          children: 0,
          material: materialChunk,
          at: [0, 0, 0],
          properties: 'box\r\n',
        }),
      );
    }
  });
  return file.bytes();
}

module.exports = { writeCgf, half, boxBody, MOST_VERTICES };
