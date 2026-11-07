'use strict';

// The editor's material work (editor.cjs): a texture drawn by code made into the author's own
// material, and a model's material with one slot drawn as another material. The game client
// writes the files into the assets folder (architect/asset_files.hpp) and reads a model's slots
// and a material's text (the author's or the game's); the editor then loads the folder again
// and draws props with the new material.

const M = require('./materials.cjs');

// A file goes in parts of this many bytes: a multiple of 3, so each part is base64 of its own, and
// its base64 (4/3 as long, 48 KiB) with the message around it under the 64 KiB a message holds.
const PART_BYTES = 36 * 1024;
// Where the editor's materials of models with a slot changed go.
const SLOT_FOLDER = 'objects/kcds/materials';

function utf8(text) {
  return new TextEncoder().encode(text);
}

// Sends a file to the game client to write into the assets folder, in parts.
function writeAsset(send, path, bytes) {
  const parts = Math.max(1, Math.ceil(bytes.length / PART_BYTES));
  for (let part = 0; part < parts; part += 1) {
    const data = M.base64Of(bytes.subarray(part * PART_BYTES, (part + 1) * PART_BYTES));
    send({ op: 'architectAssetWrite', path, data, part, parts });
  }
}

// A texture of a kind drawn and written as the author's material objects/kcds/<folder>/<name>:
// its diffuse picture and normal map (with mip levels) and the material drawing them. Answers
// { material, diffuse, normal, size }.
function makeTexture(send, options) {
  const name = String(options.name ?? '');
  const folder = String(options.folder ?? 'textures');
  if (!M.NAME.test(name) || !M.NAME.test(folder)) {
    throw new Error('name and folder are 1 to 32 lower-case letters, digits or _');
  }
  const kind = M.KINDS[options.kind];
  if (!kind) {
    throw new Error(`kind is one of ${Object.keys(M.KINDS).join(', ')}`);
  }
  if (options.color !== undefined) {
    const fits = (part) => typeof part === 'number' && part >= 0 && part <= 1;
    if (!Array.isArray(options.color) || options.color.length !== 3 || !options.color.every(fits)) {
      throw new Error('color is [r, g, b], each 0 to 1');
    }
  }
  const tile = options.tile ?? kind.tile;
  if (!(typeof tile === 'number' && tile >= 0.05 && tile <= 50)) {
    throw new Error('tile is 0.05 to 50 (times the texture repeats on a face)');
  }
  const surface = options.surface ?? kind.surface;
  if (!M.SURFACES[surface]) {
    throw new Error(`surface is one of ${Object.keys(M.SURFACES).join(', ')}`);
  }
  const drawn = M.drawTexture(options);
  const normal = M.normalMap(drawn.height, drawn.size, options.relief ?? 4);
  const base = `objects/kcds/${folder}`;
  const diffusePath = `${base}/textures/${name}_diff.dds`;
  const normalPath = `${base}/textures/${name}_ddna.dds`;
  const material = `${base}/${name}`;
  writeAsset(send, diffusePath, M.encodeDds(M.mipLevels(drawn.rgba, drawn.size), true));
  writeAsset(send, normalPath, M.encodeDds(M.mipLevels(normal, drawn.size), false));
  const xml = M.materialXml({ diffuse: diffusePath, normal: normalPath, surface, tile });
  writeAsset(send, `${material}.mtl`, utf8(xml));
  return { material, diffuse: diffusePath, normal: normalPath, size: drawn.size };
}

// The slots of a model's material a person or an assistant would change: not the model's lower
// details ($lod...), its collision (proxy...) or its shadow stand-in.
function editableSlot(name) {
  return !/^\$|proxy/i.test(name);
}

// A model's material with its slot `slot` drawn as `look`: { material } (one of the author's or
// a game material: its own definition, or its slot `lookSlot` when it has slots), or { like } (an
// other slot of the same material), or null (the slot as the model has it); tile repeats the
// look's pictures. `text` is the material being changed, `original` the model's own (for null),
// `read(path)` a material's text. Answers the new material's text.
function slotText({ text, original, slot, look, tile, read }) {
  let definition = null;
  if (look === null) {
    definition = M.slotDefinition(original, slot);
  } else if (typeof look?.like === 'string' || Number.isInteger(look?.like)) {
    definition = M.slotDefinition(text, look.like);
  } else if (typeof look?.material === 'string') {
    const source = read(`${look.material}.mtl`);
    if (typeof source !== 'string') {
      throw new Error(`no material ${look.material}`);
    }
    const hasSlots = M.subMaterials(source).length > 0;
    if (hasSlots && look.lookSlot === undefined) {
      throw new Error(`${look.material} has slots: say which one (lookSlot)`);
    }
    definition = hasSlots ? M.slotDefinition(source, look.lookSlot) : M.ownDefinition(source);
  } else {
    throw new Error('the look is { material }, { like } or null');
  }
  if (tile !== undefined) {
    if (!(typeof tile === 'number' && tile >= 0.05 && tile <= 50)) {
      throw new Error('tile is 0.05 to 50');
    }
    definition = M.retiled(definition, tile);
  }
  return M.withSlot(text, slot, definition);
}

// A free name for a model's changed material: objects/kcds/materials/<model>_<n>.
function slotMaterialName(mesh, taken) {
  const stem =
    String(mesh)
      .split('/')
      .pop()
      .replace(/\.cgf$/i, '')
      .toLowerCase()
      .replace(/[^a-z0-9_]/g, '_')
      .slice(0, 24) || 'model';
  for (let n = 1; ; n += 1) {
    const name = `${SLOT_FOLDER}/${stem}_${n}`;
    if (!taken.includes(name)) {
      return name;
    }
  }
}

module.exports = { makeTexture, slotText, slotMaterialName, editableSlot, writeAsset, utf8 };
