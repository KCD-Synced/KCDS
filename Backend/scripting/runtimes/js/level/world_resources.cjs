'use strict';

// World resources: the server's WorldResource. A world export is a JSON file the owner
// lists in the server settings; the host loads each one once, after the resources have started, and
// spawns what it lists through the same APIs a script would use (Prop, Vfx, Light, LevelEdit,
// GroundItem, Door, SmartObject, Area and PatrolRoute). The exports belong to the server, not to a
// resource, so they live until shutdown.
//
// File format (`<name>.world.json`; every list is optional):
//   {
//     "level": "<the level the export was made in>",
//     "props":        [ { model, position, rotation?, scale?, physics?, dimension? } ],
//     "effects":      [ { effect, position, rotation?, ...Vfx options, dimension? } ],
//     "lights":       [ { position, color?, intensity?, radius?, castShadows? } ],
//     "world":        [ LevelEditDefinition ],
//     "items":        [ { item, position, rotation?, amount?, name?, quality?, health?, condition?,
//                         dimension? } ]   stacks laid on the ground (GroundItem)
//     "doors":        [ { ...DoorDefinition, dimension? } ]   doors hung where the level has
//                         none (Door.spawn)
//     "smartObjects": [ { blueprint, position, rotation?, name?, dimension? } ]   the game's seats,
//                         beds, tables and stations (SmartObject.spawn)
//     "areas":        [ Area.create definition ],
//     "patrolRoutes": [ PatrolRoute.create definition ]
//   }
// `rotation` is a quaternion or Euler degrees, as the API takes it. The handles of what was spawned
// are kept in export order; anything a script destroys later drops out of the lists.

const fs = require('node:fs');
const path = require('node:path');

const NAME = /^[A-Za-z0-9_.-]+$/;

// The entries of `list`, which must be an array of objects when present.
function entriesOf(file, field, list) {
  if (list === undefined) {
    return [];
  }
  if (!Array.isArray(list) || list.some((entry) => entry === null || typeof entry !== 'object')) {
    throw new TypeError(`${file}: "${field}" must be an array of objects`);
  }
  return list;
}

class WorldResources {
  // `host`: the resource host (api, bridge, emit).
  constructor(host) {
    this.host = host;
    // name -> record { name, path, level, props, effects, lights, edits, items, doors, smartObjects,
    // areas,
    // routes }
    this.records = new Map();
    this.ready = false;
  }

  // The WorldResource class of the server API; every resource gets the same one.
  define() {
    this.WorldResource ??= this.makeClass();
    return this.WorldResource;
  }

  makeClass() {
    const registry = this;
    const handles = new Map();
    class WorldResource {
      constructor(token) {
        if (token !== registry) {
          throw new TypeError('A WorldResource is found with WorldResource.all or find');
        }
      }
      get record() {
        return registry.records.get(this.state.name) === this.state ? this.state : null;
      }
      get name() {
        return this.record?.name ?? '';
      }
      get path() {
        return this.record?.path ?? '';
      }
      get level() {
        return this.record?.level ?? '';
      }
      get loaded() {
        return this.record !== null;
      }
      // Fresh arrays of what survives, in export order.
      get props() {
        return this.record
          ? this.record.props.filter((prop) => registry.host.props.props.get(prop.id) === prop)
          : [];
      }
      get effects() {
        return this.record
          ? this.record.effects.filter((vfx) => registry.host.vfx.effects.get(vfx.id) === vfx)
          : [];
      }
      get lights() {
        return this.record
          ? this.record.lights.filter(
              (light) => registry.host.lights.lights.get(light.id) === light,
            )
          : [];
      }
      get levelEdits() {
        return this.record ? this.record.edits.filter((edit) => edit.exists) : [];
      }
      // Ground items from the export that are still there, in file order.
      get items() {
        return this.record
          ? this.record.items.filter(
              (stack) => registry.host.groundItems.stacks.get(stack.id) === stack,
            )
          : [];
      }
      // The doors the export hangs that nobody destroyed, in export order.
      get doors() {
        return this.record
          ? this.record.doors.filter(
              (door) => registry.host.doors.hungByGuid.get(door.guid) === door,
            )
          : [];
      }
      // Remaining seats, beds, tables and stations from the export, not yet destroyed, in export
      // order.
      get smartObjects() {
        return this.record
          ? this.record.smartObjects.filter(
              (object) => registry.host.smartObjects.placed.get(object.id) === object,
            )
          : [];
      }
      get areas() {
        return this.record ? this.record.areas.filter((area) => area.exists) : [];
      }
      get patrolRoutes() {
        return this.record ? this.record.routes.filter((route) => route.exists) : [];
      }
      static get ready() {
        return registry.ready;
      }
      static all() {
        return [...registry.records.values()].map((record) => handleOf(record));
      }
      static find(name) {
        if (typeof name !== 'string') {
          throw new TypeError('WorldResource.find takes a name');
        }
        const record = registry.records.get(name);
        return record ? handleOf(record) : null;
      }
    }
    function handleOf(record) {
      let handle = handles.get(record);
      if (!handle) {
        handle = new WorldResource(registry);
        handle.state = record;
        handles.set(record, handle);
      }
      return handle;
    }
    return WorldResource;
  }

  // Loads every export in `entries` (a file path, or { name, path }) in order, then raises
  // worldResourcesReady. A file that fails to load is logged and the rest still load; ready is
  // true only when all of them did.
  load(entries) {
    let allLoaded = true;
    for (const entry of entries) {
      try {
        this.loadOne(entry);
      } catch (error) {
        allLoaded = false;
        this.host.bridge.log('host', `World resource ${entry?.path ?? entry}: ${error.message}`);
      }
    }
    if (allLoaded) {
      this.ready = true;
      this.host.emit('worldResourcesReady');
    }
    return allLoaded;
  }

  loadOne(entry) {
    const file = path.resolve(typeof entry === 'string' ? entry : entry.path);
    const name = entry.name ?? path.basename(file).replace(/\.world\.json$|\.json$/, '');
    if (!NAME.test(name)) {
      throw new TypeError(`"${name}" is not a usable world resource name`);
    }
    if (this.records.has(name)) {
      throw new RangeError(`world resource ${name} exists already`);
    }
    const data = JSON.parse(fs.readFileSync(file, 'utf8'));
    if (data === null || typeof data !== 'object' || Array.isArray(data)) {
      throw new TypeError('a world export is a JSON object');
    }
    const label = path.basename(file);
    // Read every list before spawning anything, so a bad file adds nothing at all.
    const lists = {
      props: entriesOf(label, 'props', data.props),
      effects: entriesOf(label, 'effects', data.effects),
      lights: entriesOf(label, 'lights', data.lights),
      world: entriesOf(label, 'world', data.world),
      items: entriesOf(label, 'items', data.items),
      doors: entriesOf(label, 'doors', data.doors),
      smartObjects: entriesOf(label, 'smartObjects', data.smartObjects),
      areas: entriesOf(label, 'areas', data.areas),
      patrolRoutes: entriesOf(label, 'patrolRoutes', data.patrolRoutes),
    };
    const record = {
      name,
      path: file,
      level: typeof data.level === 'string' ? data.level : '',
      props: [],
      effects: [],
      lights: [],
      edits: [],
      items: [],
      doors: [],
      smartObjects: [],
      areas: [],
      routes: [],
    };
    try {
      this.spawn(record, lists);
    } catch (error) {
      this.remove(record);
      throw error;
    }
    this.records.set(name, record);
  }

  // Spawns the lists of an export through the script APIs, keeping each handle.
  spawn(record, lists) {
    const { Prop, Vfx, Light, LevelEdit, GroundItem, Door, SmartObject, Area, PatrolRoute } =
      this.host.api;
    for (const item of lists.props) {
      const { model, position, rotation, scale, physics, dimension } = item;
      record.props.push(Prop.spawn(model, position, rotation, scale, physics, dimension));
    }
    for (const item of lists.effects) {
      const { effect, position, dimension, ...options } = item;
      record.effects.push(Vfx.spawn(effect, position, options, dimension));
    }
    for (const item of lists.lights) {
      const { position, ...options } = item;
      record.lights.push(Light.spawn(position, options));
    }
    for (const item of lists.world) {
      const { dimension, ...definition } = item;
      record.edits.push(LevelEdit.apply(definition, dimension));
    }
    for (const entry of lists.items) {
      const { item, position, rotation, amount, dimension, ...properties } = entry;
      record.items.push(GroundItem.spawn(item, position, rotation, amount, dimension, properties));
    }
    for (const item of lists.doors) {
      const { dimension, ...definition } = item;
      record.doors.push(Door.spawn(definition, dimension));
    }
    for (const item of lists.smartObjects) {
      const { dimension, ...definition } = item;
      record.smartObjects.push(SmartObject.spawn(definition, dimension));
    }
    for (const item of lists.areas) {
      record.areas.push(Area.create(item));
    }
    for (const item of lists.patrolRoutes) {
      record.routes.push(PatrolRoute.create(item));
    }
  }

  // Takes everything a record spawned out of the world again.
  remove(record) {
    for (const prop of record.props) prop.destroy();
    for (const vfx of record.effects) vfx.destroy();
    for (const light of record.lights) light.destroy();
    for (const edit of record.edits) edit.restore();
    for (const stack of record.items) stack.destroy();
    for (const door of record.doors) door.destroy();
    for (const object of record.smartObjects) object.destroy();
    for (const area of record.areas) area.destroy();
    for (const route of record.routes) route.destroy();
  }

  // Unloads every export at shutdown.
  clear() {
    for (const record of this.records.values()) {
      this.remove(record);
    }
    this.records.clear();
    this.ready = false;
  }
}

module.exports = { WorldResources };
