'use strict';

// A closed building of the level opened in the game, for World Architect's "Open it" (Details of
// a level building) and an AI assistant's open_building: the same opening as the MCP server's
// open_model (Dev/tools/architect-mcp/models/open_core.cjs), its files read through the game
// (architect/asset_files.hpp: the model in parts, the streamed meshes split by the game itself)
// and written into the author's assets folder.

const core = require('../../../Dev/tools/architect-mcp/models/open_core.cjs');
const { Bytes } = require('../../../Dev/tools/architect-mcp/models/bytes.cjs');
const { writeAsset, utf8 } = require('./material_work.cjs');

// The parts of a mesh path that are a building of the level (as the client's level brushes
// tell the fixed ones from loose props).
const BUILDING_PARTS = ['/structures/', '/buildings/', '/fortress/', '/castle'];

// Reading the game's files and writing the author's through the runtime, for open_core.cjs.
function gameIo(api) {
  return {
    read: (file) => {
      const bytes = api.assetBytes?.(file);
      return bytes ? Bytes.from(bytes) : null;
    },
    text: (file) => api.assetText?.(file) ?? null,
    write: (file, data) => {
      const bytes = typeof data === 'string' ? utf8(data) : data;
      writeAsset((message) => api.send(message), file, bytes);
    },
    split: (from, base, material) => {
      const made = api.splitModel?.(from, base, material);
      return Array.isArray(made?.meshes) ? made.meshes : null;
    },
  };
}

// Whether a mesh is a building of the level that can be opened (not one opened already).
function opensAsBuilding(mesh) {
  const path = String(mesh ?? '')
    .replace(/\\/g, '/')
    .toLowerCase();
  // Not one opened already, nor the level's far-away stand-in for a block of buildings (hlods/).
  if (
    !path.endsWith('.cgf') ||
    !path.startsWith('objects/') ||
    path.startsWith(`${core.OPENED_FOLDER}/`)
  ) {
    return false;
  }
  return BUILDING_PARTS.some((part) => path.includes(part));
}

// A free name for a building's opened copy: its model's file name, or with _2, _3, ... when
// a copy of that name is in `taken` (the author's meshes) already.
function openedName(mesh, taken) {
  const stem = String(mesh)
    .replace(/\\/g, '/')
    .split('/')
    .pop()
    .replace(/\.cgf$/i, '')
    .toLowerCase()
    .replace(/[^a-z0-9_]/g, '_')
    .slice(0, 40);
  const files = [...(taken ?? [])].map((file) => String(file).toLowerCase());
  // A copy's files are <name>.cgf, or <name>_<n>.cgf and <name>_walls.cgf.
  const free = (name) => {
    const whole = `${core.OPENED_FOLDER}/${name}.cgf`;
    const part = `${core.OPENED_FOLDER}/${name}_`;
    return !files.some((file) => file === whole || file.startsWith(part));
  };
  if (free(stem)) {
    return stem;
  }
  let n = 2;
  while (!free(`${stem}_${n}`)) {
    n += 1;
  }
  return `${stem}_${n}`;
}

// A point of the model's frame where a building stands at `at`, turned `turn` degrees.
function worldOf(point, at, turn, scale = 1) {
  const t = (turn * Math.PI) / 180;
  const c = Math.cos(t);
  const s = Math.sin(t);
  return {
    x: at.x + scale * (point[0] * c - point[1] * s),
    y: at.y + scale * (point[0] * s + point[1] * c),
    z: at.z + scale * point[2],
  };
}

// Where its room light goes: the middle of its largest removed box, a third of the way up (at
// most 3 m), with a radius that reaches its corners, and the heights of the box's middle and
// bottom in the world (to find the real floor between them); null when no box was removed.
function roomLightOf(removed, at, turn, scale = 1) {
  if (!Array.isArray(removed) || removed.length === 0) {
    return null;
  }
  const volume = (box) => box.size[0] * box.size[1] * box.size[2];
  const largest = removed.reduce((best, box) => (volume(box) > volume(best) ? box : best));
  const [cx, cy, cz] = largest.centre;
  const [sx, sy, sz] = largest.size;
  const spot = worldOf([cx, cy, cz - sz / 2 + Math.min(sz / 3, 3)], at, turn, scale);
  const radius = Math.min(40, Math.max(6, (Math.hypot(sx, sy) / 2) * scale + 2));
  return {
    at: spot,
    radius: Math.round(radius * 10) / 10,
    middle: at.z + scale * cz,
    bottom: at.z + scale * (cz - sz / 2),
  };
}

// Opens a building's model into the author's assets: open_core.cjs openWith's answer.
function openBuildingFiles(api, options) {
  return core.openWith(gameIo(api), options);
}

module.exports = { openBuildingFiles, opensAsBuilding, openedName, worldOf, roomLightOf, gameIo };
