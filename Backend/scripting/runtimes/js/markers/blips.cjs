'use strict';

// Blips: the server API's Blip, an icon on the compass and the map
// screen of the players it is meant for. A resource makes one with Blip.create and moves, renames
// or removes it; the server keeps it and tells each player's game when it comes to be shown to
// them, changes, or stops being (markers/presence.cjs). A blip is not tied to distance: it is sent
// to every player it is meant for, however far away they are.
//
// The ops sent to a player's client:
//   { op: 'blipShow', id: <player id>, blip, type, markState, label, compass, map, position }
//     draws the blip, or restates one the client already has under that blip id
//   { op: 'blipHide', id: <player id>, blip }  takes it away
// Both are sent again for every blip when the player's world is ready (again).
//
// Not covered: the blip has no `state` SharedState.

const fs = require('node:fs');
const { OWNER, runningResource, removeOwned } = require('../lifecycle/owned_objects.cjs');
const path = require('node:path');
const { BLIP_TYPES, DEFAULT_BLIP_TYPE, isBlipType } = require('./blip_types.cjs');
const { iconFits } = require('../inventory/custom_items.cjs');
const { clientFileSet } = require('../manifest.cjs');
const { labelOf, plain, positionOf, sameValue, viewersOf, worldOf } = require('./values.cjs');
const { validWorld } = require('../npcs/npc_values.cjs');

const blipState = Symbol('kcds.blipState');

function flagOf(value, name) {
  if (typeof value !== 'boolean') {
    throw new TypeError(`${name} takes true or false`);
  }
  return value;
}

function markStateOf(value) {
  if (!Number.isInteger(value)) {
    throw new TypeError('Blip markState takes an integer');
  }
  return value;
}

class Blips {
  constructor(host, presence) {
    this.host = host;
    this.presence = presence;
    // blip id -> state, in creation order
    this.byId = new Map();
    presence.add(this);
  }

  // The states, for the presence.
  objects() {
    return this.byId.values();
  }

  // The handle of the blip with this entity id, or undefined.
  find(id) {
    return this.byId.get(id)?.handle;
  }

  show(player, state) {
    const message = {
      op: 'blipShow',
      id: player.id,
      blip: state.id,
      type: state.type,
      markState: state.markState,
      label: state.label,
      compass: state.compass,
      map: state.map,
      position: plain(state.position),
    };
    if (state.imagePath) {
      message.image = { resource: state.imageResource, path: state.imagePath };
    }
    this.host.bridge.send(message);
  }

  // KCDS: a blip's own icon, a .dds the calling resource ships to clients ('' for none):
  // { imageResource, imagePath }, both '' for none.
  imageOf(file) {
    if (file === '') {
      return { imageResource: '', imagePath: '' };
    }
    const owner = this.host.context?.getStore();
    const definition = owner ? this.host.manifests?.get(owner.name) : null;
    if (typeof file !== 'string' || !definition) {
      throw new TypeError('Blip image is the path of a .dds the resource ships to clients');
    }
    if (!clientFileSet(definition.folder, definition.manifest).has(file)) {
      throw new TypeError(`Blip image: ${owner.name} does not ship ${file} to clients`);
    }
    if (!iconFits(fs.readFileSync(path.join(definition.folder, file)))) {
      throw new TypeError(
        'Blip image: a .dds of 64 x 64, or an uncompressed 32-bit one clients scale to it',
      );
    }
    return { imageResource: owner.name, imagePath: file };
  }

  hide(player, state) {
    this.host.bridge.send({ op: 'blipHide', id: player.id, blip: state.id });
  }

  // The Blip class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class Blip {
      constructor(id) {
        const handle = registry.find(id);
        if (!handle) {
          throw new Error('Unknown blip');
        }
        return handle;
      }
      get exists() {
        return this[blipState].alive;
      }
      get type() {
        return this[blipState].type;
      }
      set type(value) {
        registry.requireLive(this);
        // Unknown names have no effect on the blip.
        if (typeof value === 'string' && isBlipType(value)) {
          registry.change(this[blipState], { type: value });
        }
      }
      get markState() {
        return this[blipState].markState;
      }
      set markState(value) {
        registry.requireLive(this);
        registry.change(this[blipState], { markState: markStateOf(value) });
      }
      get label() {
        return this[blipState].label;
      }
      set label(value) {
        registry.requireLive(this);
        registry.change(this[blipState], { label: labelOf(value) });
      }
      get compass() {
        return this[blipState].compass;
      }
      set compass(value) {
        registry.requireLive(this);
        registry.change(this[blipState], { compass: flagOf(value, 'Blip compass') });
      }
      get map() {
        return this[blipState].map;
      }
      set map(value) {
        registry.requireLive(this);
        registry.change(this[blipState], { map: flagOf(value, 'Blip map') });
      }
      // KCDS: the .dds its resource ships that is its own icon on the map and the compass, ''
      // for its type's.
      get image() {
        return this[blipState].imagePath;
      }
      set image(value) {
        registry.requireLive(this);
        registry.change(this[blipState], registry.imageOf(value));
      }
      get position() {
        return this[blipState].position.clone();
      }
      set position(value) {
        registry.requireLive(this);
        registry.change(this[blipState], { position: positionOf(value, 'Blip.position') });
      }
      get dimension() {
        return this[blipState].world;
      }
      setDimension(world) {
        registry.requireLive(this);
        if (!validWorld(world)) {
          throw new TypeError('Invalid dimension');
        }
        registry.move(this[blipState], world);
      }
      // Limits the blip to a player, to a list of players, or (null) to everyone again.
      showOnlyTo(target) {
        registry.requireLive(this);
        registry.limit(this[blipState], viewersOf(registry.host, target));
      }
      remove() {
        if (this[blipState].alive) {
          registry.remove(this[blipState]);
        }
      }
      destroy() {
        this.remove();
      }
      toString() {
        const state = this[blipState];
        return `Blip(${state.id}, ${state.type}, ${state.label})`;
      }
      static create(options) {
        return registry.create(Blip, options);
      }
      static all(dimension) {
        return registry.list(dimension);
      }
      static getById(id) {
        return registry.find(id) ?? null;
      }
      static removeAll(dimension) {
        return registry.removeAll(dimension);
      }
      static types() {
        return [...BLIP_TYPES];
      }
    }
    Object.setPrototypeOf(Blip.prototype, Entity.prototype);
    return Blip;
  }

  requireLive(handle) {
    if (!handle[blipState].alive) {
      throw new Error('The blip was destroyed');
    }
  }

  create(Blip, options) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('Blip.create takes an options object');
    }
    const position = positionOf(options.position, 'Blip.create');
    if (options.position === undefined) {
      throw new TypeError('Blip.create takes a position');
    }
    const type = options.type ?? DEFAULT_BLIP_TYPE;
    if (typeof type !== 'string' || !isBlipType(type)) {
      throw new RangeError('Blip.create takes one of the names Blip.types() lists');
    }
    const markState = options.markState === undefined ? 0 : markStateOf(options.markState);
    const label = options.label === undefined ? '' : labelOf(options.label);
    const compass = options.compass === undefined ? true : flagOf(options.compass, 'Blip compass');
    const map = options.map === undefined ? true : flagOf(options.map, 'Blip map');
    const world = worldOf(options.dimension, 'Blip.create');
    const viewers = options.player === undefined ? null : viewersOf(this.host, options.player);
    const image =
      options.image === undefined
        ? { imageResource: '', imagePath: '' }
        : this.imageOf(options.image);
    const handle = Object.create(Blip.prototype);
    Object.defineProperty(handle, 'id', { value: this.host.allocateEntityId(), enumerable: true });
    const state = {
      id: handle.id,
      handle,
      type,
      markState,
      label,
      compass,
      map,
      position,
      ...image,
      world,
      viewers,
      shownTo: new Set(),
      alive: true,
      [OWNER]: runningResource(this.host),
    };
    handle[blipState] = state;
    this.byId.set(state.id, state);
    this.presence.reconcile(this, state);
    return handle;
  }

  // Applies changed fields and restates the blip to the players who see it.
  change(state, fields) {
    const differs = Object.entries(fields).some(([name, value]) => !sameValue(state[name], value));
    if (!differs) {
      return;
    }
    Object.assign(state, fields);
    this.presence.refresh(this, state);
  }

  move(state, world) {
    if (state.world !== world) {
      state.world = world;
      this.presence.reconcile(this, state);
    }
  }

  limit(state, viewers) {
    state.viewers = viewers;
    this.presence.reconcile(this, state);
  }

  // The blips a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.byId.values(), name, (state) => this.remove(state));
  }

  remove(state) {
    state.alive = false;
    this.presence.withdraw(this, state);
    this.byId.delete(state.id);
  }

  list(dimension) {
    const found = [];
    for (const state of this.byId.values()) {
      if (dimension === undefined || state.world === dimension) {
        found.push(state.handle);
      }
    }
    return found;
  }

  removeAll(dimension) {
    if (dimension !== undefined && !validWorld(dimension)) {
      throw new TypeError('Invalid dimension');
    }
    let removed = 0;
    for (const handle of this.list(dimension)) {
      this.remove(handle[blipState]);
      removed += 1;
    }
    return removed;
  }
}

module.exports = { Blips };
