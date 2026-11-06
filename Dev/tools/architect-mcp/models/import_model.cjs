'use strict';

// A model from Blender (or any 3D program) as the game's own, made by the MCP server on the
// author's PC (the import_model tool): an OBJ with its .mtl and pictures into the author's assets
// folder (Documents\KCDS\World Architect\assets), at objects/kcds/<folder>/:
//
//   <name>.cgf                 the model (a large one: also <name>_1.cgf, ... its further pieces of
//                              at most PIECE_SIZE metres, placed together at one spot)
//   <name>.mtl                 its materials (one sub-material each of the OBJ's), Illum shader
//   textures/<name>_<n>_diff.dds  each material's picture (or its plain colour)
//
// Its objects by their names (Blender's Outliner names):
//   collision...   not drawn: its box collides (walls, floors a player walks on)
//   door...        not drawn: a place where a game door goes, answered as `doors`
//   anything else  drawn; with `collision: 'objects'` (when the model marks no collision) each
//                  object's box collides too
//
// The model's own origin is the place it stands on when placed; metres, Z up in the game.

const fs = require('node:fs');
const path = require('node:path');

const { readObj } = require('./obj_reader.cjs');
const { writeCgf, MOST_VERTICES } = require('./cgf_writer.cjs');
const { writeTexture, encodeDds, mipLevels, NAME } = require('../textures.cjs');

const PIECE_SIZE = 32;
// The farthest a vertex may be from the model's origin, metres (half floats: about 12 cm apart there).
const FARTHEST = 250;
const COLLISION = /^(collision|col_|ucx_|\$physics)/i;
const DOOR = /^door/i;
const COLLISIONS = Object.freeze(['marked', 'objects', 'bounds', 'none']);
const PLAIN_SIDE = 4;

function boundsOf(triangles) {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (const triangle of triangles) {
    for (const corner of triangle) {
      for (let k = 0; k < 3; k += 1) {
        min[k] = Math.min(min[k], corner.p[k]);
        max[k] = Math.max(max[k], corner.p[k]);
      }
    }
  }
  return { min, max };
}

function boxOf(triangles) {
  const { min, max } = boundsOf(triangles);
  return {
    centre: min.map((value, k) => (value + max[k]) / 2),
    // At least 5 cm thick: a flat plane still stops a body.
    half: min.map((value, k) => Math.max(0.025, (max[k] - value) / 2)),
  };
}

function sub(a, b) {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function cross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

function normalize(v) {
  const length = Math.hypot(v[0], v[1], v[2]) || 1;
  return [v[0] / length, v[1] / length, v[2] / length];
}

// A triangle's tangent (along u) and bitangent (along v), from its corners' places and uvs.
function tangentsOf([a, b, c]) {
  const e1 = sub(b.p, a.p);
  const e2 = sub(c.p, a.p);
  const du1 = b.uv[0] - a.uv[0];
  const dv1 = b.uv[1] - a.uv[1];
  const du2 = c.uv[0] - a.uv[0];
  const dv2 = c.uv[1] - a.uv[1];
  const normal = normalize(cross(e1, e2));
  const det = du1 * dv2 - du2 * dv1;
  if (Math.abs(det) < 1e-12) {
    const any = Math.abs(normal[2]) < 0.9 ? [0, 0, 1] : [1, 0, 0];
    const t = normalize(cross(any, normal));
    return { normal, t, b: cross(normal, t) };
  }
  const r = 1 / det;
  const t = normalize([
    (e1[0] * dv2 - e2[0] * dv1) * r,
    (e1[1] * dv2 - e2[1] * dv1) * r,
    (e1[2] * dv2 - e2[2] * dv1) * r,
  ]);
  const bt = normalize([
    (e2[0] * du1 - e1[0] * du2) * r,
    (e2[1] * du1 - e1[1] * du2) * r,
    (e2[2] * du1 - e1[2] * du2) * r,
  ]);
  return { normal, t, b: bt };
}

// The drawn triangles in pieces, each its own file (the game physicalizes a model of one drawn
// node only: a model of several collided nowhere): one piece when it fits in
// MOST_VERTICES vertices, else by the PIECE_SIZE cell their middle is in, then by material.
function piecesOf(drawn, name) {
  // Cells from the model's own lowest corner.
  const corner = boundsOf(drawn.map(({ triangle }) => triangle)).min;
  const whole = drawn.length * 3 <= MOST_VERTICES;
  const cells = new Map();
  for (const { triangle, material } of drawn) {
    const middle = [0, 1, 2].map(
      (k) => (triangle[0].p[k] + triangle[1].p[k] + triangle[2].p[k]) / 3,
    );
    const key = whole
      ? '0,0,0'
      : middle.map((value, k) => Math.floor((value - corner[k]) / PIECE_SIZE)).join(',');
    if (!cells.has(key)) {
      cells.set(key, []);
    }
    cells.get(key).push({ triangle, material });
  }
  const pieces = [];
  for (const [key, triangles] of [...cells.entries()].sort()) {
    const cell = key.split(',').map(Number);
    // Pieces stand at the model's origin: the game takes no node offset (a piece
    // moved by one drew 16 m off), and its own buildings keep half-float metres to 67 m.
    void cell;
    const at = [0, 0, 0];
    triangles.sort((a, b) => a.material - b.material);
    let piece = null;
    const vertexIndex = new Map();
    const start = () => {
      piece = { name: `${name}_${pieces.length}`, at, vertices: [], indices: [], subsets: [] };
      pieces.push(piece);
      vertexIndex.clear();
    };
    start();
    for (const { triangle, material } of triangles) {
      if (piece.vertices.length + 3 > MOST_VERTICES) {
        start();
      }
      let subset = piece.subsets.at(-1);
      if (!subset || subset.material !== material) {
        subset = {
          material,
          firstIndex: piece.indices.length,
          indices: 0,
          firstVertex: piece.vertices.length,
          vertices: 0,
        };
        piece.subsets.push(subset);
        vertexIndex.clear();
      }
      const frame = tangentsOf(triangle);
      for (const corner of triangle) {
        const normal = corner.n ? normalize(corner.n) : frame.normal;
        const key2 = `${corner.p.join(',')}|${corner.uv.join(',')}|${normal
          .map((v) => v.toFixed(3))
          .join(',')}`;
        let index = vertexIndex.get(key2);
        if (index === undefined) {
          index = piece.vertices.length;
          const handed = Math.sign(
            normal[0] * cross(frame.t, frame.b)[0] +
              normal[1] * cross(frame.t, frame.b)[1] +
              normal[2] * cross(frame.t, frame.b)[2],
          );
          piece.vertices.push({
            p: corner.p,
            uv: corner.uv,
            t: frame.t,
            b: frame.b,
            w: handed || 1,
          });
          vertexIndex.set(key2, index);
        }
        piece.indices.push(index);
      }
      subset.indices += 3;
      subset.vertices = piece.vertices.length - subset.firstVertex;
    }
  }
  for (const piece of pieces) {
    const corners = piece.vertices.map((vertex) => [vertex]);
    const { min, max } = boundsOf(corners);
    piece.min = min;
    piece.max = max;
    for (const subset of piece.subsets) {
      const used = piece.vertices.slice(subset.firstVertex, subset.firstVertex + subset.vertices);
      const box = boundsOf(used.map((vertex) => [vertex]));
      subset.centre = box.min.map((value, k) => (value + box.max[k]) / 2);
      subset.radius = Math.hypot(...box.max.map((value, k) => (value - box.min[k]) / 2));
    }
  }
  return pieces;
}

// The game's material flags: a submaterial of a model (pure child), and drawn on both sides.
const PURE_CHILD_FLAGS = 524416;
const TWO_SIDED_FLAG = 0x2;

function materialsXml(subMaterials, twoSided) {
  const flags = twoSided ? PURE_CHILD_FLAGS | TWO_SIDED_FLAG : PURE_CHILD_FLAGS;
  const proxy =
    '    <Material Name="proxy" MtlFlags="525440" Shader="Nodraw" GenMask="0" StringGenMask="" ' +
    'SurfaceType="mat_stone" Diffuse="1,1,1" Specular="0,0,0" Opacity="1" Shininess="10" ' +
    'LayerAct="1">\n' +
    '      <Textures />\n' +
    '    </Material>\n';
  const parts = subMaterials.map(
    (material) =>
      `    <Material Name="${material.name}" MtlFlags="${flags}" Shader="Illum" GenMask="0" ` +
      `StringGenMask="" SurfaceType="mat_stone" Diffuse="1,1,1" Specular="0.04,0.04,0.04" ` +
      `Opacity="1" Shininess="10" LayerAct="1">\n` +
      '      <Textures>\n' +
      `        <Texture Map="Diffuse" File="${material.texture}" />\n` +
      '      </Textures>\n' +
      '    </Material>\n',
  );
  return (
    '<?xml version="1.0" encoding="us-ascii"?>\n' +
    '<Material MtlFlags="524544">\n' +
    '  <SubMaterials>\n' +
    proxy +
    parts.join('') +
    '  </SubMaterials>\n' +
    '</Material>\n'
  );
}

// A door's place: its box, and the way it faces (its thinnest side, east-west or north-south).
function doorOf(object) {
  const { centre, half } = boxOf(object.triangles);
  const facing = half[0] < half[1] ? 'east-west' : 'north-south';
  return {
    name: object.name,
    at: { x: centre[0], y: centre[1], z: centre[2] - half[2] },
    size: { width: 2 * Math.max(half[0], half[1]), height: 2 * half[2] },
    turn: facing === 'east-west' ? 90 : 0,
  };
}

// Imports a model: args { file, name, folder, up, scale, collision }; answers the mesh to place,
// its size, its doors and what was made.
function importModel(args, assetsFolder) {
  const name = String(args.name ?? '');
  const folder = String(args.folder ?? 'models');
  if (!NAME.test(name) || !NAME.test(folder)) {
    throw new Error('name and folder are 1 to 32 lower-case letters, digits or _');
  }
  const file = String(args.file ?? '');
  if (!file || !fs.existsSync(file) || path.extname(file).toLowerCase() !== '.obj') {
    throw new Error(
      `no OBJ file at ${file || '(no file given)'} (Blender: File > Export > Wavefront)`,
    );
  }
  if (!assetsFolder) {
    throw new Error('the game named no assets folder (open the editor first)');
  }
  const up = args.up === 'z' ? 'z' : 'y';
  const scale = Number(args.scale ?? 1);
  if (!(scale > 0 && scale <= 1000)) {
    throw new Error('scale is above 0 and at most 1000');
  }
  const model = readObj(file, { up, scale });
  const marked = model.objects.filter((object) => COLLISION.test(object.name));
  const doors = model.objects.filter((object) => DOOR.test(object.name));
  const drawnObjects = model.objects.filter(
    (object) => !COLLISION.test(object.name) && !DOOR.test(object.name),
  );
  if (drawnObjects.length === 0) {
    throw new Error('the model has nothing to draw (all its objects are collision or doors)');
  }
  const collision = args.collision ?? (marked.length > 0 ? 'marked' : 'objects');
  if (!COLLISIONS.includes(collision)) {
    throw new Error(`collision is one of ${COLLISIONS.join(', ')}`);
  }
  // Sub-materials in the order the model uses them.
  const materialNames = [...new Set(drawnObjects.map((object) => object.material ?? 'default'))];
  const drawn = [];
  for (const object of drawnObjects) {
    // Sub-material 0 is the collision boxes' proxy material: the drawn ones follow it.
    const material = 1 + materialNames.indexOf(object.material ?? 'default');
    for (const triangle of object.triangles) {
      drawn.push({ triangle, material });
    }
  }
  const reach = boundsOf(drawn.map(({ triangle }) => triangle));
  const farthest = Math.max(...reach.min.map(Math.abs), ...reach.max.map(Math.abs));
  if (farthest > FARTHEST) {
    throw new Error(
      `the model reaches ${Math.round(farthest)} m from its origin (at most ${FARTHEST} m): ` +
        'split it into parts in Blender, each with its own origin',
    );
  }
  const base = `objects/kcds/${folder}`;
  const pieces = piecesOf(drawn, name);
  let boxes = [];
  if (collision === 'marked') {
    boxes = marked.map((object) => boxOf(object.triangles));
  } else if (collision === 'objects') {
    boxes = drawnObjects.map((object) => boxOf(object.triangles));
  } else if (collision === 'bounds') {
    boxes = [boxOf(drawnObjects.flatMap((object) => object.triangles))];
  }
  // The materials' pictures (or their plain colours) as textures.
  const subMaterials = materialNames.map((materialName, index) => {
    const source = model.materials.get(materialName);
    const texture = `${base}/textures/${name}_${index}_diff.dds`;
    const target = path.join(assetsFolder, texture);
    if (source?.texture && fs.existsSync(source.texture)) {
      writeTexture(source.texture, target, Number(args.size ?? 2048));
    } else {
      const colour = (source?.colour ?? [0.8, 0.8, 0.8]).map((value) =>
        Math.round(Math.min(1, Math.max(0, value)) * 255),
      );
      const rgba = Buffer.alloc(PLAIN_SIDE * PLAIN_SIDE * 4);
      for (let at = 0; at < rgba.length; at += 4) {
        rgba[at] = colour[0];
        rgba[at + 1] = colour[1];
        rgba[at + 2] = colour[2];
        rgba[at + 3] = 255;
      }
      fs.mkdirSync(path.dirname(target), { recursive: true });
      fs.writeFileSync(
        target,
        encodeDds(mipLevels({ width: PLAIN_SIDE, height: PLAIN_SIDE, rgba })),
      );
    }
    return { name: materialName.replace(/[^A-Za-z0-9_]/g, '_'), texture };
  });
  const material = `${base}/${name}`;
  fs.writeFileSync(
    path.join(assetsFolder, `${material}.mtl`),
    materialsXml(subMaterials, args.two_sided === true),
  );
  // Each box goes with the piece its centre is in (else the nearest).
  const away = (piece, point) => {
    let sum = 0;
    for (let k = 0; k < 3; k += 1) {
      const outside = Math.max(piece.min[k] - point[k], 0, point[k] - piece.max[k]);
      sum += outside * outside;
    }
    return sum;
  };
  const owned = pieces.map(() => []);
  for (const box of boxes) {
    let best = 0;
    pieces.forEach((piece, index) => {
      if (away(piece, box.centre) < away(pieces[best], box.centre)) {
        best = index;
      }
    });
    owned[best].push(box);
  }
  // One file a piece: <name>.cgf, then <name>_1.cgf, ... placed together at one spot.
  const meshes = pieces.map((piece, index) => {
    const mesh = index === 0 ? `${base}/${name}.cgf` : `${base}/${name}_${index}.cgf`;
    fs.writeFileSync(
      path.join(assetsFolder, mesh),
      writeCgf({
        pieces: [piece],
        material,
        subMaterials: ['proxy', ...subMaterials.map((entry) => entry.name)],
        boxes: owned[index],
      }),
    );
    return mesh;
  });
  const all = boundsOf(drawnObjects.flatMap((object) => object.triangles));
  return {
    mesh: meshes[0],
    meshes,
    material,
    size: { min: all.min, max: all.max },
    pieces: pieces.length,
    triangles: drawn.length,
    materials: subMaterials.length,
    collision: { kind: collision, boxes: boxes.length },
    doors: doors.map(doorOf),
  };
}

module.exports = { importModel, piecesOf, PIECE_SIZE };
