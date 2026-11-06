'use strict';

// The client-side World: what this machine's own game can be asked about the level
// around it, answered inside the call. A trace through the world (raycast, traceAll), the ground
// under a point (groundHeight, snapToGround) and the entities inside a sphere (entitiesAround).
// Each is one synchronous request to the game client (sync_channel.cjs), answered on its next
// engine frame by Client/runtime/physics/local_world.hpp:
//
//   { op: 'localWorldRay', from, to, mode, max, ignoreSelf }
//     mode 0 = cover, 1 = anything, 2 = ground -> { status, hits: [{ p, n, d, terrain, surface,
//     kind, model, entity, cls, name, guid }] }
//   { op: 'localWorldEntities', center, radius, cls, max, physicalOnly }
//     -> [{ entity, p, d, cls, name, guid, physical }]
//
// Nothing here is replicated or authority: an answer describes what this client has streamed in
// at one moment. Not covered: a hit's material (always null).

const MODES = Object.freeze({ cover: 0, anything: 1, ground: 2 });
const MAX_RAY_LENGTH = 4096;
const MAX_HITS = 8;
const MAX_RADIUS = 256;
const MAX_ENTITIES = 64;
const MAX_PROBE = 512;
const GROUND_UP = 5;
const GROUND_DOWN = 200;
// How long a script waits for the game's answer (a frame or two, ten while the game loads).
const ANSWER_MS = 2000;

// The game's folders of natural objects, as `category` names them (lowercase, forward slashes).
const {
  categoryOf,
  meshPathOf,
} = require('../../Backend/scripting/runtimes/js/world/mesh_categories.cjs');

function finitePoint(value, where, name) {
  const valid =
    value !== null &&
    typeof value === 'object' &&
    [value.x, value.y, value.z].every((c) => typeof c === 'number' && Number.isFinite(c));
  if (!valid) {
    throw new TypeError(`${where}: ${name} must be a Vector3 of finite numbers`);
  }
  return { x: value.x, y: value.y, z: value.z };
}

function optionsOf(value, where) {
  if (value === undefined) {
    return {};
  }
  if (value === null || typeof value !== 'object') {
    throw new TypeError(`${where}: options must be an object`);
  }
  return value;
}

function modeOf(options, where) {
  const name = options.mode ?? 'cover';
  if (typeof name !== 'string' || !Object.hasOwn(MODES, name)) {
    throw new TypeError(`${where}: mode is 'cover', 'anything' or 'ground'`);
  }
  return MODES[name];
}

function flagOf(options, name, fallback, where) {
  const value = options[name];
  if (value === undefined) {
    return fallback;
  }
  if (typeof value !== 'boolean') {
    throw new TypeError(`${where}: ${name} must be true or false`);
  }
  return value;
}

function countOf(options, name, fallback, max, where) {
  const value = options[name];
  if (value === undefined) {
    return fallback;
  }
  if (!Number.isInteger(value) || value < 1 || value > max) {
    throw new RangeError(`${where}: ${name} must be a whole number from 1 to ${max}`);
  }
  return value;
}

function reachOf(options, name, fallback, where) {
  const value = options[name];
  if (value === undefined) {
    return fallback;
  }
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0 || value > MAX_PROBE) {
    throw new RangeError(`${where}: ${name} must be from 0 to ${MAX_PROBE} metres`);
  }
  return value;
}

function triple(value) {
  return Array.isArray(value) && value.length === 3 && value.every(Number.isFinite);
}

class ResourceWorld {
  // sync: the runtime's SyncChannel; Vector3; playerOf(engine id, owner): the Player handle of the
  // body with that engine id on this machine, or null; emit(name, args): raises an event in every
  // client script.
  constructor({ sync, Vector3, Quaternion, playerOf = () => null, emit = () => {} }) {
    this.sync = sync;
    this.Vector3 = Vector3;
    this.Quaternion = Quaternion;
    this.playerOf = playerOf;
    this.emit = emit;
  }

  // { op: 'weaponHit', hit }: the local player's swing met something solid that is not a body
  // (Client/runtime/weapon_hits), once per thing per attack; hit as localWorldRay's,
  // its distance from the player's feet.
  receiveWeaponHit({ hit: raw }) {
    const hit = this.hitOf(raw);
    if (hit) {
      this.emit('weaponHit', [hit]);
    }
  }

  call(request) {
    return this.sync.call(request, ANSWER_MS);
  }

  // The level object a hit names: { kind, name, guid, class, at, rotation, scale },
  // all fields a LevelEditDefinition uses to locate it; null for other things.
  levelObjectOf(raw) {
    const lo = raw?.lo;
    if (!lo || typeof lo !== 'object' || (lo.kind !== 'brush' && lo.kind !== 'entity')) {
      return null;
    }
    if (!triple(lo.at) || !triple(lo.scale) || !Array.isArray(lo.rot) || lo.rot.length !== 4) {
      return null;
    }
    const { Vector3, Quaternion } = this;
    const brush = lo.kind === 'brush';
    const name = String(lo.name ?? '');
    return Object.freeze({
      kind: lo.kind,
      // A brush's mesh in the form the hit's model has.
      name: brush ? meshPathOf(name) ?? '' : name,
      guid: brush ? undefined : String(lo.guid ?? ''),
      class: brush ? undefined : String(lo.cls ?? ''),
      at: new Vector3(lo.at[0], lo.at[1], lo.at[2]),
      rotation: new Quaternion(lo.rot[3], lo.rot[0], lo.rot[1], lo.rot[2]),
      scale: new Vector3(lo.scale[0], lo.scale[1], lo.scale[2]),
      // A brush its layer hides: an invisible collision of the level's.
      unseen: lo.unseen === true,
    });
  }

  hitOf(raw, owner) {
    if (raw === null || typeof raw !== 'object' || !triple(raw.p) || !triple(raw.n)) {
      return null;
    }
    const { Vector3 } = this;
    const entity = raw.kind === 'entity';
    // In one form, lower case with '/' (as resolveMesh and the props catalog name meshes): the game
    // reports the path as the level spelled it, 'Objects/...' or 'objects/...'.
    const model = meshPathOf(raw.model);
    const engineId = entity && Number.isInteger(raw.entity) && raw.entity > 0 ? raw.entity : null;
    return {
      position: new Vector3(raw.p[0], raw.p[1], raw.p[2]),
      normal: new Vector3(raw.n[0], raw.n[1], raw.n[2]),
      distance: Number(raw.d),
      surface: typeof raw.surface === 'string' ? raw.surface : '',
      terrain: raw.terrain === true,
      kind: typeof raw.kind === 'string' ? raw.kind : 'static',
      model,
      category: categoryOf(model),
      material: null,
      entityGuid: entity && raw.guid ? raw.guid : null,
      entityName: entity ? String(raw.name ?? '') : null,
      entityClass: entity ? String(raw.cls ?? '') : null,
      entityId: engineId,
      player: engineId ? this.playerOf(engineId, owner) : null,
      levelObject: this.levelObjectOf(raw),
    };
  }

  // Every hit along the segment, nearest first, up to `max`.
  trace(from, to, options, max, where, owner) {
    const start = finitePoint(from, where, 'from');
    const end = finitePoint(to, where, 'to');
    const length = Math.hypot(end.x - start.x, end.y - start.y, end.z - start.z);
    if (!(length > 0) || length > MAX_RAY_LENGTH) {
      throw new RangeError(`${where}: the ray must be longer than 0 and at most 4096 metres`);
    }
    const reply = this.call({
      op: 'localWorldRay',
      from: start,
      to: end,
      mode: modeOf(options, where),
      max,
      ignoreSelf: flagOf(options, 'ignoreSelf', true, where),
    });
    if (reply === null || typeof reply !== 'object' || !Array.isArray(reply.hits)) {
      return [];
    }
    const hits = [];
    for (const raw of reply.hits) {
      const hit = this.hitOf(raw, owner);
      if (hit) {
        hits.push(hit);
      }
    }
    return hits;
  }

  raycast(from, to, options, owner) {
    const where = 'World.raycast';
    const given = optionsOf(options, where);
    return this.trace(from, to, given, 1, where, owner)[0] ?? null;
  }

  traceAll(from, to, options, owner) {
    const where = 'World.traceAll';
    const given = optionsOf(options, where);
    const max = countOf(given, 'maxHits', MAX_HITS, MAX_HITS, where);
    return this.trace(from, to, given, max, where, owner);
  }

  snapToGround(position, options, owner, where = 'World.snapToGround') {
    const at = finitePoint(position, where, 'position');
    const given = optionsOf(options, where);
    const up = reachOf(given, 'up', GROUND_UP, where);
    const down = reachOf(given, 'down', GROUND_DOWN, where);
    if (up + down <= 0) {
      return null;
    }
    const from = { x: at.x, y: at.y, z: at.z + up };
    const to = { x: at.x, y: at.y, z: at.z - down };
    return this.trace(from, to, { mode: 'ground' }, 1, where, owner)[0] ?? null;
  }

  groundHeight(position, options, owner) {
    const hit = this.snapToGround(position, options, owner, 'World.groundHeight');
    return hit ? hit.position.z : null;
  }

  entitiesAround(centre, radius, options) {
    const where = 'World.entitiesAround';
    const center = finitePoint(centre, where, 'centre');
    if (typeof radius !== 'number' || !(radius > 0) || radius > MAX_RADIUS) {
      throw new RangeError(`${where}: radius must be above 0 and at most ${MAX_RADIUS} metres`);
    }
    const given = optionsOf(options, where);
    const wanted = given.class ?? '';
    if (typeof wanted !== 'string') {
      throw new TypeError(`${where}: class must be a string`);
    }
    const reply = this.call({
      op: 'localWorldEntities',
      center,
      radius,
      cls: wanted,
      max: countOf(given, 'max', MAX_ENTITIES, MAX_ENTITIES, where),
      physicalOnly: flagOf(given, 'physicalOnly', false, where),
    });
    if (!Array.isArray(reply)) {
      return [];
    }
    const { Vector3 } = this;
    const found = [];
    for (const raw of reply) {
      if (raw === null || typeof raw !== 'object' || !triple(raw.p)) {
        continue;
      }
      found.push({
        position: new Vector3(raw.p[0], raw.p[1], raw.p[2]),
        distance: Number(raw.d),
        guid: raw.guid ? raw.guid : null,
        name: String(raw.name ?? ''),
        class: String(raw.cls ?? ''),
        physicalized: raw.physical === true,
        entityId: Number(raw.entity),
      });
    }
    return found;
  }

  facade(owner) {
    const world = this;
    return Object.freeze({
      raycast(from, to, options) {
        return world.raycast(from, to, options, owner);
      },
      traceAll(from, to, options) {
        return world.traceAll(from, to, options, owner);
      },
      groundHeight(position, options) {
        return world.groundHeight(position, options, owner);
      },
      snapToGround(position, options) {
        return world.snapToGround(position, options, owner);
      },
      entitiesAround(centre, radius, options) {
        return world.entitiesAround(centre, radius, options);
      },
    });
  }
}

module.exports = { ResourceWorld, categoryOf, MAX_RAY_LENGTH };
