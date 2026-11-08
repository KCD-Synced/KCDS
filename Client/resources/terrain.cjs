'use strict';

// The client-side Terrain (World Architect terraforming): a client script shapes the level's
// ground in this game only, as an editor does under a building it places. Nobody else sees it and
// nothing goes to the server. The game client writes the terrain's own heights and has the game
// rebuild them, solid at once (Client/runtime/terrain/terrain_edits.hpp):
//
//   { op: 'localTerrain', kind: 'edit', id, shape: 'flatten' | 'raise' | 'paint', x, y, radius,
//     falloff, value, material? }
//
// paint covers the ground with a material (value: the level's surface type index; material: its
// name, which a game level finds among its own surface types): the ground's look, footsteps and
// dust. A World Architect custom map carries the 15 GROUND_MATERIALS in every sector; on a game
// level a sector without the material takes it into its palette when the level defines it.
//   { op: 'localTerrain', kind: 'revert', id }
//
// hideGrass takes the level's grass carpet away in a circle (the World Architect editor's own
// grass message), and its undo brings it back:
//   { op: 'architectGrass', kind: 'hide', id, x, y, radius } and { ..., kind: 'show', id }
// Its ids start at GRASS_ID_BASE, apart from the editor's, which count from 1.
//
// Every edit can be undone; a resource's edits are undone when it stops.

const isNumber = (value) => typeof value === 'number' && Number.isFinite(value);

// The widest edit, metres from its middle to the end of its falloff.
const REACH_MAX = 90;
const GRASS_ID_BASE = 0x40000000;
const WORLD_LIMIT = 1e6;
const HEIGHT_LIMIT = 10000;
const handleState = Symbol('kcds.terrainEdit');
// The ground materials by name and their surface type index in the level (a custom map's,
// Dev/tools/architect/make_custom_map.py GROUND_MATERIALS; klaster's leveldata.xml SurfaceTypes).
const GROUND_MATERIALS = Object.freeze({
  grass: 18,
  meadow: 8,
  pasture: 29,
  trampled: 16,
  soil: 14,
  mud: 6,
  stony_mud: 34,
  sand: 10,
  gravel: 3,
  forest: 4,
  forest_leaves: 12,
  cobblestones: 37,
  paving: 22,
  cobblestones_grass: 38,
  river_shore: 15,
});
// What a game level calls each material: parts of its surface types' names (their material files,
// such as materials/terrain/cobblestone/cobble_stones_a_2x2), the first a level has wins.
const GROUND_NAMES = Object.freeze({
  grass: ['grass_pasture_a', 'grass'],
  meadow: ['grass_pasture_b', 'meadow', 'pasture'],
  pasture: ['kh_pasture', 'pasture'],
  trampled: ['trampled'],
  soil: ['soil/soil_a', 'soil_a', 'soil'],
  mud: ['/mud', 'soil_wet', 'wetlands'],
  stony_mud: ['stony_mud'],
  sand: ['sand_soil', 'sand'],
  gravel: ['gravel'],
  forest: ['forest_mixed', 'forest'],
  forest_leaves: ['beech', 'leaves'],
  cobblestones: ['cobble_stones_a', 'cobble'],
  paving: ['paving', 'cobble_stones_b', 'cobble'],
  cobblestones_grass: ['cobble_stones_grass', 'cobble_stones_b', 'cobble'],
  river_shore: ['river_shore', 'shore'],
});
// A level holds at most 127 surface types (a sector's table marks a free slot 127).
const SURFACE_LIMIT = 127;

function centerOf(center, what) {
  if (!center || !isNumber(center.x) || !isNumber(center.y)) {
    throw new TypeError(`${what} takes a position with x and y`);
  }
  if (Math.max(Math.abs(center.x), Math.abs(center.y)) > WORLD_LIMIT) {
    throw new RangeError(`${what} is out of the world`);
  }
  return { x: center.x, y: center.y };
}

function sizeOf(radius, falloff, what) {
  if (!isNumber(radius) || radius < 0) {
    throw new TypeError(`${what} radius is a number of metres, 0 or more`);
  }
  const fade = falloff === undefined ? radius : falloff;
  if (!isNumber(fade) || fade < 0) {
    throw new TypeError(`${what} falloff is a number of metres, 0 or more`);
  }
  if (radius + fade > REACH_MAX) {
    throw new RangeError(`${what} reaches at most ${REACH_MAX} m from its middle`);
  }
  return { radius, falloff: fade };
}

function heightOf(value, what) {
  if (!isNumber(value) || Math.abs(value) > HEIGHT_LIMIT) {
    throw new TypeError(`${what} takes a number of metres`);
  }
  return value;
}

function materialOf(material) {
  if (typeof material === 'string') {
    if (!Object.hasOwn(GROUND_MATERIALS, material)) {
      throw new RangeError(
        `Terrain.paint material is one of ${Object.keys(GROUND_MATERIALS).join(', ')}`,
      );
    }
    return GROUND_MATERIALS[material];
  }
  if (!Number.isInteger(material) || material < 0 || material >= SURFACE_LIMIT) {
    throw new TypeError('Terrain.paint takes a material name or a surface type index (0 to 126)');
  }
  return material;
}

class ResourceTerrain {
  constructor({ send = () => {} } = {}) {
    this.send = send;
    this.nextId = 1;
    // id -> { owner, handle }
    this.edits = new Map();
  }

  // `material`: a paint's material name, which a game level finds among its own surface types.
  edit(owner, TerrainEdit, shape, center, value, size, material = null) {
    const id = this.nextId;
    this.nextId += 1;
    const handle = new TerrainEdit(handleState);
    Object.defineProperty(handle, 'id', { value: id, enumerable: true });
    Object.defineProperty(handle, 'shape', { value: shape, enumerable: true });
    this.edits.set(id, { owner, handle });
    if (shape === 'grass') {
      this.send({
        op: 'architectGrass',
        kind: 'hide',
        id: GRASS_ID_BASE + id,
        x: center.x,
        y: center.y,
        radius: size.radius,
      });
      return handle;
    }
    this.send({
      op: 'localTerrain',
      kind: 'edit',
      id,
      shape,
      x: center.x,
      y: center.y,
      radius: size.radius,
      falloff: size.falloff,
      value,
      ...(material ? { material } : {}),
    });
    return handle;
  }

  undo(id) {
    const entry = this.edits.get(id);
    if (!entry) {
      return false;
    }
    this.edits.delete(id);
    if (entry.handle.shape === 'grass') {
      this.send({ op: 'architectGrass', kind: 'show', id: GRASS_ID_BASE + id });
    } else {
      this.send({ op: 'localTerrain', kind: 'revert', id });
    }
    return true;
  }

  // The Terrain object of one resource.
  facade(owner) {
    const registry = this;
    class TerrainEdit {
      constructor(token) {
        if (token !== handleState) {
          throw new TypeError('Terrain.flatten, Terrain.raise and Terrain.paint make edits');
        }
      }
      // False once undone or its resource stopped.
      get valid() {
        return registry.edits.get(this.id)?.handle === this;
      }
      // Puts the ground back as it was before this edit; false when it already was.
      undo() {
        return this.valid && registry.undo(this.id);
      }
    }
    return Object.freeze({
      // The ground in the circle to `height` metres, fading into the ground around over
      // `falloff` metres (default: the radius).
      flatten(center, radius, height, falloff) {
        const at = centerOf(center, 'Terrain.flatten');
        const size = sizeOf(radius, falloff, 'Terrain.flatten');
        const value = heightOf(height, 'Terrain.flatten height');
        return registry.edit(owner, TerrainEdit, 'flatten', at, value, size);
      },
      // The ground in the circle `amount` metres up (down when negative), fading at the edge.
      raise(center, radius, amount, falloff) {
        const at = centerOf(center, 'Terrain.raise');
        const size = sizeOf(radius, falloff, 'Terrain.raise');
        const value = heightOf(amount, 'Terrain.raise amount');
        return registry.edit(owner, TerrainEdit, 'raise', at, value, size);
      },
      // The ground in the circle covered with `material` (a GROUND_MATERIALS name or a surface
      // type index), blended into the ground around over `falloff` metres (default: the radius).
      paint(center, radius, material, falloff) {
        const at = centerOf(center, 'Terrain.paint');
        const size = sizeOf(radius, falloff, 'Terrain.paint');
        const value = materialOf(material);
        // The name and what a game level calls it, for the level to find its own surface type.
        const name =
          typeof material === 'string' ? [material, ...GROUND_NAMES[material]].join('|') : null;
        return registry.edit(owner, TerrainEdit, 'paint', at, value, size, name);
      },
      // The level's grass carpet taken away in the circle (a yard, a floor), until the edit is
      // undone or the resource stops. Trees, bushes and the ground stay.
      hideGrass(center, radius) {
        const at = centerOf(center, 'Terrain.hideGrass');
        const size = sizeOf(radius, 0, 'Terrain.hideGrass');
        if (size.radius === 0) {
          throw new RangeError('Terrain.hideGrass radius is more than 0 m');
        }
        return registry.edit(owner, TerrainEdit, 'grass', at, 0, size);
      },
      // The material names paint takes, with their surface type index.
      materials() {
        return { ...GROUND_MATERIALS };
      },
      // This resource's edits that are still in place, oldest first.
      edits() {
        return [...registry.edits.values()]
          .filter((entry) => entry.owner === owner)
          .map((entry) => entry.handle);
      },
    });
  }

  // The resource stopped: its edits are undone, newest first.
  removeOwner(owner) {
    const ids = [...this.edits]
      .filter(([, entry]) => entry.owner === owner)
      .map(([id]) => id)
      .reverse();
    for (const id of ids) {
      this.undo(id);
    }
  }
}

module.exports = { ResourceTerrain, REACH_MAX, GROUND_MATERIALS };
