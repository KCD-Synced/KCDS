'use strict';

// The maps one server runs at once (server.json maps): each player is on one of them, and
// nothing on one map meets anything on another. The native server tells maps apart by the
// dimension value (Backend/replication/.../world.hpp): the first map keeps the values a one-map
// server uses, map k from 1 on is (k << 24) | world, with 0xffffff as that map's global world.
// Scripts keep seeing their own worlds (Player.dimension); the map is added only on the way to
// the native server and taken off on the way back.

const GLOBAL_WORLD = 0xffffffff;
const MAP_SHIFT = 24;
const WORLD_MASK = 0xffffff;
const MAP_STEP = 2 ** MAP_SHIFT;

// The native dimension value of a script's world on a map (an index of the server's maps).
function onMap(map, world) {
  if (map === 0) {
    return world;
  }
  if (world === GLOBAL_WORLD) {
    return map * MAP_STEP + WORLD_MASK;
  }
  return map * MAP_STEP + (world % MAP_STEP);
}

// The map a native dimension value lies on.
function mapOf(dimension) {
  if (dimension === GLOBAL_WORLD) {
    return 0;
  }
  return Math.floor(dimension / MAP_STEP);
}

// The script's world of a native dimension value: its map taken off.
function worldOf(dimension) {
  if (mapOf(dimension) === 0) {
    return dimension;
  }
  const world = dimension % MAP_STEP;
  return world === WORLD_MASK ? GLOBAL_WORLD : world;
}

// Whether a script's world can lie on a map of a server with `count` maps: a server that runs
// more than one keeps numbered worlds below 0xffffff, the rest of the value names the map.
function worldFitsMaps(world, count) {
  return count <= 1 || world === GLOBAL_WORLD || world < WORLD_MASK;
}

// The server's maps from the resource host's arguments: --maps=a,b (more than one map) or the
// --map= level alone.
function mapsFromArguments(map, mapsArgument) {
  const listed = (mapsArgument ?? '')
    .split(',')
    .map((name) => name.trim())
    .filter(Boolean);
  if (listed.length) {
    return listed;
  }
  return map ? [map] : [];
}

module.exports = {
  GLOBAL_WORLD,
  WORLD_MASK,
  mapOf,
  mapsFromArguments,
  onMap,
  worldFitsMaps,
  worldOf,
};
