'use strict';

// The client-side Prefab (World Architect): the game's own prefabs (a house
// with its doors, a wall, a well) a client script builds in this game only, as an editor places
// them while its author works. Nobody else sees them and nothing goes to the server. The game
// client builds them with the game's own prefab spawner (Client/runtime/prefabs/game_prefabs.hpp),
// solid, as the server's Prop.spawn('prefab:...') does for everyone:
//
//   { op: 'localPrefab', kind: 'spawn' | 'move', id, template, position, rotation }
//   { op: 'localPrefab', kind: 'destroy', id }
//
// A prefab is named by its path under the game's Prefabs/ folder or its file name, from the same
// catalog the server uses (Backend/scripting/runtimes/js/props/prefabs.json). Ids come from
// FIRST_ID up, a range the server never gives.

const {
  resolvePrefab,
  prefabNames,
} = require('../../Backend/scripting/runtimes/js/props/catalog.cjs');
const { Quaternion, Vector3, rotation } = require('../../Backend/scripting/runtimes/js/math.cjs');

const FIRST_ID = 0xe0000000;
const LAST_ID = 0xefffffff;
// The prefabs one game holds at most for its scripts (a village's worth).
const MAX_PREFABS = 2048;
// How far from the map's middle a prefab may stand, metres.
const WORLD_LIMIT = 1e6;
// The most names find() returns.
const FIND_MAX = 200;

const handleState = Symbol('kcds.prefab');
const isNumber = (value) => typeof value === 'number' && Number.isFinite(value);

function pointOf(value, what) {
  if (!value || !isNumber(value.x) || !isNumber(value.y) || !isNumber(value.z)) {
    throw new TypeError(`${what} takes a Vector3`);
  }
  if (Math.max(Math.abs(value.x), Math.abs(value.y), Math.abs(value.z)) > WORLD_LIMIT) {
    throw new RangeError(`${what} is out of the world`);
  }
  return { x: value.x, y: value.y, z: value.z };
}

function turnOf(value) {
  const turn = rotation(value ?? {});
  return { x: turn.x, y: turn.y, z: turn.z, w: turn.w };
}

// The catalog path and template GUID of a prefab name; throws for one the game cannot build.
function templateOf(name) {
  if (typeof name !== 'string' || name === '') {
    throw new TypeError("Prefab.place takes a prefab's name");
  }
  const found = resolvePrefab(name);
  if (found === null) {
    throw new RangeError(`the prefab catalog does not carry ${name}`);
  }
  if (found.blocked) {
    throw new RangeError(`the game cannot build the prefab ${found.path}: it ${found.blocked}`);
  }
  return found;
}

class ResourcePrefabs {
  constructor({ send = () => {} } = {}) {
    this.send = send;
    this.nextId = FIRST_ID;
    // id -> { owner, handle, state: { path, guid, position, rotation } }
    this.prefabs = new Map();
  }

  allocate() {
    const id = this.nextId;
    this.nextId = this.nextId === LAST_ID ? FIRST_ID : this.nextId + 1;
    return id;
  }

  message(kind, id, state) {
    return {
      op: 'localPrefab',
      kind,
      id,
      template: state.guid,
      position: { ...state.position },
      rotation: { ...state.rotation },
    };
  }

  // The Prefab class of one resource.
  facade(owner) {
    const registry = this;
    class Prefab {
      constructor(token) {
        if (token !== handleState) {
          throw new TypeError('Prefab.place builds a prefab');
        }
      }
      // Builds the prefab in this game, solid, until destroy() or the resource stops.
      static place(name, position, orientation) {
        const found = templateOf(name);
        const state = {
          path: found.path,
          guid: found.guid,
          position: pointOf(position, 'Prefab.place position'),
          rotation: turnOf(orientation),
        };
        if (registry.prefabs.size >= MAX_PREFABS) {
          throw new RangeError(`This game holds at most ${MAX_PREFABS} placed prefabs`);
        }
        const id = registry.allocate();
        const prefab = new Prefab(handleState);
        Object.defineProperty(prefab, 'id', { value: id, enumerable: true });
        registry.prefabs.set(id, { owner, handle: prefab, state });
        registry.send(registry.message('spawn', id, state));
        return prefab;
      }
      // The names of the prefabs the game can build whose path holds `text` (any case), at most
      // `max` (default and most 200), in catalog order.
      static find(text, max = FIND_MAX) {
        if (typeof text !== 'string') {
          throw new TypeError('Prefab.find takes a text');
        }
        const limit = Math.min(FIND_MAX, Math.max(0, Math.floor(isNumber(max) ? max : FIND_MAX)));
        const needle = text.toLowerCase();
        const found = [];
        for (const path of prefabNames()) {
          if (found.length >= limit) {
            break;
          }
          if (path.includes(needle)) {
            found.push(path);
          }
        }
        return found;
      }
      // The prefabs this resource placed that still stand.
      static all() {
        return [...registry.prefabs.values()]
          .filter((entry) => entry.owner === owner)
          .map((entry) => entry.handle);
      }
      get valid() {
        return registry.prefabs.get(this.id)?.handle === this;
      }
      // Its path under the game's Prefabs/ folder.
      get name() {
        return registry.stateOf(this).path;
      }
      get position() {
        const { position } = registry.stateOf(this);
        return new Vector3(position.x, position.y, position.z);
      }
      set position(value) {
        registry.move(this, pointOf(value, 'Prefab position'), null);
      }
      get rotation() {
        const turn = registry.stateOf(this).rotation;
        return new Quaternion(turn.w, turn.x, turn.y, turn.z);
      }
      set rotation(value) {
        registry.move(this, null, turnOf(value));
      }
      // Moves it in one go: the game builds it again there.
      setPose(position, orientation) {
        const turn = orientation === undefined ? null : turnOf(orientation);
        registry.move(this, pointOf(position, 'Prefab.setPose position'), turn);
      }
      // Takes it away; safe to repeat.
      destroy() {
        registry.destroy(this.id);
      }
    }
    return Prefab;
  }

  stateOf(prefab) {
    const entry = this.prefabs.get(prefab.id);
    if (entry && entry.handle === prefab) {
      return entry.state;
    }
    return prefab[handleState];
  }

  move(prefab, position, turn) {
    const entry = this.prefabs.get(prefab.id);
    if (!entry || entry.handle !== prefab) {
      return;
    }
    if (position) entry.state.position = position;
    if (turn) entry.state.rotation = turn;
    this.send(this.message('move', prefab.id, entry.state));
  }

  destroy(id) {
    const entry = this.prefabs.get(id);
    if (!entry) {
      return false;
    }
    entry.handle[handleState] = entry.state;
    this.prefabs.delete(id);
    this.send({ op: 'localPrefab', kind: 'destroy', id });
    return true;
  }

  // The resource stopped: its prefabs go with it.
  removeOwner(owner) {
    for (const [id, entry] of [...this.prefabs]) {
      if (entry.owner === owner) {
        this.destroy(id);
      }
    }
  }
}

module.exports = { ResourcePrefabs, FIRST_ID, MAX_PREFABS };
