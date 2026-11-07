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
// Under structures, what is no building to go into: paths, tunnels, tents, scaffolding, fences,
// heaps.
const NOT_BUILDINGS = [
  '/paths/',
  '/tunnels/',
  '/tents/',
  '/scafolding/',
  '/scaffolding/',
  '/palisade/',
  '/composts/',
  '/tailings/',
  '/hay/',
  '/walls/',
  '/bridges/',
];
// The ground levelled under an opened building's floors: circles this wide (metres, at least and
// at most), this far under the floor, fading this far, and at most this many circles.
const GROUND_RADIUS_LEAST = 1.5;
const GROUND_RADIUS_MOST = 4;
const GROUND_UNDER_FLOOR = 0.3;
const GROUND_DABS_MOST = 120;
// No box wider than this (metres) has its ground levelled.
const GROUND_BOX_MOST = 200;
// Fill lights: one per this many metres of a room each way, reaching this many times the cell
// they light, this high over the floor at least and at most, this far apart at least, and at most
// this many.
const FILL_SPACING = 10;
const FILL_REACH = 1.3;
const FILL_LOWEST = 2.5;
const FILL_HIGHEST = 6;
const FILL_APART = 4;
const FILL_MOST = 16;
// Wall lights of a tall main room (wallSpotsOf): a room taller than WALL_TALL gets them,
// WALL_INSET in from each wall, one per WALL_SPACING metres along it, in rows from WALL_LOWEST
// over the floor every WALL_ROW_STEP up to WALL_UNDER_TOP under its top, WALL_ROWS rows and
// WALL_MOST lights at most.
const WALL_TALL = 10;
const WALL_INSET = 1.5;
const WALL_SPACING = 10;
const WALL_LOWEST = 3;
const WALL_ROW_STEP = 6;
const WALL_UNDER_TOP = 2;
const WALL_ROWS = 3;
const WALL_MOST = 30;

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

// Whether a model is a building opened already (a walk-in copy in the opened folder).
function isOpenedModel(mesh) {
  const path = String(mesh ?? '')
    .replace(/\\/g, '/')
    .toLowerCase();
  return path.startsWith(`${core.OPENED_FOLDER}/`);
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
  return (
    BUILDING_PARTS.some((part) => path.includes(part)) &&
    !NOT_BUILDINGS.some((part) => path.includes(part))
  );
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
  // Its main room: the largest on its ground storey (a church's tower is a box too).
  const largest = core.mainRoom(removed);
  const [cx, cy, cz] = largest.centre;
  const [sx, sy, sz] = largest.size;
  const floor = core.floorTopOf(largest);
  const spot = worldOf([cx, cy, floor + Math.min(sz / 3, 3)], at, turn, scale);
  // It reaches every room of the ground storey from there (St James's chancel, 20 m from its
  // nave's middle, stayed lit by the sky alone): to the farthest corner of them.
  const rooms = core.groundRooms(removed);
  let reach = Math.hypot(sx, sy) / 2;
  for (const room of rooms.length > 0 ? rooms : [largest]) {
    for (const [dx, dy] of [
      [-1, -1],
      [-1, 1],
      [1, -1],
      [1, 1],
    ]) {
      const x = room.centre[0] + (dx * room.size[0]) / 2 - cx;
      const y = room.centre[1] + (dy * room.size[1]) / 2 - cy;
      reach = Math.max(reach, Math.hypot(x, y));
    }
  }
  const radius = Math.min(40, Math.max(6, reach * scale + 2));
  return {
    at: spot,
    radius: Math.round(radius * 10) / 10,
    middle: at.z + scale * cz,
    // The floor put in (the ground inside may lie over it), and the box's bottom.
    floor: at.z + scale * floor,
    bottom: at.z + scale * (cz - sz / 2),
  };
}

// Where the ground under an opened building's floors is levelled, just under each floor (a closed
// building hid the ground inside it, a mound through its floor or a hollow under it): circles of
// `radius` on a grid `radius` apart, kept `radius` inside each removed box, so they reach its
// sides and not past them. [{ at: { x, y, z }, radius }] in the world.
function groundDabs(removed, at, turn, scale = 1) {
  const dabs = [];
  // Only under the floors of its ground storey: an upper storey's floor never raises the ground.
  for (const floor of core.groundFloors(Array.isArray(removed) ? removed : [])) {
    const cx = (floor.min[0] + floor.max[0]) / 2;
    const cy = (floor.min[1] + floor.max[1]) / 2;
    const sx = floor.max[0] - floor.min[0];
    const sy = floor.max[1] - floor.min[1];
    if (
      !(Number.isFinite(sx) && Number.isFinite(sy) && sx < GROUND_BOX_MOST && sy < GROUND_BOX_MOST)
    ) {
      continue;
    }
    const radius = Math.max(
      GROUND_RADIUS_LEAST,
      Math.min(GROUND_RADIUS_MOST, Math.min(sx, sy) / 2),
    );
    const z = floor.top - GROUND_UNDER_FLOOR;
    const along = (size) => {
      const inner = Math.max(0, size / 2 - radius);
      const steps = Math.max(1, Math.ceil((2 * inner) / radius));
      const spots = [];
      for (let k = 0; k <= steps; k += 1) {
        spots.push(-inner + (2 * inner * k) / steps);
      }
      return spots;
    };
    for (const x of along(sx)) {
      for (const y of along(sy)) {
        dabs.push({ at: worldOf([cx + x, cy + y, z], at, turn, scale), radius: radius * scale });
      }
    }
  }
  return dabs.slice(0, GROUND_DABS_MOST);
}

// Where an opened building's warm fill lights hang: over every room of its ground storey, one
// per FILL_SPACING metres of it each way (a light reaches a wall a few metres off; St James's
// walls 15 m from one in the middle stayed cold), halfway up the room (at most FILL_HIGHEST
// over the floor), none closer than FILL_APART to another, at most FILL_MOST.
// [{ at: { x, y, z }, radius }] in the world.
function fillSpotsOf(removed, at, turn, scale = 1) {
  const rooms = core.groundRooms(Array.isArray(removed) ? removed : []);
  const spots = [];
  for (const room of rooms) {
    const [cx, cy, cz] = room.centre;
    const [sx, sy, sz] = room.size;
    if (
      !(Number.isFinite(sx) && Number.isFinite(sy) && sx < GROUND_BOX_MOST && sy < GROUND_BOX_MOST)
    ) {
      continue;
    }
    const floor = core.floorTopOf(room);
    const top = cz + sz / 2;
    const z = floor + Math.min(FILL_HIGHEST, Math.max(FILL_LOWEST, (top - floor) / 2));
    const across = Math.max(1, Math.round(sx / FILL_SPACING));
    const along = Math.max(1, Math.round(sy / FILL_SPACING));
    const radius = Math.max(sx / across, sy / along) * FILL_REACH;
    for (let i = 0; i < across; i += 1) {
      for (let j = 0; j < along; j += 1) {
        const x = cx - sx / 2 + (sx * (i + 0.5)) / across;
        const y = cy - sy / 2 + (sy * (j + 0.5)) / along;
        const spot = worldOf([x, y, z], at, turn, scale);
        const near = spots.some(
          (other) => Math.hypot(other.at.x - spot.x, other.at.y - spot.y) < FILL_APART * scale,
        );
        if (!near && spots.length < FILL_MOST) {
          spots.push({ at: spot, radius: Math.round(radius * scale * 10) / 10 });
        }
      }
    }
  }
  return spots;
}

// Where a tall main room's wall lights hang (a church nave, its walls 20 m high, its sky light
// coming through windows high up: the fills, at most FILL_HIGHEST up, left St James's walls dark
// and cold): along the inside of each side of its box, in rows up the walls. Taken from the box
// the opener removed, not from rays: the copy's walls take a moment to stand in the game. [{ x,
// y, z }] in the world; none for a room of WALL_TALL or lower.
function wallSpotsOf(removed, at, turn, scale = 1) {
  const main = core.mainRoom(Array.isArray(removed) ? removed : []);
  if (!main) {
    return [];
  }
  const [cx, cy, cz] = main.centre;
  const [sx, sy, sz] = main.size;
  if (!(sz * scale > WALL_TALL) || !(sx < GROUND_BOX_MOST && sy < GROUND_BOX_MOST)) {
    return [];
  }
  const floor = core.floorTopOf(main);
  const top = cz + sz / 2 - WALL_UNDER_TOP / scale;
  const rows = [];
  for (let z = floor + WALL_LOWEST / scale; rows.length < WALL_ROWS && z <= top; ) {
    rows.push(z);
    z += WALL_ROW_STEP / scale;
  }
  const inset = WALL_INSET / scale;
  const spacing = WALL_SPACING / scale;
  // Points along one side from (x0, y0) to (x1, y1), each in the middle of its stretch.
  const along = (x0, y0, x1, y1) => {
    const length = Math.hypot(x1 - x0, y1 - y0);
    const count = Math.max(1, Math.round(length / spacing));
    return Array.from({ length: count }, (_, i) => {
      const t = (i + 0.5) / count;
      return [x0 + (x1 - x0) * t, y0 + (y1 - y0) * t];
    });
  };
  const west = cx - sx / 2 + inset;
  const east = cx + sx / 2 - inset;
  const south = cy - sy / 2 + inset;
  const north = cy + sy / 2 - inset;
  const points = [
    ...along(west, south, west, north),
    ...along(east, south, east, north),
    ...along(west, south, east, south),
    ...along(west, north, east, north),
  ];
  const spots = [];
  for (const z of rows) {
    for (const [x, y] of points) {
      if (spots.length < WALL_MOST) {
        spots.push(worldOf([x, y, z], at, turn, scale));
      }
    }
  }
  return spots;
}

// Opens a building's model into the author's assets: open_core.cjs openWith's answer.
function openBuildingFiles(api, options) {
  return core.openWith(gameIo(api), options);
}

module.exports = {
  openBuildingFiles,
  isOpenedModel,
  opensAsBuilding,
  openedName,
  worldOf,
  roomLightOf,
  groundDabs,
  fillSpotsOf,
  wallSpotsOf,
  gameIo,
};
