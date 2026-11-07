'use strict';

// A building of the game opened up, without files of its own: the MCP server's open_model
// (open_model.cjs, on the author's PC with Node) and World Architect's own "Open it" and
// open_building (Client/resources/architect/opening.cjs, in the game) both run this, each with
// its own way to read the game's files and write the author's (`io` below).
//
// The game closes the inside of a building it does not want walked into with physics boxes that
// fill it: stjames_church.cgf has 133 box proxies ($physics_proxy_* nodes, each a mesh whose
// physics chunk, 0x1018, is a serialized box), and a few of them (proxy_stjames_1, about
// 30 x 26 x 24 m, and the chancel's and the upper floors') fill the nave, so a camera or a player
// inside stands in solid ground. Its walls, pillars and buttresses are boxes of their own, a
// metre or two thick.
//
// The boxes that fill (none of their three half sizes thinner than `thinnest`) are moved far
// under the ground and made a centimetre small, so they collide with nothing; every other box
// stays as it was. The drawn meshes the game streams from the .cgfm beside the model become
// models of their own, and the boxes kept, the walls and floors asked for, and a floor under each
// removed box (the drawn floor has no collision of its own: the box was it) go into a model drawn
// by nothing. The answer names each removed box in the model's frame (its corners' box, the
// rotation of its basis applied).

const { Bytes } = require('./bytes.cjs');
const { writeCgf } = require('./cgf_writer.cjs');

const CHUNK_NODE = 0x100b;
const CHUNK_PHYSICS = 0x1018;
const CHUNK_MATERIAL = 0x1014;
const CHUNK_MESH = 0x1000;
const CHUNK_EXPORT = 0x1015;
const NODE_NAME_SIZE = 64;
const NODE_OBJECT = 64;
const NODE_PARENT = 68;
const NODE_CHILDREN = 72;
const MESH_SUBSETS = 20;
const MESH_STREAMS = 28;
const MESH_PHYSICS = 92;
const MATERIAL_NAME_SIZE = 128;
// The serialized box after its chunk's six ints: type, 0, inertia (3), rotation (4), centre of
// mass (3), volume, 1, 4 x 0, basis (3 x 3), 0, centre (3), half size (3).
const BOX_HEADER = 24;
const BOX_BASIS = 4 * (2 + 3 + 4 + 3 + 1 + 1 + 4);
const BOX_CENTRE = BOX_BASIS + 4 * (9 + 1);
const BOX_HALF = BOX_CENTRE + 12;
const BOX_TYPE = 1;
// Where a removed box goes, and its size there.
const GONE_DEPTH = -2000;
const GONE_HALF = 0.01;
// The largest centre or half size (metres) a building's box is read with (a removed box lies at
// GONE_DEPTH).
const BOX_MOST = 10000;
// A floor put under a removed box: this thick, its top this far over the box's bottom (the drawn
// floor lies on the box's bottom or a little over it).
const FLOOR_THICKNESS = 0.5;
const FLOOR_RAISE = 0.02;
// A box reaching this far over the model's origin (and under it) stood for its ground floor.
const FLOOR_OVER_ORIGIN = 2;
// A drawn floor's texture repeats every this many metres.
const FLOOR_TILE = 2;
// A ceiling hangs this far under its box's top; boxes overlap when they share more than this.
const CEILING_UNDER_TOP = 0.05;
const OVERLAP_LEAST = 0.1;
// A floor or ceiling this far or more inside another removed box hangs in that box's room.
const STOREY_LEAST = 1.5;
// A floor this high over the building's origin (metres) or lower is on its ground storey.
const GROUND_STOREY = 1.5;
// A ceiling goes only where an inside ends this high over the origin or lower: a house's rooms
// under its roof. A church's or a tower's boxes reach far higher, to no roof a ceiling belongs
// under (St James has none: a ceiling there floated over its walls).
const CEILING_MOST = 12;
// The main room's ceiling goes up to this high (metres over the origin; a church's nave), not a
// tower's top.
const ROOF_MOST = 40;
// An opened building's name: what its files are called.
const STEM = /^[a-z0-9_]{1,48}$/;
const OPENED_FOLDER = 'objects/kcds/opened';

// The chunks of a CGF: [{ kind, version, id, size, offset }], by id too.
function chunksOf(bytes) {
  if (bytes.toString('latin1', 0, 4) !== 'CrCh') {
    throw new Error('not a CGF file');
  }
  const count = bytes.readUInt32LE(8);
  const table = bytes.readUInt32LE(12);
  const list = [];
  for (let index = 0; index < count; index += 1) {
    const at = table + index * 16;
    list.push({
      kind: bytes.readUInt16LE(at),
      version: bytes.readUInt16LE(at + 2),
      id: bytes.readUInt32LE(at + 4),
      size: bytes.readUInt32LE(at + 8),
      offset: bytes.readUInt32LE(at + 12),
    });
  }
  return { list, byId: new Map(list.map((chunk) => [chunk.id, chunk])) };
}

// The model's material path (its material chunk's name, without .mtl).
function materialOf(bytes) {
  const chunk = chunksOf(bytes).list.find((item) => item.kind === CHUNK_MATERIAL);
  if (!chunk) {
    return null;
  }
  return bytes
    .subarray(chunk.offset, chunk.offset + MATERIAL_NAME_SIZE)
    .toString('latin1')
    .split('\0')[0];
}

// Points the model at another material (its material chunk's name).
function renameMaterial(bytes, material) {
  const chunk = chunksOf(bytes).list.find((item) => item.kind === CHUNK_MATERIAL);
  if (!chunk || material.length >= MATERIAL_NAME_SIZE) {
    throw new Error('the model has no material chunk to point at its own material');
  }
  bytes.fill(0, chunk.offset, chunk.offset + MATERIAL_NAME_SIZE);
  bytes.write(material, chunk.offset, 'latin1');
}

// A sub-material's whole <Material ...>...</Material> in a .mtl, by its name.
function subMaterial(text, name) {
  const safe = String(name).replace(/[^\w$]/g, '');
  // Written closed in itself (<Material Name="void" .../>, as the game writes one without
  // textures) or with its textures inside.
  const closed = new RegExp(`<Material [^>]*Name="${safe}"[^>]*/>`);
  const open = new RegExp(`<Material [^>]*Name="${safe}"[^>]*[^/]>[\\s\\S]*?</Material>`);
  const found = text.match(closed) ?? text.match(open);
  return found ? found[0] : null;
}

// The material text with each sub-material named in `swaps` ({ void: 'plaster_b' }) drawn as
// another one of the same material: the other's whole definition under the first's name.
function swapSubMaterials(text, swaps) {
  let out = text;
  for (const [target, look] of Object.entries(swaps)) {
    const old = subMaterial(out, target);
    const model = subMaterial(text, look);
    if (!old || !model) {
      throw new Error(`the material has no sub-material ${old ? look : target}`);
    }
    out = out.replace(old, model.replace(/Name="[^"]*"/, `Name="${target}"`));
  }
  return out;
}

// What an opened copy's inside ("void", which the game never shows and draws black without
// textures) is drawn as by default: the material's own plaster, else its stone, as swaps
// ({ void: 'plaster_b' }); none when it has no void or neither.
function insideLookOf(text) {
  if (!subMaterial(text, 'void')) {
    return {};
  }
  const names = [...String(text).matchAll(/<Material [^>]*Name="([^"]*)"/g)].map((m) => m[1]);
  const usable = (name) => !/decal|proxy|void|terrain|\$lod/i.test(name);
  const look =
    names.find((name) => /plaster/i.test(name) && usable(name)) ??
    names.find((name) => /stone|granite|wall/i.test(name) && usable(name));
  return look ? { void: look } : {};
}

// The sub-materials of a material's text an opened copy draws as nothing: those that only cast a
// building's shadow, a closed mesh around it the game never draws, which a copy would draw black
// around its rooms ("shadowproxy", "fake_shadowproxy"); and the ground dressing laid on the
// ground inside ("terrain", "terrain_decal" in St James), which the copy's own floor replaces
// (with the ground levelled under that floor, it stood out of it).
function shadowProxiesOf(text) {
  const names = [
    ...String(text).matchAll(/<Material [^>]*Name="([^"]*shadowproxy[^"]*|terrain[^"]*)"/gi),
  ];
  return [...new Set(names.map((found) => found[1]))];
}

// The material text with each sub-material named in `names` drawn as nothing (Nodraw).
function hideSubMaterials(text, names) {
  let out = text;
  for (const name of names) {
    const safe = String(name).replace(/[^\w$]/g, '');
    // One tag ending "/>", else an opening tag to its own end (a self-closing one matched as
    // opening ran on to the next </Material> and took the closing tags with it).
    const closed = new RegExp(`<Material [^>]*Name="${safe}"[^>]*/>`);
    const open = new RegExp(`<Material [^>]*Name="${safe}"[^>]*[^/]>[\\s\\S]*?</Material>`);
    const pattern = closed.test(out) ? closed : open;
    out = out.replace(
      pattern,
      `<Material Name="${safe}" MtlFlags="525440" Shader="Nodraw" GenMask="0" ` +
        'StringGenMask="" SurfaceType="" Diffuse="1,1,1" Specular="0,0,0" Opacity="1" ' +
        'Shininess="10" LayerAct="1"><Textures /></Material>',
    );
  }
  return out;
}

// The model's box proxies: { name, at (the physics body's offset), centre, half, extent } with
// the centre and extent (half the size of its box along x, y and z) in the model's frame.
function boxProxies(bytes) {
  const { list, byId } = chunksOf(bytes);
  const boxes = [];
  for (const chunk of list) {
    if (chunk.kind !== CHUNK_NODE) {
      continue;
    }
    const name = bytes
      .subarray(chunk.offset, chunk.offset + NODE_NAME_SIZE)
      .toString('latin1')
      .split('\0')[0];
    if (!name.startsWith('$physics_proxy')) {
      continue;
    }
    const mesh = byId.get(bytes.readInt32LE(chunk.offset + NODE_OBJECT));
    const physics = mesh ? byId.get(bytes.readInt32LE(mesh.offset + MESH_PHYSICS)) : null;
    if (!physics || physics.kind !== CHUNK_PHYSICS) {
      continue;
    }
    const body = physics.offset + BOX_HEADER;
    if (bytes.readInt32LE(body) !== BOX_TYPE) {
      continue;
    }
    // The node's place: the translation in its matrix's last row.
    const matrix = chunk.offset + 84;
    const origin = [0, 1, 2].map((k) => bytes.readFloatLE(matrix + 48 + k * 4));
    const basis = [];
    for (let k = 0; k < 9; k += 1) {
      basis.push(bytes.readFloatLE(body + BOX_BASIS + k * 4));
    }
    const centre = [0, 1, 2].map((k) => origin[k] + bytes.readFloatLE(body + BOX_CENTRE + k * 4));
    const half = [0, 1, 2].map((k) => bytes.readFloatLE(body + BOX_HALF + k * 4));
    // The box's half size along the model's axes. The basis turns the model's frame into the
    // box's own, so the box's own axes are its rows: axis k of the box adds |basis[k][i]| of its
    // half size along the model's axis i (the church's four portal walls, the same box turned,
    // came out of one size only this way).
    const extent = [0, 1, 2].map((axis) =>
      [0, 1, 2].reduce((sum, own) => sum + Math.abs(basis[own * 3 + axis]) * half[own], 0),
    );
    // A proxy laid out otherwise than a plain box reads as nonsense (vlasska_10_house's has half
    // sizes of 1e50): it is no box to remove or keep.
    const sane = (value) => Number.isFinite(value) && Math.abs(value) <= BOX_MOST;
    if (![...centre, ...half, ...extent].every(sane)) {
      continue;
    }
    boxes.push({ name, at: body, centre, half, basis, extent });
  }
  return boxes;
}

function rounded(values) {
  return values.map((value) => Math.round(value * 100) / 100);
}

// A model's bytes with its filling boxes taken out (each half size at least `thinnest` metres,
// or those named in `remove`): { copy, removed, kept, keptShapes }, the boxes as { name, centre,
// size } and the kept ones also as they are ({ centre, half, basis, extent }) for their collision.
function openBytes(bytes, { thinnest = 2.5, remove = null } = {}) {
  const copy = Bytes.from(bytes);
  const boxes = boxProxies(copy);
  const named = Array.isArray(remove) ? new Set(remove.map(String)) : null;
  const removed = [];
  const kept = [];
  const keptShapes = [];
  for (const box of boxes) {
    const fills = named ? named.has(box.name) : Math.min(...box.half) >= thinnest;
    const answer = {
      name: box.name,
      centre: rounded(box.centre),
      size: rounded(box.extent.map((value) => value * 2)),
    };
    if (!fills) {
      kept.push(answer);
      keptShapes.push({
        centre: box.centre,
        half: box.half,
        basis: box.basis,
        extent: box.extent,
      });
      continue;
    }
    copy.writeFloatLE(GONE_DEPTH, box.at + BOX_CENTRE + 8);
    for (let k = 0; k < 3; k += 1) {
      copy.writeFloatLE(GONE_HALF, box.at + BOX_HALF + k * 4);
    }
    removed.push(answer);
  }
  return { copy, removed, kept, keptShapes };
}

// ---- the pieces of a streamed model ----

// A chunk file of chunks ({ kind, version, id, body }): the table after the header, then the bodies.
function chunkFile(chunks) {
  const header = 16;
  const table = chunks.length * 16;
  let offset = header + table;
  const out = Bytes.alloc(offset + chunks.reduce((sum, chunk) => sum + chunk.body.length, 0));
  out.write('CrCh', 0, 'latin1');
  out.writeUInt32LE(0x746, 4);
  out.writeUInt32LE(chunks.length, 8);
  out.writeUInt32LE(header, 12);
  chunks.forEach((chunk, index) => {
    const at = header + index * 16;
    out.writeUInt16LE(chunk.kind, at);
    out.writeUInt16LE(chunk.version, at + 2);
    out.writeUInt32LE(chunk.id, at + 4);
    out.writeUInt32LE(chunk.body.length, at + 8);
    out.writeUInt32LE(offset, at + 12);
    chunk.body.copy(out, offset);
    offset += chunk.body.length;
  });
  return out;
}

// Each drawn node of a streamed model's .cgfm as a model of its own: its mesh with its subsets
// and streams, its material (named `material`), no physics, no children. The game does the same
// split itself for World Architect (Client/runtime/architect/model_split.hpp).
function drawnPieces(bytes, material) {
  const { list, byId } = chunksOf(bytes);
  const body = (chunk) => Bytes.from(bytes.subarray(chunk.offset, chunk.offset + chunk.size));
  const exported = list.find((chunk) => chunk.kind === CHUNK_EXPORT);
  const pieces = [];
  for (const node of list) {
    if (node.kind !== CHUNK_NODE) {
      continue;
    }
    const mesh = byId.get(bytes.readInt32LE(node.offset + NODE_OBJECT));
    if (!mesh || mesh.kind !== CHUNK_MESH) {
      continue;
    }
    const nodeBody = body(node);
    nodeBody.writeInt32LE(-1, NODE_PARENT);
    nodeBody.writeInt32LE(0, NODE_CHILDREN);
    const meshBody = body(mesh);
    meshBody.fill(0, MESH_PHYSICS, MESH_PHYSICS + 16);
    const chunks = [];
    if (exported) {
      chunks.push({ ...exported, body: body(exported) });
    }
    const materialChunk = byId.get(nodeBody.readInt32LE(76));
    if (materialChunk) {
      const materialBody = body(materialChunk);
      materialBody.fill(0, 0, MATERIAL_NAME_SIZE);
      materialBody.write(material, 0, 'latin1');
      chunks.push({ ...materialChunk, body: materialBody });
    }
    const subsets = byId.get(meshBody.readInt32LE(MESH_SUBSETS));
    if (subsets) {
      chunks.push({ ...subsets, body: body(subsets) });
    }
    for (let k = 0; k < 16; k += 1) {
      const stream = byId.get(meshBody.readInt32LE(MESH_STREAMS + k * 4));
      if (stream) {
        chunks.push({ ...stream, body: body(stream) });
      }
    }
    chunks.push({ ...mesh, body: meshBody });
    chunks.push({ ...node, body: nodeBody });
    pieces.push(chunkFile(chunks));
  }
  return pieces;
}

// ---- collision ----

const COLLISION_MTL =
  '<?xml version="1.0" encoding="us-ascii"?>\n' +
  '<Material MtlFlags="524544">\n' +
  '  <SubMaterials>\n' +
  '    <Material Name="proxy" MtlFlags="525440" Shader="Nodraw" GenMask="0" StringGenMask="" ' +
  'SurfaceType="mat_stone" Diffuse="1,1,1" Specular="0,0,0" Opacity="1" Shininess="10" ' +
  'LayerAct="1">\n      <Textures />\n    </Material>\n' +
  '    <Material Name="hidden" MtlFlags="525440" Shader="Nodraw" GenMask="0" StringGenMask="" ' +
  'SurfaceType="" Diffuse="1,1,1" Specular="0,0,0" Opacity="1" Shininess="10" LayerAct="1">\n' +
  '      <Textures />\n    </Material>\n' +
  '  </SubMaterials>\n' +
  '</Material>\n';

// Walls ({ from: [x, y], to: [x, y], base, height, thickness }) along the model's x or y axis.
function wallBoxes(walls) {
  return (Array.isArray(walls) ? walls : []).map((wall) => {
    const [x0, y0] = wall.from;
    const [x1, y1] = wall.to;
    const thick = Number(wall.thickness ?? 0.8) / 2;
    const base = Number(wall.base ?? 0);
    const height = Number(wall.height ?? 10);
    const alongX = Math.abs(x1 - x0) >= Math.abs(y1 - y0);
    return {
      centre: [(x0 + x1) / 2, (y0 + y1) / 2, base + height / 2],
      half: alongX
        ? [Math.abs(x1 - x0) / 2, thick, height / 2]
        : [thick, Math.abs(y1 - y0) / 2, height / 2],
    };
  });
}

// Floors ({ min: [x, y], max: [x, y], top, thickness }): a slab whose top is the floor.
function floorBoxes(floors) {
  return (Array.isArray(floors) ? floors : []).map((floor) => {
    const thick = Number(floor.thickness ?? FLOOR_THICKNESS);
    const top = Number(floor.top ?? 0);
    return {
      centre: [
        (floor.min[0] + floor.max[0]) / 2,
        (floor.min[1] + floor.max[1]) / 2,
        top - thick / 2,
      ],
      half: [
        Math.abs(floor.max[0] - floor.min[0]) / 2,
        Math.abs(floor.max[1] - floor.min[1]) / 2,
        thick / 2,
      ],
    };
  });
}

// The height of the floor a removed box ({ centre, size } in the model's frame) stood for: the
// model's origin, the building's ground floor, when the box reaches down past it into the ground
// (a house's box goes 4 m under its doorstep), else the box's bottom (an upper storey's).
function floorTopOf(box) {
  const bottom = box.centre[2] - box.size[2] / 2;
  const top = box.centre[2] + box.size[2] / 2;
  const floor = bottom < 0 && top > FLOOR_OVER_ORIGIN ? 0 : bottom;
  return floor + FLOOR_RAISE;
}

// Whether height `z` over a removed box's footprint lies inside another removed box: the boxes
// overlap there (the church's chancel boxes), so no floor or ceiling of one hangs in the other.
function insideAnother(box, z, removed) {
  return removed.some((other) => {
    if (other === box) {
      return false;
    }
    const overlaps = [0, 1].every(
      (k) =>
        Math.abs(other.centre[k] - box.centre[k]) <
        (other.size[k] + box.size[k]) / 2 - OVERLAP_LEAST,
    );
    // Well inside it: boxes side by side on one storey share their bottom and top.
    const bottom = other.centre[2] - other.size[2] / 2;
    const top = other.centre[2] + other.size[2] / 2;
    return overlaps && z > bottom + STOREY_LEAST && z < top - STOREY_LEAST;
  });
}

function footprintOf(box) {
  return {
    min: [box.centre[0] - box.size[0] / 2, box.centre[1] - box.size[1] / 2],
    max: [box.centre[0] + box.size[0] / 2, box.centre[1] + box.size[1] / 2],
  };
}

// A floor for each removed box, as floorBoxes takes (none inside another removed box).
function floorsUnder(removed) {
  return removed
    .map((box) => ({ ...footprintOf(box), top: floorTopOf(box), box }))
    .filter((floor) => !insideAnother(floor.box, floor.top, removed))
    .map(({ min, max, top }) => ({ min, max, top }));
}

// The removed boxes that are rooms of its ground storey: their floor on it, in no other box.
function groundRooms(removed) {
  return removed.filter((box) => {
    const floor = floorTopOf(box);
    return floor <= GROUND_STOREY && !insideAnother(box, floor, removed);
  });
}

// The floors that lie on the building's ground storey (at most GROUND_STOREY over its origin):
// the ground the building hid is levelled under these only, never raised to an upper storey's.
function groundFloors(removed) {
  return floorsUnder(removed).filter((floor) => floor.top <= GROUND_STOREY);
}

// A ceiling at each removed box's top, where the inside it filled ends (a closed building's roof
// is drawn two-sided, black from below): { min, max, top } (none inside another removed box, none
// over CEILING_MOST).
function ceilingsOver(removed) {
  const ceilingOf = (box) => ({
    ...footprintOf(box),
    top: box.centre[2] + box.size[2] / 2 - CEILING_UNDER_TOP,
    box,
  });
  const found = removed
    .map(ceilingOf)
    .filter((ceiling) => ceiling.top <= CEILING_MOST)
    .filter((ceiling) => !insideAnother(ceiling.box, ceiling.top, removed));
  // Its main room is covered at its top whatever its height: St James's nave stands open to the
  // sky (a building site), and a closed church's vault is drawn black from inside.
  const main = mainRoom(removed);
  if (main && !found.some((ceiling) => ceiling.box === main)) {
    const ceiling = ceilingOf(main);
    if (ceiling.top <= ROOF_MOST) {
      found.push(ceiling);
    }
  }
  return found.map(({ min, max, top }) => ({ min, max, top }));
}

// A building's main room: the largest (by its floor) of the rooms of its ground storey, or of all
// its removed boxes when none is; null when none was removed. Its room light hangs there.
function mainRoom(removed) {
  const rooms = groundRooms(removed);
  const list = rooms.length > 0 ? rooms : removed;
  if (list.length === 0) {
    return null;
  }
  const area = (box) => box.size[0] * box.size[1];
  return list.reduce((best, box) => (area(box) > area(best) ? box : best));
}

// The floors drawn: a plank (or any) material over each floor's top, its texture repeated every
// FLOOR_TILE metres, seen from above and below.
// `surfaces`: the floors and ceilings, each { min, max, top }.
function floorModel(material, surfaces) {
  const vertex = (x, y, z) => ({
    p: [x, y, z],
    uv: [x / FLOOR_TILE, y / FLOOR_TILE],
    t: [1, 0, 0],
    b: [0, 1, 0],
    w: 1,
  });
  const pieces = surfaces.map((floor, index) => {
    const [x0, y0] = floor.min;
    const [x1, y1] = floor.max;
    const z = floor.top;
    const vertices = [vertex(x0, y0, z), vertex(x1, y0, z), vertex(x1, y1, z), vertex(x0, y1, z)];
    // Both windings: drawn from above and below (a floor, a ceiling).
    const indices = [0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2];
    const centre = [(x0 + x1) / 2, (y0 + y1) / 2, z];
    return {
      name: `floor_${index}`,
      at: [0, 0, 0],
      vertices,
      indices,
      subsets: [
        {
          firstIndex: 0,
          indices: indices.length,
          firstVertex: 0,
          vertices: vertices.length,
          material: 0,
          radius: Math.hypot(x1 - x0, y1 - y0) / 2,
          centre,
        },
      ],
      min: [x0, y0, z],
      max: [x1, y1, z],
    };
  });
  return writeCgf({ pieces, material, subMaterials: ['floor'], boxes: [] });
}

// A model drawn by nothing whose boxes collide: one hidden triangle and the boxes.
function collisionModel(material, boxes) {
  const vertex = (p) => ({ p, uv: [0, 0], t: [1, 0, 0], b: [0, 1, 0], w: 1 });
  const piece = {
    name: 'collision',
    at: [0, 0, 0],
    vertices: [vertex([0, 0, 0]), vertex([0.01, 0, 0]), vertex([0, 0.01, 0])],
    indices: [0, 1, 2],
    subsets: [
      {
        firstIndex: 0,
        indices: 3,
        firstVertex: 0,
        vertices: 3,
        material: 1,
        radius: 0.01,
        centre: [0, 0, 0],
      },
    ],
    min: [0, 0, 0],
    max: [0.01, 0.01, 0],
  };
  return writeCgf({ pieces: [piece], material, subMaterials: ['proxy', 'hidden'], boxes });
}

// ---- opening ----

function folderOf(file) {
  const at = file.lastIndexOf('/');
  return at < 0 ? '' : file.slice(0, at);
}

function stemOf(file) {
  return file
    .slice(file.lastIndexOf('/') + 1)
    .replace(/\.cgf$/i, '')
    .toLowerCase();
}

// Opens `mesh` (a game model's path) as objects/kcds/opened/<name>.cgf and beside it, through
// `io`:
//   read(path)   the bytes of a game file (the model), or null
//   text(path)   a game file's text (its material), or null
//   write(path, data)   writes a file of the author's (bytes or text) at a game path
//   split(from, base, material)   the streamed meshes of `from` (a .cgfm) written as models of
//                their own at <base>_<n>.cgf drawn with material: their paths, or null when the
//                model has none
// The boxes that fill (each half size at least `thinnest` metres), or those named in `remove`, go;
// with floor (the default), a floor is put under each. Answers the copy's meshes, its material,
// the boxes removed, how many stay and how many collision boxes were added.
function openWith(
  io,
  {
    mesh,
    thinnest = 2.5,
    remove = null,
    name = null,
    list_only: listOnly = false,
    materials = null,
    walls = null,
    floors = null,
    floor = true,
    floor_look: floorLook = null,
  },
) {
  const source = String(mesh ?? '')
    .replace(/\\/g, '/')
    .toLowerCase();
  if (!/\.cgf$/.test(source)) {
    throw new Error('open takes a game model, a .cgf path');
  }
  if (source.startsWith(`${OPENED_FOLDER}/`)) {
    throw new Error('that building is opened already');
  }
  const bytes = io.read(source);
  if (!bytes) {
    throw new Error(`the game has no ${source}`);
  }
  const { copy, removed, kept, keptShapes } = openBytes(bytes, { thinnest, remove });
  if (listOnly) {
    return { from: source, removed, kept: kept.length, boxes: [...removed, ...kept] };
  }
  if (removed.length === 0) {
    throw new Error(
      `nothing fills ${source}: no box of it is ${thinnest} m thick or more (list_only lists ` +
        'its boxes; remove takes them by name). It may have rooms of its own, or be open already',
    );
  }
  const stem = name ? String(name) : stemOf(source);
  if (!STEM.test(stem)) {
    throw new Error('the opened model needs a name of lower-case letters, digits and _');
  }
  const base = `${OPENED_FOLDER}/${stem}`;
  // A material of its own: the game's copied beside it, some sub-materials drawn as others (the
  // inside the game never shows is drawn with "void", black and without textures).
  const own = materialOf(copy);
  let material = own;
  const text = own ? io.text(`${own}.mtl`) : null;
  // Unless told otherwise, its inside is drawn as its own plaster (or stone) when it has one: left
  // "void", an opened church's inner walls were bare grey faces (judged plain, live).
  const swaps =
    materials && typeof materials === 'object' ? materials : text ? insideLookOf(text) : {};
  // Its shadow's closed mesh drawn as nothing: from inside it hid the rooms in black.
  const hidden = text ? shadowProxiesOf(text) : [];
  if (Object.keys(swaps).length > 0 || hidden.length > 0) {
    if (!text) {
      throw new Error(`the game has no material ${own}.mtl`);
    }
    material = base;
    // Its textures named beside the game's material ("./trimsheet_diff.tif") stay there: the
    // copy lives in another folder (its walls drew grey without them, live).
    const home = folderOf(own);
    const placed = text.replace(/File="\.\/([^"]+)"/g, `File="${home}/$1"`);
    io.write(`${material}.mtl`, hideSubMaterials(swapSubMaterials(placed, swaps), hidden));
  }
  // The drawn meshes: each of the .cgfm's nodes a model of its own, drawn, with no physics.
  const pieces = io.split(source.replace(/\.cgf$/, '.cgfm'), base, material ?? '');
  const streamed = Array.isArray(pieces);
  const meshes = streamed ? [...pieces] : [];
  if (!streamed) {
    // A model whose meshes are its own: the copy itself is drawn.
    if (material && material !== own) {
      renameMaterial(copy, material);
    }
    io.write(`${base}.cgf`, copy);
    meshes.push(`${base}.cgf`);
  }
  // Its collision: the boxes kept, the walls and floors given (in the model's frame) and a floor
  // under each removed box, in a model drawn by nothing.
  const under = floor === false ? [] : floorsUnder(removed);
  const added = [...wallBoxes(walls), ...floorBoxes(floors), ...floorBoxes(under)];
  const boxes = streamed ? [...keptShapes, ...added] : added;
  if (boxes.length > 0) {
    // Not "collision": the catalogue leaves models of that name out.
    const collision = `${base}_walls`;
    io.write(`${collision}.mtl`, COLLISION_MTL);
    io.write(`${collision}.cgf`, collisionModel(collision, boxes));
    meshes.push(`${collision}.cgf`);
  }
  // The floors put in drawn too, and a ceiling where each box's inside ended, with `floor_look`
  // (a material: planks made in the editor): a closed shell has neither of its own inside, and
  // its roof shows black from below.
  const over = floor === false ? [] : ceilingsOver(removed);
  if (typeof floorLook === 'string' && floorLook !== '' && under.length + over.length > 0) {
    io.write(`${base}_floor.cgf`, floorModel(floorLook, [...under, ...over]));
    meshes.push(`${base}_floor.cgf`);
  }
  return {
    meshes,
    from: source,
    material,
    hidden,
    removed,
    kept: kept.length,
    added: added.length,
    floors: under.length,
    ceilings: floorLook ? over.length : 0,
  };
}

module.exports = {
  openWith,
  openBytes,
  boxProxies,
  drawnPieces,
  floorsUnder,
  groundFloors,
  groundRooms,
  mainRoom,
  ceilingsOver,
  floorTopOf,
  shadowProxiesOf,
  hideSubMaterials,
  insideLookOf,
  swapSubMaterials,
  renameMaterial,
  materialOf,
  OPENED_FOLDER,
};
