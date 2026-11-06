'use strict';

// A Wavefront OBJ model (Blender's File > Export > Wavefront) and its .mtl materials, read into
// triangles in the game's frame (metres, Z up): the importer's input (import_model.cjs).

const fs = require('node:fs');
const path = require('node:path');

// One OBJ corner: "v", "v/vt", "v//vn" or "v/vt/vn" (1-based, negative counts from the end).
function corner(text, counts) {
  const [v, vt, vn] = text.split('/');
  const index = (value, count) => {
    if (!value) {
      return -1;
    }
    const number = Number.parseInt(value, 10);
    return number < 0 ? count + number : number - 1;
  };
  return {
    v: index(v, counts.v),
    vt: index(vt, counts.vt),
    vn: index(vn, counts.vn),
  };
}

// The materials of a .mtl file: name -> { colour: [r, g, b] 0..1, texture: full path or null }.
function readMaterials(file) {
  const materials = new Map();
  if (!fs.existsSync(file)) {
    return materials;
  }
  let current = null;
  for (const raw of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const line = raw.trim();
    const [keyword, ...rest] = line.split(/\s+/);
    if (keyword === 'newmtl') {
      current = { colour: [0.8, 0.8, 0.8], texture: null };
      materials.set(rest.join(' '), current);
    } else if (current && keyword === 'Kd') {
      current.colour = rest.slice(0, 3).map(Number);
    } else if (current && keyword === 'map_Kd') {
      // The picture's path is the rest of the line (it may hold spaces), after options such as
      // -s 1 1 1 or -bm 1.
      const name = line
        .slice('map_Kd'.length)
        .replace(/\s-(s|o|t)\s+\S+\s+\S+\s+\S+/g, ' ')
        .replace(/\s-mm\s+\S+\s+\S+/g, ' ')
        .replace(/\s-(bm|blendu|blendv|boost|clamp|imfchan|texres|cc)\s+\S+/g, ' ')
        .trim();
      current.texture = path.resolve(path.dirname(file), name);
    }
  }
  return materials;
}

// The model: { objects: [{ name, material, triangles: [[a, b, c]] }], materials } where each
// corner is { p: [x, y, z], uv: [u, v], n: [x, y, z] | null }. `up` is the axis the file has up:
// 'y' (Blender's OBJ default: x right, y up, -z forward) or 'z'; `scale` multiplies.
function readObj(file, { up = 'y', scale = 1 } = {}) {
  const positions = [];
  const uvs = [];
  const normals = [];
  const objects = [];
  let materials = new Map();
  let object = null;
  let material = null;
  const toGame = (x, y, z) =>
    up === 'y' ? [x * scale, -z * scale, y * scale] : [x * scale, y * scale, z * scale];
  const startObject = (name) => {
    object = { name, material, triangles: [] };
    objects.push(object);
  };
  for (const raw of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) {
      continue;
    }
    const [keyword, ...rest] = line.split(/\s+/);
    if (keyword === 'v') {
      const [x, y, z] = rest.map(Number);
      positions.push(toGame(x, y, z));
    } else if (keyword === 'vt') {
      uvs.push([Number(rest[0]), Number(rest[1] ?? 0)]);
    } else if (keyword === 'vn') {
      const [x, y, z] = rest.map(Number);
      const n = up === 'y' ? [x, -z, y] : [x, y, z];
      normals.push(n);
    } else if (keyword === 'o' || keyword === 'g') {
      startObject(rest.join(' ') || `object_${objects.length}`);
    } else if (keyword === 'usemtl') {
      material = rest.join(' ');
      // A new material within an object goes on as a part of the same name.
      startObject(object ? object.name : 'object');
    } else if (keyword === 'mtllib') {
      materials = readMaterials(path.resolve(path.dirname(file), rest.join(' ')));
    } else if (keyword === 'f') {
      if (!object) {
        startObject('object');
      }
      const counts = { v: positions.length, vt: uvs.length, vn: normals.length };
      const corners = rest
        .map((text) => corner(text, counts))
        .map((c) => ({
          p: positions[c.v],
          uv: c.vt >= 0 ? uvs[c.vt] : [0, 0],
          n: c.vn >= 0 ? normals[c.vn] : null,
        }));
      if (corners.some((c) => !c.p)) {
        throw new Error(`a face points at a vertex the file does not have: "${line}"`);
      }
      // A polygon as a fan of triangles.
      for (let k = 1; k + 1 < corners.length; k += 1) {
        object.triangles.push([corners[0], corners[k], corners[k + 1]]);
      }
    }
  }
  return { objects: objects.filter((item) => item.triangles.length > 0), materials };
}

module.exports = { readObj, readMaterials };
