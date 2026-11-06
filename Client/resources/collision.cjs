'use strict';

// The client-side Collision (World Architect's Collision layer): invisible solid boxes a
// client script adds in this game only, as an editor's author blocks a cliff edge, closes a gap
// with a floor or lays a ramp. Nobody else has them and nothing goes to the server. The game
// client builds each as an area's collision wall is built (Client/runtime/collision/
// local_collision.hpp):
//
//   { op: 'localCollision', kind: 'set', id, center, half, rotation }
//   { op: 'localCollision', kind: 'remove', id }

const { Quaternion, Vector3, rotation } = require('../../Backend/scripting/runtimes/js/math.cjs');

// The boxes one game holds at most for its scripts.
const MAX_BOXES = 1024;
// A box's sizes, metres.
const MIN_SIZE = 0.02;
const MAX_SIZE = 200;
// How far from the map's middle a box may stand, metres.
const WORLD_LIMIT = 1e6;

const handleState = Symbol('kcds.collisionBox');
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

function sizeOf(value, what) {
  if (!value || !isNumber(value.x) || !isNumber(value.y) || !isNumber(value.z)) {
    throw new TypeError(`${what} takes a Vector3 of sizes in metres`);
  }
  for (const side of [value.x, value.y, value.z]) {
    if (side < MIN_SIZE || side > MAX_SIZE) {
      throw new RangeError(`${what} must be ${MIN_SIZE} to ${MAX_SIZE} metres on each side`);
    }
  }
  return { x: value.x, y: value.y, z: value.z };
}

function turnOf(value) {
  const turn = rotation(value ?? {});
  return { x: turn.x, y: turn.y, z: turn.z, w: turn.w };
}

class ResourceCollision {
  constructor({ send = () => {} } = {}) {
    this.send = send;
    this.nextId = 1;
    // id -> { owner, handle, state: { center, size, rotation } }
    this.boxes = new Map();
  }

  message(id, state) {
    return {
      op: 'localCollision',
      kind: 'set',
      id,
      center: { ...state.center },
      half: { x: state.size.x / 2, y: state.size.y / 2, z: state.size.z / 2 },
      rotation: { ...state.rotation },
    };
  }

  // The Collision class of one resource.
  facade(owner) {
    const registry = this;
    class CollisionBox {
      constructor(token) {
        if (token !== handleState) {
          throw new TypeError('Collision.addBox makes a box');
        }
      }
      get valid() {
        return registry.boxes.get(this.id)?.handle === this;
      }
      // Its middle.
      get center() {
        const { center } = registry.stateOf(this);
        return new Vector3(center.x, center.y, center.z);
      }
      // Its width (x), depth (y) and height (z) in metres, along its own axes.
      get size() {
        const { size } = registry.stateOf(this);
        return new Vector3(size.x, size.y, size.z);
      }
      get rotation() {
        const turn = registry.stateOf(this).rotation;
        return new Quaternion(turn.w, turn.x, turn.y, turn.z);
      }
      // Moves, sizes or turns it in one go; a part left undefined stays as it is.
      set({ center, size, rotation: turn } = {}) {
        registry.change(this, {
          center: center === undefined ? null : pointOf(center, 'CollisionBox.set center'),
          size: size === undefined ? null : sizeOf(size, 'CollisionBox.set size'),
          rotation: turn === undefined ? null : turnOf(turn),
        });
      }
      // Takes it away; safe to repeat.
      remove() {
        registry.remove(this.id);
      }
    }
    return Object.freeze({
      // Adds a solid invisible box, until it is removed or the resource stops.
      addBox(box) {
        if (box === null || typeof box !== 'object') {
          throw new TypeError('Collision.addBox takes { center, size, rotation }');
        }
        const { center, size, rotation: turn } = box;
        const state = {
          center: pointOf(center, 'Collision.addBox center'),
          size: sizeOf(size, 'Collision.addBox size'),
          rotation: turnOf(turn),
        };
        if (registry.boxes.size >= MAX_BOXES) {
          throw new RangeError(`This game holds at most ${MAX_BOXES} collision boxes`);
        }
        const id = registry.nextId;
        registry.nextId += 1;
        const handle = new CollisionBox(handleState);
        Object.defineProperty(handle, 'id', { value: id, enumerable: true });
        registry.boxes.set(id, { owner, handle, state });
        registry.send(registry.message(id, state));
        return handle;
      },
      // The boxes this resource added that still stand.
      all() {
        return [...registry.boxes.values()]
          .filter((entry) => entry.owner === owner)
          .map((entry) => entry.handle);
      },
    });
  }

  stateOf(box) {
    const entry = this.boxes.get(box.id);
    if (entry && entry.handle === box) {
      return entry.state;
    }
    return box[handleState];
  }

  change(box, { center, size, rotation: turn }) {
    const entry = this.boxes.get(box.id);
    if (!entry || entry.handle !== box) {
      return;
    }
    if (center) {
      entry.state.center = center;
    }
    if (size) {
      entry.state.size = size;
    }
    if (turn) {
      entry.state.rotation = turn;
    }
    this.send(this.message(box.id, entry.state));
  }

  remove(id) {
    const entry = this.boxes.get(id);
    if (!entry) {
      return false;
    }
    entry.handle[handleState] = entry.state;
    this.boxes.delete(id);
    this.send({ op: 'localCollision', kind: 'remove', id });
    return true;
  }

  // The resource stopped: its boxes go with it.
  removeOwner(owner) {
    for (const [id, entry] of [...this.boxes]) {
      if (entry.owner === owner) {
        this.remove(id);
      }
    }
  }
}

module.exports = { ResourceCollision, MAX_BOXES };
