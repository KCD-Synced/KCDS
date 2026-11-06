'use strict';

// Ground markers: the server API's Marker. A resource puts a marker
// in the world with Marker.place (a decal, or a cylinder, sphere, chevron, cube or plane mesh), may
// limit it to some players and to a dimension, and with `trigger` on hears markerEnter and
// markerExit when a player walks into or out of it. markerPlace and markerRemove are raised when
// one is drawn or removed.
//
// The server keeps every marker and tells each player's game when it comes to be shown to them,
// changes, or stops being (markers/presence.cjs). The ops sent to a player's client:
//   { op: 'markerShow', id: <player id>, marker, material, shape, position, rotation, size, height,
//     spin, bob, color }  draws the marker, or restates one the client already has under that id
//   { op: 'markerHide', id: <player id>, marker }  takes it away
// Both are sent again for every marker when the player's world is ready (again). The server does
// not cull by distance; a client may leave out markers far from the player.
//
// Enter and exit are decided here from the positions the server already holds, the way areas do
// it (areas/areas.cjs), and reuse its grid: a player is checked only when their position or world
// changed, only against the trigger markers on their grid cell, and there is no timer. A trigger
// volume is a vertical cylinder as wide as the marker (a ball for a sphere) from 1 m under the
// marker up to its height (2 m for a decal or a plane, which have none). A player counts only for
// a marker meant for them in their world, which is what their game would show.
//
// A change of a marker (its trigger, position, size, viewers or world) is settled once, in a
// microtask after the calling code returns, so the events arrive after the handle was returned. No
// exit is raised when the marker is removed or its trigger is turned off, or when the player
// leaves.
//
// Marker.list answers from the game's decal materials (markers/decals.json, made by
// Dev/tools/markers/build_decals.py); Marker.place still takes any material name.
//
// Not covered: the marker has no `state` SharedState.

const { Quaternion } = require('../math.cjs');
const { OWNER, runningResource, removeOwned } = require('../lifecycle/owned_objects.cjs');
const DECAL_MATERIALS = Object.freeze(require('./decals.json'));
const { validWorld } = require('../npcs/npc_values.cjs');
const { AreaGrid } = require('../areas/area_grid.cjs');
const {
  clamp,
  plain,
  plainRotation,
  positionOf,
  sameValue,
  unitRotationOf,
  viewersOf,
  worldOf,
} = require('./values.cjs');

const markerState = Symbol('kcds.markerState');
const SHAPES = ['decal', 'cylinder', 'sphere', 'chevron', 'cube', 'plane'];
const DECAL_FOLDER = 'materials/decals/';
const MATERIAL_MAX = 256;
const SIZE_RANGE = [0.25, 64];
const HEIGHT_RANGE = [0.25, 64];
const SPIN_RANGE = [-720, 720];
const BOB_RANGE = [0, 4];
const DEFAULT_SIZE = 2;
const DEFAULT_HEIGHT = 2;
// The trigger volume reaches this far under the marker, and this high over it when the shape has
// no height of its own.
const TRIGGER_FLOOR = 1;
const TRIGGER_HEIGHT = 2;
// How far a player must have moved from where they were last checked to be checked again.
const MOVE_EPSILON = 0.05;
// The fields whose change can change who is inside.
const TRIGGER_FIELDS = ['position', 'size', 'height', 'trigger'];

function materialOf(value) {
  if (typeof value !== 'string' || value.length === 0 || value.length > MATERIAL_MAX) {
    throw new TypeError(`Marker material takes a string of 1 to ${MATERIAL_MAX} characters`);
  }
  return value;
}

function shapeOf(value) {
  if (!SHAPES.includes(value)) {
    throw new RangeError(`Marker shape takes one of ${SHAPES.join(', ')}`);
  }
  return value;
}

function colorOf(value) {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    throw new RangeError('Marker color takes a packed 0xRRGGBBAA integer');
  }
  return value;
}

function triggerOf(value) {
  if (typeof value !== 'boolean') {
    throw new TypeError('Marker trigger takes true or false');
  }
  return value;
}

// The subfolder of the material under materials/decals/, or '' at the top level.
function groupOf(material) {
  if (!material.startsWith(DECAL_FOLDER)) {
    return '';
  }
  return material.slice(DECAL_FOLDER.length).split('/').slice(0, -1).join('/');
}

// The part of the XY plane a trigger marker touches, for the grid.
function boundsOf(state) {
  const reach = state.size / 2;
  const { x, y } = state.position;
  return { min: { x: x - reach, y: y - reach }, max: { x: x + reach, y: y + reach } };
}

class Markers {
  constructor(host, presence) {
    this.host = host;
    this.presence = presence;
    this.grid = new AreaGrid();
    // marker id -> state, in creation order
    this.byId = new Map();
    // The markers whose trigger is on.
    this.triggers = new Set();
    // player id -> { id, player, x, y, z, world, placed, markers: Set of states }
    this.bodies = new Map();
    // The markers that changed and wait for the settling pass.
    this.dirty = new Set();
    this.settling = false;
    presence.add(this);
  }

  // The states, for the presence.
  objects() {
    return this.byId.values();
  }

  // The handle of the marker with this entity id, or undefined.
  find(id) {
    return this.byId.get(id)?.handle;
  }

  show(player, state) {
    this.host.bridge.send({
      op: 'markerShow',
      id: player.id,
      marker: state.id,
      material: state.material,
      shape: state.shape,
      position: plain(state.position),
      rotation: plainRotation(state.rotation),
      size: state.size,
      height: state.height,
      spin: state.spin,
      bob: state.bob,
      color: state.color,
    });
  }

  hide(player, state) {
    this.host.bridge.send({ op: 'markerHide', id: player.id, marker: state.id });
  }

  // The Marker class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class Marker {
      constructor(id) {
        const handle = registry.find(id);
        if (!handle) {
          throw new Error('Unknown marker');
        }
        return handle;
      }
      get exists() {
        return this[markerState].alive;
      }
      get material() {
        return this[markerState].material;
      }
      get group() {
        return groupOf(this[markerState].material);
      }
      get shape() {
        return this[markerState].shape;
      }
      get size() {
        return this[markerState].size;
      }
      set size(value) {
        registry.requireLive(this);
        registry.change(this[markerState], { size: clamp(value, ...SIZE_RANGE, 'Marker size') });
      }
      get height() {
        return this[markerState].height;
      }
      set height(value) {
        registry.requireLive(this);
        registry.change(this[markerState], {
          height: clamp(value, ...HEIGHT_RANGE, 'Marker height'),
        });
      }
      get spin() {
        return this[markerState].spin;
      }
      set spin(value) {
        registry.requireLive(this);
        registry.change(this[markerState], { spin: clamp(value, ...SPIN_RANGE, 'Marker spin') });
      }
      get bob() {
        return this[markerState].bob;
      }
      set bob(value) {
        registry.requireLive(this);
        registry.change(this[markerState], { bob: clamp(value, ...BOB_RANGE, 'Marker bob') });
      }
      get color() {
        return this[markerState].color;
      }
      set color(value) {
        registry.requireLive(this);
        registry.change(this[markerState], { color: colorOf(value) });
      }
      get trigger() {
        return this[markerState].trigger;
      }
      set trigger(value) {
        registry.requireLive(this);
        registry.change(this[markerState], { trigger: triggerOf(value) });
      }
      get position() {
        return this[markerState].position.clone();
      }
      set position(value) {
        registry.requireLive(this);
        registry.change(this[markerState], { position: positionOf(value, 'Marker.position') });
      }
      get rotation() {
        return this[markerState].rotation.clone();
      }
      set rotation(value) {
        registry.requireLive(this);
        registry.change(this[markerState], { rotation: unitRotationOf(value, 'Marker') });
      }
      get dimension() {
        return this[markerState].world;
      }
      setDimension(world) {
        registry.requireLive(this);
        if (!validWorld(world)) {
          throw new TypeError('Invalid dimension');
        }
        registry.move(this[markerState], world);
      }
      // Limits the marker to a player, to a list of players, or (null) to everyone again.
      showOnlyTo(target) {
        registry.requireLive(this);
        registry.limit(this[markerState], viewersOf(registry.host, target));
      }
      // Raises markerRemove, then removes the marker from every game that shows it.
      remove() {
        if (this[markerState].alive) {
          registry.remove(this[markerState]);
        }
      }
      destroy() {
        this.remove();
      }
      toString() {
        const state = this[markerState];
        return `Marker(${state.id}, ${state.material}, ${state.size} m)`;
      }
      static place(material, position, options, dimension) {
        return registry.place(Marker, material, position, options, dimension);
      }
      // The game's decal materials (markers/decals.json), those starting with `prefix` when given.
      static list(prefix) {
        if (prefix !== undefined && typeof prefix !== 'string') {
          throw new TypeError('Marker.list takes a prefix string');
        }
        const start = (prefix ?? '').toLowerCase();
        return DECAL_MATERIALS.filter((name) => name.startsWith(start));
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
    }
    Object.setPrototypeOf(Marker.prototype, Entity.prototype);
    return Marker;
  }

  requireLive(handle) {
    if (!handle[markerState].alive) {
      throw new Error('The marker was destroyed');
    }
  }

  place(Marker, material, position, options, dimension) {
    const fields = options ?? {};
    if (typeof fields !== 'object') {
      throw new TypeError('Marker.place takes an options object');
    }
    const handle = Object.create(Marker.prototype);
    const state = {
      id: 0,
      handle,
      material: materialOf(material),
      shape: fields.shape === undefined ? 'decal' : shapeOf(fields.shape),
      position: positionOf(position, 'Marker.place'),
      rotation:
        fields.rotation === undefined
          ? new Quaternion(1, 0, 0, 0)
          : unitRotationOf(fields.rotation, 'Marker'),
      size:
        fields.size === undefined ? DEFAULT_SIZE : clamp(fields.size, ...SIZE_RANGE, 'Marker size'),
      height:
        fields.height === undefined
          ? DEFAULT_HEIGHT
          : clamp(fields.height, ...HEIGHT_RANGE, 'Marker height'),
      spin: fields.spin === undefined ? 0 : clamp(fields.spin, ...SPIN_RANGE, 'Marker spin'),
      bob: fields.bob === undefined ? 0 : clamp(fields.bob, ...BOB_RANGE, 'Marker bob'),
      color: fields.color === undefined ? 0 : colorOf(fields.color),
      trigger: fields.trigger === undefined ? false : triggerOf(fields.trigger),
      world: worldOf(dimension, 'Marker.place'),
      viewers: null,
      shownTo: new Set(),
      occupants: new Set(),
      alive: true,
      [OWNER]: runningResource(this.host),
    };
    state.id = this.host.allocateEntityId();
    Object.defineProperty(handle, 'id', { value: state.id, enumerable: true });
    handle[markerState] = state;
    this.byId.set(state.id, state);
    this.regrid(state);
    this.presence.reconcile(this, state);
    this.changed(state);
    this.host.emit('markerPlace', handle);
    return handle;
  }

  // Applies changed fields, restates the marker to the players who see it, and re-decides who is
  // inside when that can have changed.
  change(state, fields) {
    const differs = Object.entries(fields).some(([name, value]) => !sameValue(state[name], value));
    if (!differs) {
      return;
    }
    Object.assign(state, fields);
    if (Object.keys(fields).some((name) => TRIGGER_FIELDS.includes(name))) {
      this.regrid(state);
      this.changed(state);
    }
    this.presence.refresh(this, state);
  }

  move(state, world) {
    if (state.world !== world) {
      state.world = world;
      this.presence.reconcile(this, state);
      this.changed(state);
    }
  }

  limit(state, viewers) {
    state.viewers = viewers;
    this.presence.reconcile(this, state);
    this.changed(state);
  }

  // Enters the marker in the grid under its trigger volume, or takes it out when it has none.
  regrid(state) {
    if (state.trigger) {
      this.triggers.add(state);
      this.grid.add(state, boundsOf(state));
    } else {
      this.triggers.delete(state);
      this.grid.remove(state);
    }
  }

  // The markers a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.byId.values(), name, (state) => this.remove(state));
  }

  // Raises markerRemove while the handle still resolves, then takes the marker away.
  remove(state) {
    this.host.emit('markerRemove', state.handle);
    state.alive = false;
    for (const record of state.occupants) {
      record.markers.delete(state);
    }
    state.occupants.clear();
    this.triggers.delete(state);
    this.grid.remove(state);
    this.dirty.delete(state);
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
      const state = handle[markerState];
      if (state.alive) {
        this.remove(state);
        removed += 1;
      }
    }
    return removed;
  }

  // A marker changed: every player is checked against it once, after the calling code returns.
  changed(state) {
    this.dirty.add(state);
    if (!this.settling) {
      this.settling = true;
      queueMicrotask(() => this.settle());
    }
  }

  settle() {
    this.settling = false;
    const states = [...this.dirty];
    this.dirty.clear();
    for (const state of states) {
      if (state.alive) {
        this.refresh(state);
      }
    }
  }

  // Whether the player stands in the marker's trigger volume and the marker is meant for them.
  holds(state, record) {
    const { x, y, z } = state.position;
    const dx = record.x - x;
    const dy = record.y - y;
    const dz = record.z - z;
    const reach = state.size / 2;
    let inside;
    if (state.shape === 'sphere') {
      inside = dx * dx + dy * dy + dz * dz <= reach * reach;
    } else {
      const top =
        state.shape === 'decal' || state.shape === 'plane' ? TRIGGER_HEIGHT : state.height;
      inside = dx * dx + dy * dy <= reach * reach && dz >= -TRIGGER_FLOOR && dz <= top;
    }
    return inside && this.presence.wanted(state, record.player);
  }

  // Makes one marker's occupants those players who are inside it now. Turning the trigger off
  // lets everyone go without an exit.
  refresh(state) {
    const inside = new Set();
    if (state.trigger) {
      for (const record of this.bodies.values()) {
        if (record.placed && this.holds(state, record)) {
          inside.add(record);
        }
      }
    }
    const exits = [...state.occupants].filter((record) => !inside.has(record));
    const enters = [...inside].filter((record) => !state.occupants.has(record));
    for (const record of exits) {
      this.unlink(state, record);
    }
    for (const record of enters) {
      this.link(state, record);
    }
    if (state.trigger) {
      for (const record of exits) {
        this.host.emit('markerExit', state.handle, record.player);
      }
      for (const record of enters) {
        this.host.emit('markerEnter', state.handle, record.player);
      }
    }
  }

  link(state, record) {
    state.occupants.add(record);
    record.markers.add(state);
  }

  unlink(state, record) {
    state.occupants.delete(record);
    record.markers.delete(state);
  }

  // A player's position or world was learned: the host calls this whenever it does, and the
  // markers raise what changed.
  touch(player, position, world) {
    let record = this.bodies.get(player.id);
    if (!record) {
      record = {
        id: player.id,
        player,
        x: 0,
        y: 0,
        z: 0,
        world,
        placed: false,
        markers: new Set(),
      };
      this.bodies.set(player.id, record);
    }
    const dx = position.x - record.x;
    const dy = position.y - record.y;
    const dz = position.z - record.z;
    const moved = dx * dx + dy * dy + dz * dz > MOVE_EPSILON * MOVE_EPSILON;
    const changedWorld = world !== record.world;
    if (record.placed && !moved && !changedWorld) {
      return;
    }
    record.x = position.x;
    record.y = position.y;
    record.z = position.z;
    record.world = world;
    record.placed = true;
    // With no trigger marker anywhere there is nothing to compare.
    if (this.triggers.size === 0 && record.markers.size === 0) {
      return;
    }
    this.compare(record);
  }

  // Compares where a player is with what they were inside, and raises the exits and then the
  // enters.
  compare(record) {
    const inside = this.containing(record);
    const exits = [];
    for (const state of record.markers) {
      if (!inside.includes(state)) {
        exits.push(state);
      }
    }
    const enters = inside.filter((state) => !record.markers.has(state));
    for (const state of exits) {
      this.unlink(state, record);
    }
    for (const state of enters) {
      this.link(state, record);
    }
    for (const state of exits) {
      this.host.emit('markerExit', state.handle, record.player);
    }
    for (const state of enters) {
      this.host.emit('markerEnter', state.handle, record.player);
    }
  }

  // The trigger markers the player stands in, from the ones on their grid cell.
  containing(record) {
    const found = [];
    const candidates = [...this.grid.near(record.x, record.y), ...this.grid.wide];
    for (const state of candidates) {
      if (state.alive && state.trigger && this.holds(state, record)) {
        found.push(state);
      }
    }
    return found;
  }

  // A player who left the server: the markers lose them and raise nothing.
  forget(id) {
    const record = this.bodies.get(id);
    if (!record) {
      return;
    }
    for (const state of [...record.markers]) {
      this.unlink(state, record);
    }
    this.bodies.delete(id);
  }
}

module.exports = { Markers };
