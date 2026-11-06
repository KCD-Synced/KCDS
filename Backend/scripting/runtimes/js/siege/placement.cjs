'use strict';

// What the siege ladders and the stone piles share: the checks a placement goes through and the
// reading of a player argument.

const { rotation, vector } = require('../math.cjs');

const GLOBAL_WORLD = 0xffffffff;
// A placement farther out than this lies outside any level.
const WORLD_LIMIT = 1e6;

function validWorld(world) {
  return Number.isSafeInteger(world) && world >= 0 && world <= GLOBAL_WORLD;
}

// Throws unless the resource that is running the call is still running.
function requireRunning(host) {
  const owner = host.context.getStore();
  if (owner && (host.resources.get(owner.name) !== owner || !owner.active || owner.failing)) {
    throw new Error('Resource is stopped');
  }
}

// The checked { at, facing, world } of a placement: plain copies of the vector and quaternion.
function placementOf(what, position, orientation, dimension) {
  const world = dimension ?? GLOBAL_WORLD;
  if (!validWorld(world)) {
    throw new TypeError('Invalid dimension');
  }
  const at = vector(position);
  if (![at.x, at.y, at.z].every((n) => Number.isFinite(n) && Math.abs(n) <= WORLD_LIMIT)) {
    throw new RangeError(`Invalid ${what} position`);
  }
  const facing = rotation(orientation);
  return {
    at: { x: at.x, y: at.y, z: at.z },
    facing: { x: facing.x, y: facing.y, z: facing.z, w: facing.w },
    world,
  };
}

// The player an argument names (a Player, its id, or nothing), or null.
function playerOf(host, value) {
  if (value === undefined || value === null) {
    return null;
  }
  const id = typeof value === 'object' ? value.id : value;
  return host.players.get(id) ?? null;
}

const distance = (a, b) => Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z);

module.exports = { GLOBAL_WORLD, distance, placementOf, playerOf, requireRunning, validWorld };
