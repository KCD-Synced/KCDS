'use strict';

// Level edits: the server's LevelEdit. An edit takes an object the level itself placed
// (a brush, or a level entity) out of the world, moves it, or both, for every player in a virtual
// world. Brushes are identified by mesh and level position, entities by EntityGuid. A layer edit
// turns one of the level's story layers (a place's state in the story) off when `hidden`, on
// otherwise; restoring it gives the layer back as the level has it.
//
// The server keeps the edits in memory only and tells every client with two bridge ops, sent when
// something changes (no timers):
//   { op: 'levelEditApply', id, kind, name, guid, at, hidden, position, rotation, scale, dimension }
//   { op: 'levelEditRemove', id }
// `levelEditApply` states the whole edit, so a later change to the same id restates it. `position`
// and `scale` are { x, y, z } and `rotation` is { x, y, z, w }; each is null while the object has
// not been moved there. `at` is the level's own place of the object, matched within 5 cm.
//
// An edit with a `model` swaps the object for another mesh: every client takes the
// object out (sent as hidden) and the mesh stands where it stood, or where a move put it, as a
// static Prop of the resource that made the edit (its stand-in), turned as the edit's rotation
// says. Clearing the model gives the object back; restoring the edit takes the stand-in away.

const { Quaternion, rotation } = require('../math.cjs');
const { resolveMesh } = require('../props/catalog.cjs');

const GLOBAL_WORLD = 0xffffffff;
const SCALE_MIN = 0.01;
const SCALE_MAX = 100;
const WORLD_LIMIT = 1e6;
// A brush's mesh, as the World Builder shows it.
const MESH = /^objects\/.+\.cgf$/;
const GUID = /^[0-9a-f]{16}$/;
// A layer's name in the level's files: letters, digits, '_', '-' and '.'.
const LAYER = /^[A-Za-z0-9_.-]{1,255}$/;
// Two brushes of the same mesh closer than this are one object.
const MATCH_CM = 5;

function validWorld(world) {
  return Number.isSafeInteger(world) && world >= 0 && world <= GLOBAL_WORLD;
}

// A mesh to stand in an object's place as the props catalog names it, '' for none; throws for a
// mesh the catalog does not carry.
function modelOf(value) {
  if (value === '' || value === null) return '';
  const mesh = typeof value === 'string' ? resolveMesh(value) : null;
  if (!mesh) {
    throw new TypeError(
      `LevelEdit model ${JSON.stringify(value)} is not a mesh of the props catalog`,
    );
  }
  return mesh;
}

// A point from { x, y, z } or [x, y, z]; throws for anything that is not finite and in the world.
function pointOf(value, what) {
  const [x, y, z] = Array.isArray(value) ? value : [value?.x, value?.y, value?.z];
  if (![x, y, z].every((n) => typeof n === 'number' && Math.abs(n) <= WORLD_LIMIT)) {
    throw new TypeError(`LevelEdit ${what} must be a finite Vector3`);
  }
  return { x, y, z };
}

// A rotation from a Quaternion, { x, y, z, w }, [x, y, z, w] or Euler degrees { x, y, z }.
function facingOf(value) {
  let q;
  if (Array.isArray(value)) {
    if (value.length !== 4) throw new TypeError('LevelEdit rotation must have four components');
    q = new Quaternion(value[3], value[0], value[1], value[2]);
  } else {
    if (!value || typeof value !== 'object') throw new TypeError('Invalid LevelEdit rotation');
    q = rotation(value);
  }
  if (![q.x, q.y, q.z, q.w].every(Number.isFinite)) {
    throw new TypeError('LevelEdit rotation must be finite');
  }
  return { x: q.x, y: q.y, z: q.z, w: q.w };
}

// One size or separate sizes, each in the allowed range, as { x, y, z }.
function sizeOf(value) {
  const sizes = typeof value === 'number' ? [value, value, value] : null;
  const [x, y, z] = sizes ?? (Array.isArray(value) ? value : [value?.x, value?.y, value?.z]);
  if (![x, y, z].every((n) => typeof n === 'number' && Number.isFinite(n))) {
    throw new TypeError('LevelEdit scale must be a number or a Vector3');
  }
  const clamp = (n) => Math.min(SCALE_MAX, Math.max(SCALE_MIN, n));
  return { x: clamp(x), y: clamp(y), z: clamp(z) };
}

// What finds the object: the same key is the same edit.
function keyOf(world, kind, name, guid, at) {
  if (kind === 'entity') return `${world}|entity|${guid}`;
  if (kind === 'layer') return `${world}|layer|${name}`;
  const cell = (n) => Math.round((n * 100) / MATCH_CM);
  return `${world}|brush|${name}|${cell(at.x)},${cell(at.y)},${cell(at.z)}`;
}

class LevelEdits {
  // `host`: the resource host (bridge, allocateEntityId).
  constructor(host) {
    this.host = host;
    // id -> state of the live edit
    this.edits = new Map();
    // key -> id, so applying one object twice restates the edit
    this.keys = new Map();
    // id -> the one handle of the edit
    this.handles = new Map();
  }

  // The LevelEdit class of the server API; every resource gets the same one.
  define() {
    this.LevelEdit ??= this.makeClass();
    return this.LevelEdit;
  }

  makeClass() {
    const registry = this;
    class LevelEdit {
      constructor(id) {
        if (!registry.edits.has(id)) {
          throw new Error('Unknown level edit');
        }
        // The same edit is always the same object.
        const known = registry.handles.get(id);
        if (known) return known;
        Object.defineProperty(this, 'id', { value: id, enumerable: true });
        registry.handles.set(id, this);
      }
      get state() {
        return registry.edits.get(this.id) ?? null;
      }
      // False once the edit has been restored.
      get exists() {
        return registry.edits.has(this.id);
      }
      get kind() {
        return this.state?.kind ?? 'brush';
      }
      get name() {
        return this.state?.name ?? '';
      }
      get guid() {
        return this.state?.guid ?? '';
      }
      get dimension() {
        return this.state?.dimension ?? 0;
      }
      get hidden() {
        return this.state?.hidden ?? false;
      }
      set hidden(value) {
        registry.change(this.state, { hidden: Boolean(value) });
      }
      get moved() {
        return this.state?.position != null;
      }
      // The mesh standing in the object's place, or ''. Assigning a mesh the props catalog has
      // replaces the object for all players; '' restores it. Any other value throws.
      get model() {
        return this.state?.model ?? '';
      }
      set model(value) {
        if (this.state?.kind === 'layer') {
          throw new TypeError('A layer edit only turns its layer on or off');
        }
        registry.change(this.state, { model: modelOf(value) });
      }
      // Moves the object for every player; false if the edit is gone, is a layer edit or the
      // transform is invalid.
      move(position, rotation, scale) {
        const state = this.state;
        if (!state || state.kind === 'layer') return false;
        let change;
        try {
          change = { position: pointOf(position, 'position') };
          if (rotation !== undefined) change.rotation = facingOf(rotation);
          if (scale !== undefined) change.scale = sizeOf(scale);
        } catch {
          return false;
        }
        registry.change(state, change);
        return true;
      }
      // Returns the object to its level position without unhiding it.
      clearMove() {
        registry.change(this.state, { position: null, rotation: null, scale: null });
      }
      // Removes the edit and restores the original object for all players.
      restore() {
        registry.restore(this.id);
      }
      // A LevelEdit.apply definition that recreates this edit; null after removal.
      toJSON() {
        return registry.definitionOf(this.state);
      }
      toString() {
        const state = this.state;
        if (!state) return `LevelEdit(${this.id}, gone)`;
        if (state.kind === 'layer') {
          return `LevelEdit(${this.id}, layer, ${state.name}, ${state.hidden ? 'off' : 'on'})`;
        }
        const does = [
          state.hidden ? 'hidden' : '',
          state.position ? 'moved' : '',
          state.model ? 'swapped' : '',
        ].filter(Boolean);
        return `LevelEdit(${this.id}, ${state.kind}, ${state.name || state.guid}, ${
          does.join('+') || 'none'
        })`;
      }
      static apply(definition, dimension) {
        return registry.apply(LevelEdit, definition, dimension);
      }
      static all(dimension) {
        if (dimension !== undefined && !validWorld(dimension)) {
          throw new TypeError('Invalid dimension');
        }
        return [...registry.edits.values()]
          .filter((state) => dimension === undefined || state.dimension === dimension)
          .map((state) => new LevelEdit(state.id));
      }
      static getById(id) {
        return registry.edits.has(id) ? new LevelEdit(id) : null;
      }
      static restoreAll(dimension) {
        return registry.restoreAll(dimension);
      }
    }
    return LevelEdit;
  }

  apply(LevelEdit, definition, dimension) {
    if (!definition || typeof definition !== 'object') {
      throw new TypeError('LevelEdit.apply needs a definition');
    }
    const world = dimension ?? GLOBAL_WORLD;
    if (!validWorld(world)) {
      throw new TypeError('Invalid dimension');
    }
    const kind = definition.kind ?? 'brush';
    let name = '';
    let guid = '';
    if (kind === 'brush') {
      name = String(definition.name ?? '')
        .replace(/\\/g, '/')
        .toLowerCase();
      if (!MESH.test(name))
        throw new TypeError('A brush edit needs its mesh path (objects/...cgf)');
    } else if (kind === 'entity') {
      // As the level files write it ("10cbd99b-495b-4d4d"), the dashes optional.
      guid = String(definition.guid ?? '')
        .replace(/-/g, '')
        .toLowerCase();
      if (!GUID.test(guid)) throw new TypeError('An entity edit needs its guid (16 hex digits)');
      name = String(definition.name ?? '');
    } else if (kind === 'layer') {
      // A story layer by its full name as the level's files write it ("beforeSiege_<guid>").
      name = String(definition.name ?? '');
      if (!LAYER.test(name)) throw new TypeError('A layer edit needs the layer name');
      if (definition.position != null || definition.model) {
        throw new TypeError('A layer edit only turns its layer on or off');
      }
    } else {
      throw new TypeError('LevelEdit kind must be brush, entity or layer');
    }
    const at =
      kind === 'layer' && definition.at == null
        ? { x: 0, y: 0, z: 0 }
        : pointOf(definition.at, 'at');
    const next = {
      model: modelOf(definition.model ?? ''),
      // The stand-in's turn: the definition's rotation, also without a position.
      standFacing: definition.rotation == null ? null : facingOf(definition.rotation),
      hidden: definition.hidden === true,
      position: definition.position == null ? null : pointOf(definition.position, 'position'),
      rotation: definition.rotation == null ? null : facingOf(definition.rotation),
      scale: definition.scale == null ? null : sizeOf(definition.scale),
    };
    const key = keyOf(world, kind, name, guid, at);
    const known = this.keys.get(key);
    if (known !== undefined) {
      // The same object again: restate the edit rather than double it.
      this.change(this.edits.get(known), next);
      return new LevelEdit(known);
    }
    const id = this.host.allocateEntityId();
    const state = { id, kind, name, guid, at, dimension: world, key, standIn: null, ...next };
    this.edits.set(id, state);
    this.keys.set(key, id);
    this.host.bridge.send(this.message(state));
    this.keepStandIn(state);
    return new LevelEdit(id);
  }

  // The stand-in of an edit with a model: made, made again where the edit now puts it, or taken
  // away once the model is cleared.
  keepStandIn(state) {
    const old = state.standIn;
    const where = state.position ?? state.at;
    const facing = state.rotation ?? state.standFacing ?? { x: 0, y: 0, z: 0, w: 1 };
    const scale = state.scale ?? { x: 1, y: 1, z: 1 };
    const wanted = state.model
      ? JSON.stringify([state.model, where, facing, scale, state.dimension])
      : '';
    if (old && old.wanted === wanted && this.host.props.props.get(old.prop.id) === old.prop) {
      return;
    }
    if (old) {
      this.host.props.destroy(old.prop);
      state.standIn = null;
    }
    if (!wanted) {
      return;
    }
    const prop = this.host.api.Prop.spawn(
      state.model,
      where,
      facing,
      scale,
      'static',
      state.dimension,
    );
    state.standIn = { prop, wanted };
  }

  // Sets the changed fields and tells the clients; nothing is sent when nothing differs.
  change(state, fields) {
    if (!state) return;
    const differs = Object.keys(fields).some(
      (name) => JSON.stringify(fields[name]) !== JSON.stringify(state[name]),
    );
    if (!differs) return;
    Object.assign(state, fields);
    this.host.bridge.send(this.message(state));
    this.keepStandIn(state);
  }

  restore(id) {
    const state = this.edits.get(id);
    if (!state) return;
    this.edits.delete(id);
    this.keys.delete(state.key);
    if (state.standIn) {
      this.host.props.destroy(state.standIn.prop);
      state.standIn = null;
    }
    this.host.bridge.send({ op: 'levelEditRemove', id });
  }

  restoreAll(dimension) {
    if (dimension !== undefined && !validWorld(dimension)) {
      throw new TypeError('Invalid dimension');
    }
    let dropped = 0;
    for (const state of [...this.edits.values()]) {
      if (dimension === undefined || state.dimension === dimension) {
        this.restore(state.id);
        dropped += 1;
      }
    }
    return dropped;
  }

  message(state) {
    return {
      op: 'levelEditApply',
      id: state.id,
      kind: state.kind,
      name: state.name,
      guid: state.guid,
      at: state.at,
      // A swapped object is taken out; its stand-in stands where it stood.
      hidden: state.hidden || state.model !== '',
      position: state.model ? null : state.position,
      rotation: state.rotation,
      scale: state.scale,
      dimension: state.dimension,
    };
  }

  definitionOf(state) {
    if (!state) return null;
    const definition = { kind: state.kind };
    if (state.kind === 'brush' || state.kind === 'layer') definition.name = state.name;
    else {
      definition.guid = state.guid;
      if (state.name) definition.name = state.name;
    }
    definition.at = [state.at.x, state.at.y, state.at.z];
    if (state.hidden) definition.hidden = true;
    if (state.model) definition.model = state.model;
    if (state.position) {
      const { x, y, z } = state.position;
      definition.position = [x, y, z];
      if (state.rotation) {
        const r = state.rotation;
        definition.rotation = [r.x, r.y, r.z, r.w];
      }
      if (state.scale) definition.scale = [state.scale.x, state.scale.y, state.scale.z];
    }
    return definition;
  }

  // A player's world became ready: their game lost the edits, so it is told of each again. The op
  // names the player, so nobody else hears it.
  arrived(player) {
    for (const state of this.edits.values()) {
      this.host.bridge.send({ ...this.message(state), to: player.id });
    }
  }
}

module.exports = { LevelEdits };
