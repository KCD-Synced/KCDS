'use strict';

// World Architect's own materials, made in the editor (editor.cjs): textures drawn by code
// (stone blocks, bricks, plaster, wood planks, floor tiles, cobbles, roof tiles), each with a
// normal map from its relief so light rakes across it, written into the author's assets folder
// as game textures (.dds, mip levels in them) with a material (.mtl) that draws them; and a
// model's material with one of its slots (sub-materials) drawn as another material, so an opened
// building's walls take stone or plaster of the author's choice.
//
// Pure functions of numbers and text (the game client writes the files, architect/
// asset_files.hpp); every texture tiles without a seam.

const KINDS = Object.freeze({
  stone_blocks: { label: 'Stone blocks', color: [0.62, 0.58, 0.52], surface: 'stone', tile: 2 },
  bricks: { label: 'Bricks', color: [0.58, 0.32, 0.22], surface: 'stone', tile: 2 },
  plaster: { label: 'Plaster', color: [0.86, 0.82, 0.74], surface: 'plaster', tile: 2 },
  wood_planks: { label: 'Wood planks', color: [0.45, 0.31, 0.19], surface: 'wood', tile: 2 },
  floor_tiles: { label: 'Floor tiles', color: [0.6, 0.38, 0.27], surface: 'stone', tile: 3 },
  cobbles: { label: 'Cobbles', color: [0.5, 0.48, 0.45], surface: 'stone', tile: 2 },
  roof_tiles: { label: 'Roof tiles', color: [0.55, 0.27, 0.18], surface: 'stone', tile: 2 },
});
// The surface each material sounds and breaks as (the game's surface types).
const SURFACES = Object.freeze({
  stone: 'mat_stone',
  wood: 'mat_wood',
  plaster: 'mat_stone',
  metal: 'mat_metal',
  cloth: 'mat_fabric',
  ground: 'mat_soil',
});
const SIZES = Object.freeze([256, 512, 1024]);
const NAME = /^[a-z0-9_]{1,32}$/;

// ---- numbers ----

// A seeded random number generator (mulberry32): the same seed draws the same texture.
function randomOf(seed) {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const smooth = (t) => t * t * (3 - 2 * t);
const clamp01 = (value) => Math.min(1, Math.max(0, value));
const wrap = (value, period) => ((value % period) + period) % period;

// Value noise on a lattice of `cells` a side, wrapping at its edges (a tile without a seam).
function noiseField(cells, random) {
  const lattice = Float32Array.from({ length: cells * cells }, () => random());
  return (u, v) => {
    const x = wrap(u, 1) * cells;
    const y = wrap(v, 1) * cells;
    const x0 = Math.floor(x);
    const y0 = Math.floor(y);
    const fx = smooth(x - x0);
    const fy = smooth(y - y0);
    const at = (i, j) => lattice[wrap(j, cells) * cells + wrap(i, cells)];
    const top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
    const bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
    return top + (bottom - top) * fy;
  };
}

// Several octaves of value noise (fractal), 0 to 1, tiling.
function fractal(random, octaves, base) {
  const fields = [];
  for (let octave = 0; octave < octaves; octave += 1) {
    fields.push(noiseField(base << octave, random));
  }
  return (u, v) => {
    let sum = 0;
    let weight = 0;
    let amplitude = 1;
    for (const field of fields) {
      sum += field(u, v) * amplitude;
      weight += amplitude;
      amplitude *= 0.5;
    }
    return sum / weight;
  };
}

// ---- patterns: each gives, for a point of the tile (u, v in 0..1), a cell (a block, a plank,
// a stone) with its own random tone, how far inside the cell the point is (0 at a joint) and the
// joint's own darkness ----

// Rows of blocks, each row shifted half a block (stone blocks, bricks, roof tiles).
function running(u, v, across, rows, seed) {
  const row = Math.floor(v * rows);
  const shift = row % 2 === 1 ? 0.5 : 0;
  const x = u * across + shift;
  const column = Math.floor(x);
  const fx = x - column;
  const fy = v * rows - row;
  const id = (wrap(column, across) * 7919 + row * 104729 + seed) >>> 0;
  return { id, edge: Math.min(fx, 1 - fx, fy, 1 - fy), fx, fy };
}

// A grid of squares (floor tiles).
function grid(u, v, cells, seed) {
  const x = u * cells;
  const y = v * cells;
  const column = Math.floor(x);
  const row = Math.floor(y);
  const fx = x - column;
  const fy = y - row;
  return {
    id: (column * 7919 + row * 104729 + seed) >>> 0,
    edge: Math.min(fx, 1 - fx, fy, 1 - fy),
  };
}

// Cells around scattered points, wrapping (cobbles): the nearest point's cell, and how far the
// point is from the border to the next.
function cells(points) {
  return (u, v) => {
    let first = Infinity;
    let second = Infinity;
    let id = 0;
    for (let index = 0; index < points.length; index += 1) {
      const p = points[index];
      let dx = Math.abs(u - p[0]);
      let dy = Math.abs(v - p[1]);
      dx = Math.min(dx, 1 - dx);
      dy = Math.min(dy, 1 - dy);
      const d = Math.hypot(dx, dy);
      if (d < first) {
        second = first;
        first = d;
        id = index;
      } else if (d < second) {
        second = d;
      }
    }
    return { id, edge: (second - first) * 0.5 };
  };
}

const toneOf = (id) => randomOf(id * 2654435761)();

// ---- the textures ----

// A texture of a kind: { size, rgba (Uint8Array, sRGB), height (Float32Array, 0 to 1) }.
// options: { kind, size, color [r, g, b] 0 to 1, seed, variation 0 to 1, wear 0 to 1, joints
// (a joint's width, 0 to 0.2 of a cell), count (cells across) }.
function drawTexture(options) {
  const kind = KINDS[options.kind];
  if (!kind) {
    throw new Error(`kind is one of ${Object.keys(KINDS).join(', ')}`);
  }
  const size = options.size ?? 512;
  if (!SIZES.includes(size)) {
    throw new Error(`size is one of ${SIZES.join(', ')}`);
  }
  const color = options.color ?? kind.color;
  const seed = Number.isInteger(options.seed) ? options.seed : 1;
  const variation = clamp01(options.variation ?? 0.35);
  const wear = clamp01(options.wear ?? 0.4);
  const random = randomOf(seed);
  const grain = fractal(random, 5, 4);
  const fine = fractal(random, 3, 32);
  const blotch = fractal(random, 3, 2);
  // A joint's width in a cell's own size; cobbles' cells are measured across the tile, so theirs
  // is a share of the tile.
  const jointDefaults = { plaster: 0, cobbles: 0.012 };
  const joint = options.joints ?? jointDefaults[options.kind] ?? 0.06;
  const count = options.count;
  let pattern = null;
  if (options.kind === 'stone_blocks') {
    const across = count ?? 4;
    pattern = (u, v) => running(u, v, across, across * 2, seed);
  } else if (options.kind === 'bricks') {
    const across = count ?? 4;
    pattern = (u, v) => running(u, v, across, across * 4, seed);
  } else if (options.kind === 'roof_tiles') {
    const across = count ?? 6;
    pattern = (u, v) => running(u, v, across, across, seed);
  } else if (options.kind === 'floor_tiles') {
    const across = count ?? 4;
    pattern = (u, v) => grid(u, v, across, seed);
  } else if (options.kind === 'cobbles') {
    const points = Array.from({ length: (count ?? 6) ** 2 }, () => [random(), random()]);
    pattern = cells(points);
  } else if (options.kind === 'wood_planks') {
    const across = count ?? 5;
    pattern = (u, v) => {
      const x = u * across;
      const plank = Math.floor(x);
      const fx = x - plank;
      // Each plank's own length joints, at its own height.
      const offset = toneOf(plank + seed);
      const fy = wrap(v + offset, 1);
      // The distance to the plank's end in plank widths, as fx is.
      const end = Math.min(fy, 1 - fy) * across;
      return { id: (plank * 7919 + seed) >>> 0, edge: Math.min(fx, 1 - fx, end), fx, fy };
    };
  }
  const rgba = new Uint8Array(size * size * 4);
  const height = new Float32Array(size * size);
  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) {
      const u = (x + 0.5) / size;
      const v = (y + 0.5) / size;
      const g = grain(u, v);
      const f = fine(u, v);
      let tone = 1 + (blotch(u, v) - 0.5) * variation;
      let relief = 0.5 + (g - 0.5) * 0.4 + (f - 0.5) * 0.15;
      let inJoint = false;
      if (pattern) {
        const cell = pattern(u, v);
        tone *= 1 + (toneOf(cell.id) - 0.5) * variation;
        // Bevelled cell edges, the joint recessed and darker.
        const bevel = smooth(clamp01(cell.edge / Math.max(joint, 0.001) - 0.6));
        inJoint = cell.edge < joint * 0.6;
        relief = inJoint ? 0.08 + f * 0.05 : 0.35 + bevel * 0.4 + (g - 0.5) * 0.3;
        if (options.kind === 'wood_planks' && !inJoint) {
          // The grain runs along the plank.
          const lines = Math.sin((cell.fx * 12 + g * 6 + toneOf(cell.id) * 9) * Math.PI);
          tone *= 0.88 + 0.12 * (0.5 + 0.5 * lines);
          relief += lines * 0.03;
        }
        if (options.kind === 'roof_tiles' && !inJoint) {
          // Each tile rounded across and darker at its lower lip.
          tone *= 0.8 + 0.25 * Math.sin(cell.fx * Math.PI) * (1 - cell.fy * 0.3);
        }
        if (options.kind === 'cobbles' && !inJoint) {
          // Each stone domed from its border to its middle (about 0.04 of the tile in).
          relief = 0.3 + 0.6 * smooth(clamp01(cell.edge * 25)) + (g - 0.5) * 0.2;
        }
      }
      // Wear: dirt in the low places and lighter worn tops.
      const dirt = (1 - relief) * wear * 0.5;
      tone *= 1 - dirt + (f - 0.5) * 0.12;
      const jointShade = inJoint ? 0.55 : 1;
      const at = (y * size + x) * 4;
      rgba[at] = Math.round(clamp01(color[0] * tone * jointShade) * 255);
      rgba[at + 1] = Math.round(clamp01(color[1] * tone * jointShade) * 255);
      rgba[at + 2] = Math.round(clamp01(color[2] * tone * jointShade) * 255);
      rgba[at + 3] = 255;
      height[y * size + x] = clamp01(relief);
    }
  }
  return { size, rgba, height };
}

// A normal map from a relief, wrapping at the edges (DDNA: x and y in red and green, z in blue,
// and how smooth the surface is in alpha, for the game's Bumpmap slot).
function normalMap(height, size, strength = 4, smoothness = 0.25) {
  const out = new Uint8Array(size * size * 4);
  const at = (x, y) => height[wrap(y, size) * size + wrap(x, size)];
  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) {
      const dx = (at(x + 1, y) - at(x - 1, y)) * strength;
      const dy = (at(x, y + 1) - at(x, y - 1)) * strength;
      const length = Math.hypot(dx, dy, 1);
      const i = (y * size + x) * 4;
      out[i] = Math.round(((-dx / length) * 0.5 + 0.5) * 255);
      out[i + 1] = Math.round(((-dy / length) * 0.5 + 0.5) * 255);
      out[i + 2] = Math.round(((1 / length) * 0.5 + 0.5) * 255);
      out[i + 3] = Math.round(smoothness * 255);
    }
  }
  return out;
}

// The mip levels of a square RGBA picture, each half the last (a 2 x 2 box).
function mipLevels(rgba, size) {
  const levels = [{ size, rgba }];
  let last = levels[0];
  while (last.size > 1) {
    const half = last.size >> 1;
    const next = new Uint8Array(half * half * 4);
    for (let y = 0; y < half; y += 1) {
      for (let x = 0; x < half; x += 1) {
        for (let c = 0; c < 4; c += 1) {
          const read = (dx, dy) => last.rgba[((y * 2 + dy) * last.size + x * 2 + dx) * 4 + c];
          next[(y * half + x) * 4 + c] =
            (read(0, 0) + read(1, 0) + read(0, 1) + read(1, 1) + 2) >> 2;
        }
      }
    }
    last = { size: half, rgba: next };
    levels.push(last);
  }
  return levels;
}

// A DDS file of 32-bit BGRA pixels with its mip levels, as the MCP server's texture tool writes
// them (Dev/tools/architect-mcp/textures.cjs): sRGB for a picture, linear for a normal map.
function encodeDds(levels, srgb = true) {
  const headerSize = 4 + 124 + 20;
  const bytes = levels.reduce((sum, level) => sum + level.rgba.length, 0);
  const out = new Uint8Array(headerSize + bytes);
  const view = new DataView(out.buffer);
  const size = levels[0].size;
  view.setUint32(0, 0x20534444, true);
  view.setUint32(4, 124, true);
  view.setUint32(8, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000, true);
  view.setUint32(12, size, true);
  view.setUint32(16, size, true);
  view.setUint32(20, size * 4, true);
  view.setUint32(28, levels.length, true);
  view.setUint32(36, srgb ? 0x800 : 0, true);
  view.setUint32(76, 32, true);
  view.setUint32(80, 0x4, true);
  view.setUint32(84, 0x30315844, true);
  view.setUint32(108, 0x8 | 0x1000 | 0x400000, true);
  out.set([0x46, 0x59, 0x52, 0x43], 124);
  view.setUint32(128, srgb ? 91 : 87, true);
  view.setUint32(132, 3, true);
  view.setUint32(140, 1, true);
  let at = headerSize;
  for (const level of levels) {
    for (let i = 0; i < level.rgba.length; i += 4) {
      out[at] = level.rgba[i + 2];
      out[at + 1] = level.rgba[i + 1];
      out[at + 2] = level.rgba[i];
      out[at + 3] = level.rgba[i + 3];
      at += 4;
    }
  }
  return out;
}

// ---- materials ----

const attribute = (value) => String(value).replace(/[^\w ./,-]/g, '');

// A material drawing a diffuse texture and its normal map (the game's Illum shader), repeated
// `tile` times on a face.
function materialXml({ diffuse, normal, surface = 'stone', tile = 1 }) {
  const repeat =
    tile === 1 ? ' />' : `>\n      <TexMod TileU="${tile}" TileV="${tile}" />\n    </Texture>`;
  return (
    '<?xml version="1.0" encoding="us-ascii"?>\n' +
    '<Material MtlFlags="524288" Shader="Illum" GenMask="0" StringGenMask="%NORMAL_MAP" ' +
    `SurfaceType="${attribute(SURFACES[surface] ?? SURFACES.stone)}" Diffuse="1,1,1" ` +
    'Specular="0.04,0.04,0.04" Opacity="1" Shininess="30" LayerAct="1">\n' +
    '  <Textures>\n' +
    `    <Texture Map="Diffuse" File="${attribute(diffuse)}"${repeat}\n` +
    `    <Texture Map="Bumpmap" File="${attribute(normal)}"${repeat}\n` +
    '  </Textures>\n' +
    '</Material>\n'
  );
}

// The sub-materials (<Material ...>...</Material> or <Material .../>) directly in a material's
// SubMaterials, in order: [{ name, start, end }] (offsets in the text).
function subMaterials(text) {
  const open = text.indexOf('<SubMaterials>');
  const close = text.lastIndexOf('</SubMaterials>');
  if (open < 0 || close < 0) {
    return [];
  }
  const found = [];
  let at = open + '<SubMaterials>'.length;
  while (at < close) {
    const start = text.indexOf('<Material', at);
    if (start < 0 || start >= close) {
      break;
    }
    const tagEnd = text.indexOf('>', start);
    const name = /\sName="([^"]*)"/.exec(text.slice(start, tagEnd + 1))?.[1] ?? '';
    let end = tagEnd + 1;
    if (text[tagEnd - 1] !== '/') {
      // Its matching </Material>, past any nested one.
      let depth = 1;
      let cursor = tagEnd + 1;
      while (depth > 0) {
        const nextOpen = text.indexOf('<Material', cursor);
        const nextClose = text.indexOf('</Material>', cursor);
        if (nextClose < 0) {
          return found;
        }
        if (nextOpen >= 0 && nextOpen < nextClose) {
          const openEnd = text.indexOf('>', nextOpen);
          depth += text[openEnd - 1] === '/' ? 0 : 1;
          cursor = openEnd + 1;
        } else {
          depth -= 1;
          cursor = nextClose + '</Material>'.length;
        }
      }
      end = cursor;
    }
    found.push({ name, start, end });
    at = end;
  }
  return found;
}

// A material's own definition (a single material's <Material>, without the XML line), to stand in
// a slot: its SubMaterials left out.
function ownDefinition(text) {
  const start = text.indexOf('<Material');
  const end = text.lastIndexOf('</Material>');
  if (start < 0 || end < 0) {
    throw new Error('the material has no definition');
  }
  return text.slice(start, end + '</Material>'.length);
}

// The model's material text with the slot `slot` (its index, or its name) drawn as `definition`
// (a <Material>...</Material>), keeping the slot's name; throws when the slot is not there.
function withSlot(text, slot, definition) {
  const slots = subMaterials(text);
  const index = Number.isInteger(slot) ? slot : slots.findIndex((entry) => entry.name === slot);
  const target = slots[index];
  if (!target) {
    throw new Error(`the material has no slot ${slot} (it has ${slots.length})`);
  }
  const named = definition.replace(/^<Material([^>]*?)\sName="[^"]*"/, '<Material$1');
  const renamed = named.replace(/^<Material/, `<Material Name="${attribute(target.name)}"`);
  return text.slice(0, target.start) + renamed + text.slice(target.end);
}

// The definition of a slot (index or name) of a material's text, for another slot to look like it.
function slotDefinition(text, slot) {
  const slots = subMaterials(text);
  const index = Number.isInteger(slot) ? slot : slots.findIndex((entry) => entry.name === slot);
  const found = slots[index];
  if (!found) {
    throw new Error(`the material has no slot ${slot}`);
  }
  return text.slice(found.start, found.end);
}

// A definition with its pictures (diffuse, normal, height and detail maps) repeated `tile` times
// on a face: each <Texture> of them gets TileU and TileV, a TexMod added where it has none.
function retiled(definition, tile) {
  const value = Math.round(tile * 1000) / 1000;
  const maps = /^(Diffuse|Bumpmap|Heightmap|Detail|Specular)$/;
  return definition.replace(
    /<Texture\b([^>]*?)(\/>|>([\s\S]*?)<\/Texture>)/g,
    (whole, attributes, end, inner) => {
      const map = /\bMap="([^"]*)"/.exec(attributes)?.[1] ?? '';
      if (!maps.test(map)) {
        return whole;
      }
      if (end === '/>') {
        return `<Texture${attributes}><TexMod TileU="${value}" TileV="${value}" /></Texture>`;
      }
      if (/<TexMod\b/.test(inner)) {
        const tiled = inner
          .replace(/\sTileU="[^"]*"/, '')
          .replace(/\sTileV="[^"]*"/, '')
          .replace(/<TexMod\b/, `<TexMod TileU="${value}" TileV="${value}"`);
        return `<Texture${attributes}>${tiled}</Texture>`;
      }
      return `<Texture${attributes}>${inner}<TexMod TileU="${value}" TileV="${value}" /></Texture>`;
    },
  );
}

// Bytes as base64 text, a piece at a time (the editor's btoa takes text).
function base64Of(bytes) {
  let text = '';
  const piece = 0x8000;
  for (let at = 0; at < bytes.length; at += piece) {
    text += String.fromCharCode(...bytes.subarray(at, at + piece));
  }
  return btoa(text);
}

module.exports = {
  KINDS,
  SURFACES,
  SIZES,
  NAME,
  drawTexture,
  normalMap,
  mipLevels,
  encodeDds,
  materialXml,
  subMaterials,
  ownDefinition,
  withSlot,
  slotDefinition,
  retiled,
  base64Of,
  randomOf,
};
