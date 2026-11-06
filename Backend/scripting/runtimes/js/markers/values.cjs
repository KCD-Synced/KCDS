'use strict';

// Checks and conversions the marker and blip handles share.

const { Vector3 } = require('../math.cjs');
const { validWorld } = require('../npcs/npc_values.cjs');
const { rotationOf } = require('../areas/area_shapes.cjs');

const GLOBAL_WORLD = 0xffffffff;
const LABEL_BYTES = 64;
const encoder = new TextEncoder();

function isFinite(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

// A position as a Vector3. A coordinate left out is 0; one that is not a finite number is an error.
function positionOf(value, name) {
  if (value === undefined) {
    return new Vector3(0, 0, 0);
  }
  if (value === null || typeof value !== 'object') {
    throw new TypeError(`${name} takes a position with finite x, y and z`);
  }
  const coordinates = [value.x ?? 0, value.y ?? 0, value.z ?? 0];
  if (!coordinates.every(isFinite)) {
    throw new TypeError(`${name} takes a position with finite x, y and z`);
  }
  return new Vector3(coordinates[0], coordinates[1], coordinates[2]);
}

// A unit quaternion from Euler degrees or a quaternion.
function unitRotationOf(value, name) {
  try {
    return rotationOf(value);
  } catch (error) {
    if (error instanceof TypeError || error instanceof RangeError) {
      throw new error.constructor(error.message.replace('Area rotation', `${name} rotation`));
    }
    throw error;
  }
}

// A dimension number; a world left out is the global one.
function worldOf(value, name) {
  if (value === undefined) {
    return GLOBAL_WORLD;
  }
  if (!validWorld(value)) {
    throw new TypeError(`${name} takes a valid dimension`);
  }
  return value;
}

// The ids of the players an object is limited to, or null for every player. A player, a list of
// players, or null.
function viewersOf(host, target) {
  if (target === null) {
    return null;
  }
  const players = Array.isArray(target) ? target : [target];
  const ids = new Set();
  for (const player of players) {
    if (!player || host.players.get(player.id) !== player) {
      throw new TypeError('showOnlyTo takes a Player, a list of Players or null');
    }
    ids.add(player.id);
  }
  return ids;
}

function clamp(value, low, high, name) {
  if (!isFinite(value)) {
    throw new TypeError(`${name} takes a finite number`);
  }
  return Math.min(high, Math.max(low, value));
}

// A label cut to LABEL_BYTES bytes of UTF-8 without splitting a character.
function labelOf(value) {
  if (typeof value !== 'string') {
    throw new TypeError('A blip label takes a string');
  }
  let label = value;
  while (encoder.encode(label).length > LABEL_BYTES) {
    label = Array.from(label).slice(0, -1).join('');
  }
  return label;
}

// Whether two field values are the same: numbers, texts and flags by value, a position or a
// rotation by its components.
function sameValue(a, b) {
  if (a !== null && typeof a === 'object') {
    return a.x === b.x && a.y === b.y && a.z === b.z && a.w === b.w;
  }
  return a === b;
}

function plain(vector) {
  return { x: vector.x, y: vector.y, z: vector.z };
}

function plainRotation(q) {
  return { x: q.x, y: q.y, z: q.z, w: q.w };
}

module.exports = {
  GLOBAL_WORLD,
  clamp,
  labelOf,
  plain,
  plainRotation,
  positionOf,
  sameValue,
  unitRotationOf,
  viewersOf,
  worldOf,
};
