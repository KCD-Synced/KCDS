'use strict';

// Named patrol routes: the server API's PatrolRoute. A route is a definition a
// resource registers once ({ id, name, points, mode, speed, radius, waitSeconds }) and hands to
// Npc.patrol by handle, id or name. An NPC copies the definition when it is ordered, so updating
// or destroying a route changes only the patrols started after it (npcs/patrols.cjs steps them).
//
// A route registers under a stable `id`, with a `name` lookup, and a route can
// be made from a bare array of points: whichever of id and name is given stands in for the other,
// and a route given neither gets the id `route-<n>`.

const { Vector3 } = require('../math.cjs');
const { OWNER, removeOwned } = require('../lifecycle/owned_objects.cjs');
const {
  PATROL_ROUTE_POINTS_MAX,
  PATROL_MODES,
  SPEEDS,
  utf8Length,
  choiceOf,
  radiusOf,
  waitOf,
  positionOf,
} = require('./npc_values.cjs');

// The longest id and name in UTF-8 bytes.
const ROUTE_ID_MAX_BYTES = 128;
const ROUTE_NAME_MAX_BYTES = 256;

// An id or name: a text of one to `max` bytes without control characters; undefined when left out.
function textOf(value, max, what) {
  if (value === undefined) {
    return undefined;
  }
  if (typeof value !== 'string') {
    throw new TypeError(`${what} must be a string`);
  }
  if (value.length === 0 || utf8Length(value) > max) {
    throw new RangeError(`${what} must be 1 to ${max} bytes`);
  }
  if (/[\x00-\x1f\x7f]/.test(value)) {
    throw new RangeError(`${what} must not hold a line break or control character`);
  }
  return value;
}

// The speed, radius and waitSeconds an object gives, only those that are there. Throws for one
// that cannot be used.
function limitsOf(given, what) {
  const limits = {};
  const speed = choiceOf(given.speed, SPEEDS, undefined, `${what} speed`);
  if (speed !== undefined) {
    limits.speed = speed;
  }
  if (given.radius !== undefined) {
    limits.radius = radiusOf(given.radius, 0, what);
  }
  const waitSeconds = waitOf(given.waitSeconds, undefined, what);
  if (waitSeconds === null) {
    throw new TypeError(`${what} waitSeconds must be a number`);
  }
  if (waitSeconds !== undefined) {
    limits.waitSeconds = waitSeconds;
  }
  return limits;
}

// One point of a route: a PatrolPoint ({ position, speed?, radius?, waitSeconds? }) or a bare
// position. Throws for one that cannot be used.
function pointOf(entry, index) {
  const what = `PatrolRoute point ${index}`;
  if (entry === null || typeof entry !== 'object') {
    throw new TypeError(`${what} must be a position or a PatrolPoint`);
  }
  if (entry.position === undefined) {
    return { position: positionOf(entry, what) };
  }
  return { position: positionOf(entry.position, what), ...limitsOf(entry, what) };
}

// The definition a resource gave, checked and normalised. `id` and `name` stay undefined when left
// out. Throws for an invalid value.
function definitionOf(given) {
  if (given === null || typeof given !== 'object' || Array.isArray(given)) {
    throw new TypeError('PatrolRoute takes a definition object or an array of points');
  }
  if (!Array.isArray(given.points)) {
    throw new TypeError('PatrolRoute points must be an array');
  }
  if (given.points.length === 0 || given.points.length > PATROL_ROUTE_POINTS_MAX) {
    throw new RangeError(`PatrolRoute takes 1 to ${PATROL_ROUTE_POINTS_MAX} points`);
  }
  const definition = {
    id: textOf(given.id, ROUTE_ID_MAX_BYTES, 'PatrolRoute id'),
    name: textOf(given.name, ROUTE_NAME_MAX_BYTES, 'PatrolRoute name'),
    points: given.points.map(pointOf),
    ...limitsOf(given, 'PatrolRoute'),
  };
  const mode = choiceOf(given.mode, PATROL_MODES, undefined, 'PatrolRoute mode');
  if (mode !== undefined) {
    definition.mode = mode;
  }
  return definition;
}

// An independent copy of a stored definition (the points' positions are new Vector3s).
function copyOf(definition) {
  const copy = { ...definition };
  copy.points = definition.points.map((point) => ({
    ...point,
    position: new Vector3(point.position.x, point.position.y, point.position.z),
  }));
  return copy;
}

class PatrolRoutes {
  // `ownerOf` names the resource whose code runs now (null for the host's own work).
  constructor(ownerOf = () => null) {
    this.ownerOf = ownerOf;
    // id -> { definition, handle }; the creation order of the map is the order of all().
    this.records = new Map();
    // The registered definition behind each handle; null once destroyed.
    this.recordOf = new WeakMap();
    this.created = 0;
  }

  // The PatrolRoute class of the server API; every resource gets the same one.
  define() {
    this.PatrolRoute ??= this.makeClass();
    return this.PatrolRoute;
  }

  makeClass() {
    const routes = this;
    const token = Symbol('PatrolRoute');
    class PatrolRoute {
      constructor(key) {
        if (key !== token) {
          throw new TypeError('A PatrolRoute is made with PatrolRoute.create');
        }
      }
      // False after destroy.
      get exists() {
        return routes.recordOf.get(this).definition !== null;
      }
      // The stable id; '' after destroy.
      get id() {
        return routes.recordOf.get(this).definition?.id ?? '';
      }
      // The display name; '' after destroy.
      get name() {
        return routes.recordOf.get(this).definition?.name ?? '';
      }
      // An independent copy of the definition, or null after destroy.
      toJSON() {
        const { definition } = routes.recordOf.get(this);
        return definition === null ? null : copyOf(definition);
      }
      // Removes the definition; a patrol that is running keeps its copy. False when it is gone.
      destroy() {
        return routes.destroy(this);
      }
      // Replaces the fields in `changes` as a whole or not at all; the id cannot change. Throws
      // for a field it cannot use; false after destroy.
      update(changes) {
        return routes.update(this, changes);
      }
      // Registers a route from a definition ({ id?, name?, points, mode?, speed?, radius?,
      // waitSeconds? }) or from an array of points, which takes `name` as its name. Throws for a
      // definition it cannot use or an id that is taken.
      static create(source, name) {
        return routes.create(PatrolRoute, token, source, name);
      }
      // The route registered under this id, or null.
      static getById(id) {
        return routes.records.get(id)?.handle ?? null;
      }
      // The first route registered under this name, or null (a KCDS addition).
      static getByName(name) {
        return routes.named(name)?.handle ?? null;
      }
      // Every registered route, in the order they were made.
      static all() {
        return [...routes.records.values()].map((record) => record.handle);
      }
    }
    return PatrolRoute;
  }

  create(PatrolRoute, token, source, name) {
    const given = Array.isArray(source) ? { points: source, name } : source;
    const definition = definitionOf(given);
    this.created += 1;
    definition.id ??= definition.name ?? `route-${this.created}`;
    definition.name ??= definition.id;
    if (this.records.has(definition.id)) {
      throw new RangeError(`PatrolRoute ${definition.id} exists already`);
    }
    const handle = new PatrolRoute(token);
    const record = { definition, handle, [OWNER]: this.ownerOf() };
    this.records.set(definition.id, record);
    this.recordOf.set(handle, record);
    return handle;
  }

  // The patrol routes a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.records.values(), name, (record) => this.destroy(record.handle));
  }

  destroy(handle) {
    const record = this.recordOf.get(handle);
    if (record.definition === null) {
      return false;
    }
    this.records.delete(record.definition.id);
    record.definition = null;
    return true;
  }

  update(handle, changes) {
    if (changes === null || typeof changes !== 'object') {
      throw new TypeError('PatrolRoute.update takes an object of changes');
    }
    const record = this.recordOf.get(handle);
    if (record.definition === null) {
      return false;
    }
    if (changes.id !== undefined && changes.id !== record.definition.id) {
      throw new RangeError('A PatrolRoute id cannot change');
    }
    const merged = definitionOf({ ...copyOf(record.definition), ...changes });
    record.definition = { ...merged, id: record.definition.id, name: merged.name };
    return true;
  }

  // The record of the first route with this name, or null.
  named(name) {
    for (const record of this.records.values()) {
      if (record.definition.name === name) {
        return record;
      }
    }
    return null;
  }

  // The stored definition Npc.patrol starts from: a handle's own, or that of a route named by id
  // (looked up first) or by name. Null when the route is gone or unknown. Throws for anything
  // else.
  lookup(source) {
    if (typeof source === 'string') {
      return (this.records.get(source) ?? this.named(source))?.definition ?? null;
    }
    const record = source !== null && typeof source === 'object' ? this.recordOf.get(source) : null;
    if (!record) {
      throw new TypeError(
        'patrol takes an array of positions, a PatrolRoute or a route id or name',
      );
    }
    return record.definition;
  }
}

module.exports = { PatrolRoutes };
