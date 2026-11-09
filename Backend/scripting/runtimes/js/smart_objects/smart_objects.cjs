'use strict';

// The level's stations as the server's SmartObject lists them: its
// dice tables (dice/tables.json), smithies and alchemy tables (crafting/stations.json), drying
// racks and smokehouses (food_stations.json, Dev/tools/smart_objects/build_food_stations.py), on
// the level the server runs.
//
//   SmartObject.stations(dimension?)   the stations players in a world can use
//   SmartObject.remove(id, dimension?) takes one of the level's out of a world
//   SmartObject.restore(id, dimension?) puts it back
//   SmartObject.removed(dimension?)    the ones taken out
//
// A station is taken out through a level edit on its entity (LevelEdit, kind 'entity', hidden), so
// every client seeing that world hides it, a World Builder export's edit counts the same, and the
// server refuses any new session at it (dice, crafting, drying and smoking; one already running
// finishes). A removal from the global dimension applies to all. Not saved, as with every
// level edit.
//
// A script places one of the game's interactive prefabs (SmartObject.spawn, blueprints.json from
// Dev/tools/smart_objects/build_blueprints.py): every client in its dimension builds the whole
// prefab natively at an anchor of its own (Client/runtime/smart_objects/), a player who comes
// later too. It goes when the resource that placed it stops.
//
//   { op: 'smartObjectSpawn', id, template, position, rotation, dimension, to? }
//   { op: 'smartObjectDestroy', id }

const foodStations = require('./food_stations.json');
const diceTables = require('../dice/tables.json');
const craftingStations = require('../crafting/stations.json');
const blueprintRows = require('./blueprints.json');
const { OWNER, removeOwned } = require('../lifecycle/owned_objects.cjs');
const { Vector3 } = require('../math.cjs');

const GLOBAL_WORLD = 0xffffffff;
const GUID = /^[0-9a-f]{16}$/;
const KINDS = Object.freeze(['dice', 'alchemy', 'smithing', 'drying', 'smoking']);
// A drying rack or smokehouse is the one within this many metres of where the player's game
// prepares.
const FOOD_REACH = 4;
// A placed smart object: within this far of the origin, a name of at most this many bytes.
const SPAWN_LIMIT = 16384;
const NAME_MAX_BYTES = 64;

// What SmartObject.spawn places, by name: { name, template, kind, seatPlaces, stationPlace,
// station, corrections, uses }; a place is an entity's index in the game's build of the prefab,
// `station` where its station stands in the prefab ({ at, behavior? }, null for a seat or a bed),
// `uses` its seats and beds: [place, [x, y, z], [x, y, z, w], helper class] in the prefab's frame.
const BLUEPRINTS = new Map(
  blueprintRows.map(
    ([name, template, kind, seatPlaces, stationPlace, station, corrections, uses = []]) => [
      name.toLowerCase(),
      Object.freeze({ name, template, kind, seatPlaces, stationPlace, station, corrections, uses }),
    ],
  ),
);

// The EntityGuid every client gives the entity at `place` of the build of smart object `id`
// (Client/runtime/smart_objects/ smart_object_guid: 'kc' in the high 16 bits, the id, the place),
// as sixteen lowercase hex digits in the level files' order (the low 32 bits, then bits 32 to 47,
// then 48 to 63), as the level's stations are named (dice/tables.json) and level edits take them.
function placeGuid(id, place) {
  const key = ((0x6b63n << 48n) | (BigInt(id) << 16n) | BigInt(place & 0xffff))
    .toString(16)
    .padStart(16, '0');
  return key.slice(8, 16) + key.slice(4, 8) + key.slice(0, 4);
}

// A point of a placed object's prefab in the world: its origin plus the point turned by its
// rotation.
function worldPoint(object, [x, y, z]) {
  const { x: qx, y: qy, z: qz, w: qw } = object.facing;
  const tx = 2 * (qy * z - qz * y);
  const ty = 2 * (qz * x - qx * z);
  const tz = 2 * (qx * y - qy * x);
  return {
    x: object.at.x + x + qw * tx + (qy * tz - qz * ty),
    y: object.at.y + y + qw * ty + (qz * tx - qx * tz),
    z: object.at.z + z + qw * tz + (qx * ty - qy * tx),
  };
}

// Whether a player of `viewer` sees what is placed in `placed` (the server's world_sees).
function sees(viewer, placed) {
  return viewer === GLOBAL_WORLD || placed === GLOBAL_WORLD || viewer === placed;
}

function finiteList(value, count) {
  if (Array.isArray(value)) {
    return value.length === count && value.every(Number.isFinite) ? value : null;
  }
  if (!value || typeof value !== 'object') {
    return null;
  }
  const list = ['x', 'y', 'z', 'w'].slice(0, count).map((name) => value[name]);
  return list.every(Number.isFinite) ? list : null;
}

function validWorld(world) {
  return Number.isSafeInteger(world) && world >= 0 && world <= GLOBAL_WORLD;
}

function worldOf(dimension, what) {
  const world = dimension ?? GLOBAL_WORLD;
  if (!validWorld(world)) {
    throw new TypeError(`${what}: invalid dimension`);
  }
  return world;
}

// "klaster/13edb65c-7cae-0bff" or "13edb65c-7cae-0bff" as sixteen lowercase hex digits.
function guidOf(id) {
  return String(id).split('/').pop().replace(/-/g, '').toLowerCase();
}

// Every station of every level: { id, kind, level, position }.
function catalog() {
  const rows = [];
  for (const table of diceTables) {
    rows.push({ id: table.id, kind: 'dice', level: table.level, position: table.position });
  }
  for (const station of craftingStations) {
    rows.push({
      id: guidOf(station.id),
      kind: station.kind,
      level: station.level,
      position: station.position,
    });
  }
  for (const [level, id, kind, x, y, z] of foodStations) {
    rows.push({ id, kind, level, position: { x, y, z } });
  }
  return rows.map((row) => Object.freeze({ ...row, position: Object.freeze({ ...row.position }) }));
}

const STATIONS = Object.freeze(catalog());
const BY_ID = new Map(STATIONS.map((station) => [station.id, station]));

class SmartObjects {
  // `host`: the resource host (levelEdits, the level the server runs).
  constructor(host) {
    this.host = host;
    // The smart objects scripts placed, by id.
    this.placed = new Map();
  }

  level() {
    return this.host.statusRoutes?.settings?.map || 'trosecko';
  }

  // The level's stations, on the level the server runs.
  levelStations() {
    const level = this.level();
    return STATIONS.filter((station) => station.level === level);
  }

  // The hiding level edits of one station's entity, by dimension.
  removals(id) {
    const removals = new Map();
    for (const edit of this.host.levelEdits.edits.values()) {
      if (edit.kind === 'entity' && edit.hidden && edit.guid === id) {
        removals.set(edit.dimension, edit);
      }
    }
    return removals;
  }

  // Whether a station (any of its ids' spellings) is out of a dimension: out of that world or
  // out of the global one, which is out of every world.
  removedIn(id, dimension) {
    const guid = guidOf(id);
    if (!BY_ID.has(guid)) {
      return false;
    }
    const removals = this.removals(guid);
    return removals.has(GLOBAL_WORLD) || removals.has(dimension);
  }

  // The drying rack or smokehouse of `kind` nearest a point, within FOOD_REACH; null for none.
  foodStationAt(kind, position) {
    let nearest = null;
    for (const station of [...this.levelStations(), ...this.placedStations(kind)]) {
      if (station.kind !== kind) {
        continue;
      }
      const reach = Math.hypot(
        station.position.x - position.x,
        station.position.y - position.y,
        station.position.z - position.z,
      );
      if (reach <= FOOD_REACH && (!nearest || reach < nearest.reach)) {
        nearest = { station, reach };
      }
    }
    return nearest ? nearest.station : null;
  }

  stationInfo(station) {
    const { x, y, z } = station.position;
    return Object.freeze({
      id: station.id,
      kind: station.kind,
      position: new Vector3(x, y, z),
      placed: false,
      smartObject: null,
    });
  }

  // SmartObject.spawn: the placed object, told to the players of its world.
  spawn(SmartObject, definition, dimension) {
    const owner = this.host.context.getStore();
    if (
      owner &&
      (this.host.resources.get(owner.name) !== owner || !owner.active || owner.failing)
    ) {
      throw new Error('Resource is stopped');
    }
    if (!definition || typeof definition !== 'object') {
      throw new TypeError('SmartObject.spawn takes a smart object definition');
    }
    const world = worldOf(dimension, 'SmartObject.spawn');
    const blueprint =
      typeof definition.blueprint === 'string'
        ? BLUEPRINTS.get(definition.blueprint.toLowerCase())
        : undefined;
    if (!blueprint) {
      throw new TypeError(
        `SmartObject.spawn: unknown blueprint '${definition.blueprint}' (SmartObject.blueprints())`,
      );
    }
    const position = finiteList(definition.position, 3);
    if (!position || position.some((n) => Math.abs(n) > SPAWN_LIMIT)) {
      throw new TypeError(
        `SmartObject.spawn: position must be finite, within ${SPAWN_LIMIT} of the origin`,
      );
    }
    const turn =
      definition.rotation === undefined ? [0, 0, 0, 1] : finiteList(definition.rotation, 4);
    const length = turn ? Math.hypot(...turn) : 0;
    if (!turn || length < 1e-6) {
      throw new TypeError('SmartObject.spawn: rotation must be a finite quaternion');
    }
    const name = definition.name ?? '';
    if (typeof name !== 'string' || Buffer.byteLength(name, 'utf8') > NAME_MAX_BYTES) {
      throw new TypeError(
        `SmartObject.spawn: name must be a text of at most ${NAME_MAX_BYTES} bytes`,
      );
    }
    const [x, y, z, w] = turn.map((n) => n / length);
    const object = Object.create(SmartObject.prototype);
    const fixed = {
      id: this.host.allocateEntityId(),
      blueprint: blueprint.name,
      kind: blueprint.kind,
      name,
      dimension: world,
    };
    for (const [field, value] of Object.entries(fixed)) {
      Object.defineProperty(object, field, { value, enumerable: true });
    }
    Object.defineProperty(object, 'at', {
      value: { x: position[0], y: position[1], z: position[2] },
    });
    Object.defineProperty(object, 'facing', { value: { x, y, z, w } });
    object[OWNER] = owner?.name ?? null;
    this.placed.set(object.id, object);
    this.host.bridge.send(this.spawnMessage(object));
    return object;
  }

  spawnMessage(object) {
    const blueprint = BLUEPRINTS.get(object.blueprint.toLowerCase());
    return {
      op: 'smartObjectSpawn',
      id: object.id,
      template: blueprint.template,
      position: { ...object.at },
      rotation: { ...object.facing },
      dimension: object.dimension,
      // The entities with a brush among their parents, which the game's runtime build places by
      // their own position only: where they stand in the prefab (blueprints.json).
      corrections: blueprint.corrections.map(([place, [x, y, z], [qx, qy, qz, qw]]) => ({
        place,
        at: { x, y, z },
        turn: { x: qx, y: qy, z: qz, w: qw },
      })),
    };
  }

  // Takes a placed object down on every client and forgets it.
  destroy(object) {
    if (this.placed.get(object.id) !== object) {
      return;
    }
    this.placed.delete(object.id);
    // The NPCs on its seats stand up where they are (Npc.useObject).
    this.host.objectUses?.objectGone(object);
    this.host.bridge.send({ op: 'smartObjectDestroy', id: object.id });
  }

  // A player's world became ready: their game builds the placed objects of its world again.
  arrived(player) {
    for (const object of this.placed.values()) {
      const world = player.dimension;
      if (
        world === GLOBAL_WORLD ||
        object.dimension === GLOBAL_WORLD ||
        world === object.dimension
      ) {
        this.host.bridge.send({ ...this.spawnMessage(object), to: player.id });
      }
    }
  }

  // The smart objects a stopping resource placed go with it.
  resourceStopped(name) {
    removeOwned(this.placed.values(), name, (object) => this.destroy(object));
  }

  // The placed stations of a kind (every kind when left out), as the server's station catalogs
  // keep theirs: { id, kind, position, dimension, smartObject } and, for a dice table, behavior
  // and seats.
  placedStations(kind) {
    const rows = [];
    for (const object of this.placed.values()) {
      const blueprint = BLUEPRINTS.get(object.blueprint.toLowerCase());
      if (!blueprint.station || (kind !== undefined && blueprint.kind !== kind)) {
        continue;
      }
      const row = {
        id: placeGuid(object.id, blueprint.stationPlace),
        kind: blueprint.kind,
        position: worldPoint(object, blueprint.station.at),
        dimension: object.dimension,
        smartObject: object,
      };
      if (blueprint.kind === 'dice') {
        row.behavior = placeGuid(object.id, blueprint.station.behavior);
        row.seats = blueprint.seatPlaces.map((place) => placeGuid(object.id, place));
      }
      rows.push(Object.freeze(row));
    }
    return rows;
  }

  // Whether a player of `viewer` sees a placed station's world.
  seesPlaced(viewer, station) {
    return sees(viewer, station.dimension);
  }

  // The SmartObject class of the server API.
  define(Entity) {
    const registry = this;
    this.SmartObject ??= class SmartObject {
      constructor(id) {
        const object = registry.placed.get(id);
        if (!object) {
          throw new TypeError('Unknown smart object; SmartObject.spawn places one');
        }
        return object;
      }
      // Places one of the game's interactive prefabs (SmartObject.blueprints()) for every player
      // in a dimension (the global one when left out). Throws for an unknown blueprint or a
      // transform that is not finite.
      static spawn(definition, dimension) {
        return registry.spawn(SmartObject, definition, dimension);
      }
      // The smart objects scripts placed, of every world or of one.
      static all(dimension) {
        if (dimension !== undefined && !validWorld(dimension)) {
          throw new TypeError('SmartObject.all: invalid dimension');
        }
        return [...registry.placed.values()].filter(
          (object) => dimension === undefined || object.dimension === dimension,
        );
      }
      static getById(id) {
        return registry.placed.get(id) ?? null;
      }
      // Everything SmartObject.spawn places, sorted by name: { name, kind, seats } each.
      static blueprints() {
        return [...BLUEPRINTS.values()]
          .map(({ name, kind, seatPlaces }) =>
            Object.freeze({ name, kind, seats: seatPlaces.length }),
          )
          .sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
      }
      get position() {
        return new Vector3(this.at.x, this.at.y, this.at.z);
      }
      // The EntityGuid of its station part (the dice table's DiceInteractor, an AlchemyTable, a
      // Smithery, a FoodProcessingTrigger), as every client builds it; empty for a seat or a bed.
      get stationKey() {
        const { stationPlace } = BLUEPRINTS.get(this.blueprint.toLowerCase());
        return stationPlace === null ? '' : placeGuid(this.id, stationPlace);
      }
      // EntityGuids of its seats or sleeping places.
      get seats() {
        const { seatPlaces } = BLUEPRINTS.get(this.blueprint.toLowerCase());
        return seatPlaces.map((place) => placeGuid(this.id, place));
      }
      // Takes it down for every player.
      destroy() {
        registry.destroy(this);
      }
      // A SmartObject.spawn definition that recreates it; null after removal.
      toJSON() {
        if (registry.placed.get(this.id) !== this) {
          return null;
        }
        const definition = {
          blueprint: this.blueprint,
          position: { ...this.at },
          rotation: { ...this.facing },
        };
        if (this.name) {
          definition.name = this.name;
        }
        return definition;
      }
      toString() {
        return `SmartObject(${this.id}, ${this.blueprint}, ${this.kind})`;
      }
      // The level's own seats, beds and work places near a point, nearest first
      // (npcs/object_use.cjs): what Npc.useObject takes besides a SmartObject.
      static levelPlaces(position, radius) {
        return registry.host.objectUses.levelPlaces(position, radius);
      }
      // The stations players in a dimension can use (the global one when left out): the
      // level's dice tables, crafting stations, drying racks and smokehouses, minus any a level
      // edit took out of that world.
      static stations(dimension) {
        const world = worldOf(dimension, 'SmartObject.stations');
        const level = registry
          .levelStations()
          .filter((station) => !registry.removedIn(station.id, world))
          .map((station) => registry.stationInfo(station));
        const placed = registry
          .placedStations()
          .filter((station) => sees(world, station.dimension))
          .map((station) =>
            Object.freeze({
              id: station.id,
              kind: station.kind,
              position: new Vector3(station.position.x, station.position.y, station.position.z),
              placed: true,
              smartObject: station.smartObject,
            }),
          );
        return [...level, ...placed];
      }
      // Removes a level station from a dimension (by default the global one, meaning all) with
      // a level edit: lists skip it, new sessions are blocked and clients there stop showing
      // it. Throws for an id that is not one
      // of the level's stations.
      static remove(id, dimension) {
        const station = registry.stationOf(id, 'SmartObject.remove');
        const world = worldOf(dimension, 'SmartObject.remove');
        registry.host.api.LevelEdit.apply(
          { kind: 'entity', guid: station.id, at: station.position, hidden: true },
          world,
        );
      }
      // Puts back one of the level's stations a level edit took out of a dimension (the global
      // one when left out). False when that world had no such removal.
      static restore(id, dimension) {
        const world = worldOf(dimension, 'SmartObject.restore');
        const guid = typeof id === 'string' ? guidOf(id) : '';
        const edit = GUID.test(guid) ? registry.removals(guid).get(world) : undefined;
        if (!edit) {
          return false;
        }
        registry.host.levelEdits.restore(edit.id);
        return true;
      }
      // The level's stations a level edit took out, by a script or a World Builder export alike:
      // { id, kind, position, dimension } each.
      static removed(dimension) {
        if (dimension !== undefined && !validWorld(dimension)) {
          throw new TypeError('SmartObject.removed: invalid dimension');
        }
        const rows = [];
        for (const station of registry.levelStations()) {
          for (const world of registry.removals(station.id).keys()) {
            if (dimension === undefined || world === dimension) {
              const { x, y, z } = station.position;
              rows.push(
                Object.freeze({
                  id: station.id,
                  kind: station.kind,
                  position: new Vector3(x, y, z),
                  dimension: world,
                }),
              );
            }
          }
        }
        return rows;
      }
    };
    if (Entity) {
      Object.setPrototypeOf(this.SmartObject.prototype, Entity.prototype);
    }
    return this.SmartObject;
  }

  // One of the level's stations by its id; throws for any other.
  stationOf(id, what) {
    const guid = typeof id === 'string' ? guidOf(id) : '';
    const station = GUID.test(guid) ? BY_ID.get(guid) : undefined;
    if (!station || station.level !== this.level()) {
      throw new RangeError(`${what}: ${String(id)} is not one of the level's stations`);
    }
    return station;
  }
}

module.exports = { SmartObjects, KINDS, guidOf, BLUEPRINTS, worldPoint };
