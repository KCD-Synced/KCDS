'use strict';

// World.replicasAround: the server's own entities near a point, nearest first. It needs
// no client, as the server knows where its replicas are. Gates are left out: their catalog has no
// positions.

const { GLOBAL_WORLD } = require('../npcs/npc_values.cjs');

function finiteNumber(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function replicasAround(api, position, radius, dimension = GLOBAL_WORLD) {
  if (
    !position ||
    !finiteNumber(position.x) ||
    !finiteNumber(position.y) ||
    !finiteNumber(position.z)
  ) {
    throw new TypeError('World.replicasAround: position needs finite x, y and z');
  }
  if (!finiteNumber(radius) || radius < 0) {
    throw new TypeError('World.replicasAround: radius is a number of metres, 0 or more');
  }
  if (!Number.isInteger(dimension)) {
    throw new TypeError('World.replicasAround: dimension is a whole number');
  }
  const kinds = [
    api.Player,
    api.Horse,
    api.Npc,
    api.Dog,
    api.Prop,
    api.GroundItem,
    api.Door,
    api.Stash,
  ];
  const found = [];
  for (const kind of kinds) {
    for (const entity of kind.all()) {
      const at = entity.position;
      if (!at || entity.dimension !== dimension) {
        continue;
      }
      const distance = Math.hypot(at.x - position.x, at.y - position.y, at.z - position.z);
      if (distance <= radius) {
        found.push({ entity, distance });
      }
    }
  }
  found.sort((first, second) => first.distance - second.distance);
  return found.map((row) => row.entity);
}

module.exports = { replicasAround };
