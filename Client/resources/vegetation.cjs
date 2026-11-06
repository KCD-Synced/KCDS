'use strict';

// The client-side Vegetation (World Architect): trees, bushes and the like a client script plants
// in this game only, any kind the level has, solid and bending in the wind as the level's own
// (Client/runtime/vegetation/local_vegetation.hpp). Nobody else sees them and nothing goes to the
// server. The game answers at once through the sync channel:
//
//   { op: 'localVegetationKinds' }  -> { kinds: [mesh path, ...] }
//   { op: 'localVegetationPlant', kind or mesh, x, y, z, scale, turn } -> { id, kind, mesh } or null
//
// A kind named by its mesh path or file name goes to the game as `mesh` and the game finds it:
// one question per plant (asking for the kinds list first made Add nature lag).
//   { op: 'localVegetationRemove', id } -> true or false
//   { op: 'localVegetationPlantMany', plants: [plant question, ...] } -> [{ id, kind, mesh } or null]
//   { op: 'localVegetationRemoveMany', ids: [id, ...] } -> [true or false, ...]
//
// A resource's plants are taken away when it stops.

const ANSWER_MS = 2000;
const SCALE_MIN = 1 / 64;
const SCALE_MAX = 255 / 64;
const WORLD_LIMIT = 1e6;
const handleState = Symbol('kcds.plant');
// Most plants of one plantMany question (the game takes as many).
const MANY_MOST = 256;

const isNumber = (value) => typeof value === 'number' && Number.isFinite(value);

function pointOf(position) {
  if (!position || !isNumber(position.x) || !isNumber(position.y) || !isNumber(position.z)) {
    throw new TypeError('Vegetation.plant takes a position with x, y and z');
  }
  if (Math.max(Math.abs(position.x), Math.abs(position.y), Math.abs(position.z)) > WORLD_LIMIT) {
    throw new RangeError('Vegetation.plant position is out of the world');
  }
  return { x: position.x, y: position.y, z: position.z };
}

function optionsOf(options) {
  const given = options ?? {};
  if (typeof given !== 'object') {
    throw new TypeError('Vegetation.plant options are { scale, turn }');
  }
  const scale = given.scale ?? 1;
  const turn = given.turn ?? 0;
  if (!isNumber(scale) || scale < SCALE_MIN || scale > SCALE_MAX) {
    throw new RangeError(`Vegetation.plant scale is ${SCALE_MIN} to ${SCALE_MAX}`);
  }
  if (!isNumber(turn)) {
    throw new TypeError('Vegetation.plant turn is a number of degrees');
  }
  return { scale, turn };
}

const stemOf = (path) => path.slice(path.lastIndexOf('/') + 1).replace(/\.cgf$/, '');

class ResourceVegetation {
  constructor({ sync } = {}) {
    this.sync = sync;
    // id -> { owner, handle }
    this.plants = new Map();
  }

  call(request) {
    return this.sync ? this.sync.call(request, ANSWER_MS) : null;
  }

  // The level's vegetation kinds, a kind's index its place in the list; [] outside a world.
  kinds() {
    const reply = this.call({ op: 'localVegetationKinds' });
    if (!reply || !Array.isArray(reply.kinds)) {
      return [];
    }
    return reply.kinds.map((path) => (typeof path === 'string' ? path.toLowerCase() : ''));
  }

  // The index in `kinds` of a kind named by its index, its mesh path or its file name; -1 when the
  // level has none of that name.
  indexOf(kind, kinds) {
    if (Number.isInteger(kind)) {
      return kind >= 0 && kind < kinds.length && kinds[kind] !== '' ? kind : -1;
    }
    if (typeof kind !== 'string' || kind === '') {
      throw new TypeError('Vegetation.plant takes a kind: its index, mesh path or file name');
    }
    const name = kind.replace(/\\/g, '/').toLowerCase();
    const exact = kinds.indexOf(name);
    if (exact >= 0) {
      return exact;
    }
    return kinds.findIndex((path) => path !== '' && stemOf(path) === stemOf(name));
  }

  // What a plant question names its kind by: { mesh } for a path or file name (the game finds
  // it), { kind } for an index; null for an index the level does not have.
  kindQuestion(kind) {
    if (Number.isInteger(kind)) {
      const index = this.indexOf(kind, this.kinds());
      return index < 0 ? null : { kind: index };
    }
    if (typeof kind !== 'string' || kind === '') {
      throw new TypeError('Vegetation.plant takes a kind: its index, mesh path or file name');
    }
    return { mesh: kind.replace(/\\/g, '/') };
  }

  remove(id) {
    if (!this.plants.delete(id)) {
      return false;
    }
    return this.call({ op: 'localVegetationRemove', id }) === true;
  }

  // A handle for a plant the game answered { id, mesh } for, kept as `owner`'s.
  adopt(owner, reply) {
    const registry = this;
    if (!this.Plant) {
      this.Plant = class Plant {
        constructor(token) {
          if (token !== handleState) {
            throw new TypeError('Vegetation.plant plants');
          }
        }
        // False once removed or its resource stopped.
        get valid() {
          return registry.plants.get(this.id)?.handle === this;
        }
        // Takes it away; false when it already was.
        remove() {
          return this.valid && registry.remove(this.id);
        }
      };
    }
    const plant = new this.Plant(handleState);
    const path = typeof reply.mesh === 'string' ? reply.mesh.toLowerCase() : '';
    Object.defineProperty(plant, 'id', { value: reply.id, enumerable: true });
    Object.defineProperty(plant, 'kind', { value: path, enumerable: true });
    this.plants.set(reply.id, { owner, handle: plant });
    return plant;
  }

  // Many plants in one question to the game (World Architect's nature brush and projects; not
  // in the public Vegetation): [{ kind, position, scale, turn }] -> a handle or null each.
  plantMany(owner, list) {
    const plants = list.slice(0, MANY_MOST).map((item) => {
      const at = pointOf(item.position);
      const { scale, turn } = optionsOf({ scale: item.scale, turn: item.turn });
      return { ...this.kindQuestion(item.kind), ...at, scale, turn };
    });
    const reply = this.call({ op: 'localVegetationPlantMany', plants });
    if (!Array.isArray(reply)) {
      return plants.map(() => null);
    }
    return plants.map((_, index) => {
      const answer = reply[index];
      return answer && Number.isInteger(answer.id) ? this.adopt(owner, answer) : null;
    });
  }

  // Many plants taken away in one question (World Architect's undo); how many went.
  removeMany(handles) {
    const ids = handles.filter((plant) => plant?.valid).map((plant) => plant.id);
    for (const id of ids) {
      this.plants.delete(id);
    }
    if (ids.length === 0) {
      return 0;
    }
    const reply = this.call({ op: 'localVegetationRemoveMany', ids });
    return Array.isArray(reply) ? reply.filter((gone) => gone === true).length : 0;
  }

  // The Vegetation object of one resource.
  facade(owner) {
    const registry = this;
    return Object.freeze({
      kinds() {
        return registry.kinds();
      },
      // Plants a kind at the position; null when the game could not (not in a world, a kind the
      // level does not have).
      plant(kind, position, options) {
        const at = pointOf(position);
        const { scale, turn } = optionsOf(options);
        const named = registry.kindQuestion(kind);
        if (!named) {
          return null;
        }
        const reply = registry.call({
          op: 'localVegetationPlant',
          ...named,
          ...at,
          scale,
          turn,
        });
        if (!reply || !Number.isInteger(reply.id)) {
          return null;
        }
        return registry.adopt(owner, reply);
      },
      // Many plants at once, `MANY_MOST` to a question to the game (one plant a question waits a
      // frame each: 4096 plants of a World Architect export took two minutes and held the
      // player's keys up): [{ kind, position, scale?, turn? }] -> a Plant or
      // null each, in the list's order.
      plantMany(list) {
        if (!Array.isArray(list)) {
          throw new TypeError(
            'Vegetation.plantMany takes a list of { kind, position, scale, turn }',
          );
        }
        const handles = [];
        for (let first = 0; first < list.length; first += MANY_MOST) {
          handles.push(...registry.plantMany(owner, list.slice(first, first + MANY_MOST)));
        }
        return handles;
      },
      // This resource's plants that still stand.
      planted() {
        return [...registry.plants.values()]
          .filter((entry) => entry.owner === owner)
          .map((entry) => entry.handle);
      },
    });
  }

  // The resource stopped: its plants are taken away.
  removeOwner(owner) {
    for (const [id, entry] of [...this.plants]) {
      if (entry.owner === owner) {
        this.remove(id);
      }
    }
  }
}

module.exports = { ResourceVegetation, SCALE_MIN, SCALE_MAX };
