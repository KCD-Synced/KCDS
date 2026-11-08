'use strict';

// The level's doors: the server API's Door. The server learns each of the level's doors
// from the first client that has it built (doorReport) and from then on is the authority on
// whether it stands open and whether it is locked. Doors are kept per dimension, keyed by the
// level's EntityGuid.
//
// A script hangs a door of its own where the level has none (Door.spawn): one of the
// leaves the levels hang (door_models.json, Dev/tools/doors/build_door_models.py), built by every
// client in its dimension as an AnimDoor entity of a GUID the server gives it (kcds, then a
// number), so that from then on it is reported, worked and set like the level's own. It starts
// closed, and goes when the resource that hung it stops.
//
// Client -> server ops (a ready player's game):
//   { op: 'doorReport', id, doors: [{ guid, name, position, open, side, locked, canBePicked,
//       key, builtLocked, difficulty }] }   doors that came into view; sent once per door and
//                                   player (difficulty: the lock-picking difficulty it was built
//                                   with, 0 to 1)
//   { op: 'doorUse', id, guid, action: open | close | unlock | lockpick, keySide }
//                                   the player worked a door; their game has already applied it
//   { op: 'doorNearestReply', id, request, door?: <a doorReport entry>, reason? }
// Server -> client ops (sent only when something changes, no timers):
//   { op: 'doorSet', id, guid, open, side, locked, difficulty }   pose the door and set its lock
//                   (difficulty: a script's lock-picking difficulty, -1 the door's own); sent to the
//                   other players in its world when a use was accepted or a script changed it,
//                   to every player when a use was refused, and to a player whose report shows
//                   a state the server has since changed
//   { op: 'doorNearest', id, request }                which door is the player's body standing at?
//   { op: 'doorSpawn', guid, model, position, rotation, animSet, helpers, smartObjectType, rigid,
//     pushable, locked, lockTypes, canBePicked, difficulty, key, dimension, to? }
//                   hang a door a script made, for every player of its world or for `to`
//   { op: 'doorDespawn', guid }                       take it down
//
// A door is taken out of a dimension (Door.remove, door.destroy()) through a level
// edit on its entity (LevelEdit, kind 'entity', hidden): every client seeing that world hides it,
// collision and all, and the server lists it nowhere and refuses its use there. Door.restore puts
// it back. A door is known by its GUID once a player's game reported it (the level's files name
// their doors by other GUIDs than the running game, so the server has no list of its own).

const { Vector3, vector } = require('../math.cjs');
const { lookupItemClass } = require('../inventory/catalog.cjs');
const { OWNER, removeOwned } = require('../lifecycle/owned_objects.cjs');
const MODEL_ROWS = require('./door_models.json');

// A lock-picking difficulty: a number from 0 to 1, else null.
function fraction(value) {
  return typeof value === 'number' && value >= 0 && value <= 1 ? value : null;
}

const GLOBAL_WORLD = 0xffffffff;
const ACTIONS = new Set(['open', 'close', 'unlock', 'lockpick']);
const GUID = /^(?:0x)?([0-9a-f]{1,16})$/i;
// The lock-picking difficulty a door is built with when its game reports none.
const DEFAULT_DIFFICULTY = 0.5;
// How far from a door (across the ground, metres) an NPC that worked it may stand (issue 016).
const NPC_DOOR_REACH = 4;
// A client reports at most this many doors in one message.
const REPORT_MAX = 256;
// A door farther out than this lies outside any level.
const WORLD_LIMIT = 1e6;
// A door a script hangs: within this far of the origin, a name of at most this many bytes.
const SPAWN_LIMIT = 16384;
const NAME_MAX_BYTES = 64;
// The GUIDs of the doors scripts hang: 'kcds' in its high 32 bits, a number in the low ones.
const SPAWNED_GUID_PREFIX = '6b636473';
// What each side takes to get through a hung door while it is locked (AnimDoor.lua esLockTypes,
// outside/inside).
const LOCK_TYPES = {
  none: 'none/none',
  bolt: 'manual/none',
  boltAndKey: 'manual/key',
  key: 'key/key',
};

// The leaves the levels hang, by path and by file stem (lower case).
const MODELS = new Map();
for (const [
  path,
  hinge,
  placements,
  animSet,
  helpers,
  smartObjectType,
  rigid,
  pushable,
] of MODEL_ROWS) {
  const model = { path, hinge, placements, animSet, helpers, smartObjectType, rigid, pushable };
  MODELS.set(path, model);
  MODELS.set(path.slice(path.lastIndexOf('/') + 1, -'.cdf'.length), model);
}

// The leaf of a model name (a path or a file stem), or null.
function modelOf(name) {
  return typeof name === 'string' ? MODELS.get(name.toLowerCase()) ?? null : null;
}

function finiteList(value, count) {
  if (Array.isArray(value)) {
    return value.length === count && value.every(Number.isFinite) ? value : null;
  }
  if (!value || typeof value !== 'object') {
    return null;
  }
  const names = ['x', 'y', 'z', 'w'].slice(0, count);
  const list = names.map((name) => value[name]);
  return list.every(Number.isFinite) ? list : null;
}

function validWorld(world) {
  return Number.isSafeInteger(world) && world >= 0 && world <= GLOBAL_WORLD;
}

// Whether a player of `viewer` sees what is placed in `placed`, as the server's world_sees.
function sees(viewer, placed) {
  return viewer === GLOBAL_WORLD || placed === GLOBAL_WORLD || viewer === placed;
}

// The sixteen lowercase hex digits of a GUID given with or without 0x, or null.
const { areasOfDoor, doorsOfArea } = require('../areas/area_doors.cjs');

function guidOf(value) {
  const match = typeof value === 'string' ? GUID.exec(value) : null;
  return match ? match[1].toLowerCase().padStart(16, '0') : null;
}

// The same entity key as a level edit writes it, in the level files' order (the low 32 bits, then
// bits 32 to 47, then 48 to 63; kcd2s/integration/level_edits.hpp): a door's GUID is the key's
// plain hex (doors.hpp door_guid_text).
function levelGuidOf(guid) {
  return guid.slice(8, 16) + guid.slice(4, 8) + guid.slice(0, 4);
}

class Doors {
  // `host`: the resource host (bridge, emit, allocateEntityId, players).
  constructor(host) {
    this.host = host;
    // `${world}:${guid}` -> Door
    this.doors = new Map();
    // id -> Door
    this.byId = new Map();
    // request number -> { player, resolve }
    this.queries = new Map();
    this.nextRequest = 1;
    // The doors scripts hung, by GUID, and the number of the next.
    this.hungByGuid = new Map();
    this.nextHung = 1;
  }

  // The Door class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class Door {
      constructor(id) {
        const door = registry.byId.get(id);
        if (!door) {
          throw new Error('Unknown door');
        }
        return door;
      }
      // An area's doors as the level data links them, given its id; works with nobody nearby.
      static linkedTo(areaId) {
        return doorsOfArea(registry.host.areas.levelOfAreaId(areaId), areaId);
      }
      // Every door players' games reported, minus those taken out of their world.
      static all() {
        return [...registry.byId.values()].filter((door) => !registry.removedIn(door));
      }
      static getById(id) {
        return registry.byId.get(id) ?? null;
      }
      // The key that works this door now: its own, else its area's, else the level's.
      get key() {
        return registry.effectiveKey(this);
      }
      // A key of this door's own, an item class by name or GUID, ahead of its area's; null takes it
      // away.
      setKey(item) {
        registry.setOwnKey(this, item);
      }
      // Level areas this door is linked to, such as its house, workshop or store room.
      get areas() {
        // A level door's GUID is of one level: each map's level is asked.
        const { Area } = registry.host.api ?? registry.host.makeAPI();
        const levels = registry.host.areas.levels.size
          ? [...registry.host.areas.levels.values()]
          : [registry.host.areas.level];
        return levels
          .flatMap((level) => areasOfDoor(level, this.guid))
          .map((id) => Area.getById(id))
          .filter((area) => area !== null);
      }
      static find(guid, dimension = GLOBAL_WORLD) {
        const key = guidOf(guid);
        if (key === null) {
          throw new TypeError('A door GUID is up to sixteen hex digits');
        }
        if (!validWorld(dimension)) {
          throw new TypeError('Invalid dimension');
        }
        const hung = registry.hungByGuid.get(key);
        if (hung) {
          return hung.dimension === dimension ? hung : null;
        }
        const door = registry.doors.get(`${dimension}:${key}`) ?? null;
        return door && !registry.removedIn(door) ? door : null;
      }
      // Takes a door out of a dimension (the global one, and so every world, when left out)
      // through a level edit on its entity. Throws for a GUID no player's game has reported.
      static remove(guid, dimension = GLOBAL_WORLD) {
        registry.remove(guid, dimension, 'Door.remove');
      }
      // Puts back a door a level edit took out of a dimension (the global one when left out);
      // it returns in its original form. Gives the door, or null if the dimension lacks such a
      // removal.
      static restore(guid, dimension = GLOBAL_WORLD) {
        return registry.restore(guid, dimension);
      }
      // Removed doors, from scripts or world exports: { guid, name, position,
      // dimension } each, of every world or of one.
      static removed(dimension) {
        return registry.removedRows(dimension);
      }
      // Asks the player's game which door their body stands at.
      static closestTo(player) {
        return registry.closestTo(player);
      }
      // Adds a new usable door for the players of a
      // dimension (the global one when left out). Throws for an unknown model, a transform that is not
      // finite, an invalid lock type, or a key without an item class.
      // `map`: one of World.maps, the first by default; only the players on it get the door.
      static spawn(definition, dimension = GLOBAL_WORLD, map = undefined) {
        return registry.spawn(definition, dimension, map);
      }
      // Every leaf the shipped levels hang, sorted by path.
      static models() {
        return MODEL_ROWS.map(([path, hinge, placements]) =>
          Object.freeze({
            path,
            name: path.slice(path.lastIndexOf('/') + 1, -'.cdf'.length),
            hinge,
            placements,
          }),
        );
      }
      get position() {
        return new Vector3(this.at.x, this.at.y, this.at.z);
      }
      get open() {
        return this.isOpen;
      }
      // The side it was last used from, which sets its swing direction.
      get side() {
        return this.swungBack;
      }
      get locked() {
        return this.isLocked;
      }
      set locked(value) {
        if (typeof value !== 'boolean') {
          throw new TypeError('locked is true or false');
        }
        registry.change(this, { locked: value, byScript: value });
      }
      // Whether a script or operator set the lock rather than the level; clears on unlock.
      get lockedByScript() {
        return this.scriptLock;
      }
      // How hard its lock is to pick, 0 (easy) to 1 (hard): the level's own, or a script's for
      // every player of its world. Assign a number to set it, null to give the
      // level's back.
      get lockLevel() {
        return this.difficulty ?? this.builtDifficulty;
      }
      set lockLevel(value) {
        registry.setDifficulty(this, value);
      }
      // Moves the door by script alone; a locked one gets unlocked when opened.
      // False if it already is in that state.
      setOpen(open, side) {
        if (typeof open !== 'boolean') {
          throw new TypeError('setOpen takes true or false');
        }
        return registry.change(this, {
          open,
          side: side === undefined ? undefined : side === true,
          locked: open ? false : undefined,
        });
      }
      // Whether a script hung this door (Door.spawn) rather than the level placing it.
      get spawned() {
        return this.hung !== null;
      }
      // The leaf mesh it hangs, objects/characters/assets/doors/...cdf.
      get model() {
        return this.hung ? this.hung.model.path : this.leaf;
      }
      // A hung door is taken down for good; the level's is taken out of its dimension, as
      // Door.remove does.
      destroy() {
        if (this.hung) {
          registry.takeDown(this);
          return;
        }
        registry.remove(this.guid, this.dimension, 'door.destroy');
      }
      // A Door.spawn definition that recreates this door in its current lock state; null for a
      // door the level places.
      toJSON() {
        if (!this.hung) {
          return null;
        }
        const { model, rotation, lock, lockLevel } = this.hung;
        const definition = {
          model: model.path,
          position: { x: this.at.x, y: this.at.y, z: this.at.z },
          rotation: { ...rotation },
          name: this.name,
          locked: this.isLocked,
          lock,
          canBePicked: this.canBePicked,
        };
        if (lockLevel !== undefined) {
          definition.lockLevel = lockLevel;
        }
        if (this.lockKey) {
          definition.lockKey = this.lockKey;
        }
        return definition;
      }
      toString() {
        return `Door(${this.id}, ${this.guid}, ${this.isOpen ? 'open' : 'closed'}, ${
          this.isLocked ? 'locked' : 'unlocked'
        })`;
      }
    }
    Object.setPrototypeOf(Door.prototype, Entity.prototype);
    this.Door = Door;
    return Door;
  }

  // The handle of a reported door, made the first time it is seen, which doorDiscovered announces;
  // null for a bad entry.
  learn(player, entry) {
    const guid = guidOf(entry?.guid);
    const at = entry?.position && vector(entry.position);
    if (guid === null || !at || ![at.x, at.y, at.z].every((n) => Math.abs(n) <= WORLD_LIMIT)) {
      return null;
    }
    const hung = this.hungDoor(guid, player.dimension);
    if (hung) {
      return hung;
    }
    const key = `${player.dimension}:${guid}`;
    let door = this.doors.get(key);
    if (door) {
      return door;
    }
    door = Object.create(this.Door.prototype);
    const fixed = {
      id: this.host.allocateEntityId(),
      guid,
      name: String(entry.name ?? '').slice(0, 128),
      dimension: player.dimension,
      lockKey: String(entry.key ?? '').slice(0, 64),
      canBePicked: entry.canBePicked === true,
      builtLocked: entry.builtLocked === true,
      builtDifficulty: fraction(entry.difficulty) ?? DEFAULT_DIFFICULTY,
    };
    for (const [name, value] of Object.entries(fixed)) {
      Object.defineProperty(door, name, { value, enumerable: true });
    }
    Object.defineProperty(door, 'at', { value: { x: at.x, y: at.y, z: at.z } });
    // The changeable state, only ever written by this registry.
    door.isOpen = entry.open === true;
    door.swungBack = entry.side === true;
    door.isLocked = entry.locked === true;
    door.scriptLock = false;
    // A script's lock-picking difficulty, null for the level's own.
    door.difficulty = null;
    // The leaf it hangs, and what a script hung it with.
    door.leaf = String(entry.model ?? '').slice(0, 160);
    door.hung = null;
    this.doors.set(key, door);
    this.byId.set(door.id, door);
    // Once per door and dimension for each server run, so a script can lock it or give it a key
    // before the reporting player uses it.
    this.host.emit('doorDiscovered', door, player);
    return door;
  }

  // The door a script hung of that GUID that a player of `world` sees, or null.
  hungDoor(guid, world) {
    const door = this.hungByGuid.get(guid);
    return door && sees(world, door.dimension) ? door : null;
  }

  // Door.spawn: the door, told to the players of its world on its map.
  spawn(definition, dimension, map) {
    const mapIndex = this.host.mapIndexOf(map, 'Door.spawn');
    const owner = this.host.context.getStore();
    if (
      owner &&
      (this.host.resources.get(owner.name) !== owner || !owner.active || owner.failing)
    ) {
      throw new Error('Resource is stopped');
    }
    if (!definition || typeof definition !== 'object') {
      throw new TypeError('Door.spawn takes a door definition');
    }
    if (!validWorld(dimension)) {
      throw new TypeError('Door.spawn: invalid dimension');
    }
    const model = modelOf(definition.model);
    if (!model) {
      throw new TypeError(`Door.spawn: unknown door model '${definition.model}' (Door.models())`);
    }
    const position = finiteList(definition.position, 3);
    if (!position || position.some((n) => Math.abs(n) > SPAWN_LIMIT)) {
      throw new TypeError(
        `Door.spawn: position must be finite, within ${SPAWN_LIMIT} of the origin`,
      );
    }
    const turn =
      definition.rotation === undefined ? [0, 0, 0, 1] : finiteList(definition.rotation, 4);
    const length = turn ? Math.hypot(...turn) : 0;
    if (!turn || length < 1e-6) {
      throw new TypeError('Door.spawn: rotation must be a finite quaternion');
    }
    const [x, y, z, w] = turn.map((n) => n / length);
    const name = definition.name ?? `spawned_door_${this.nextHung}`;
    if (typeof name !== 'string' || Buffer.byteLength(name, 'utf8') > NAME_MAX_BYTES) {
      throw new TypeError(`Door.spawn: name must be a text of at most ${NAME_MAX_BYTES} bytes`);
    }
    const lock = definition.lock ?? 'boltAndKey';
    if (!Object.hasOwn(LOCK_TYPES, lock)) {
      throw new TypeError("Door.spawn: lock must be 'none', 'bolt', 'boltAndKey' or 'key'");
    }
    for (const field of ['locked', 'canBePicked']) {
      if (definition[field] !== undefined && typeof definition[field] !== 'boolean') {
        throw new TypeError(`Door.spawn: ${field} is true or false`);
      }
    }
    const lockLevel = definition.lockLevel;
    if (lockLevel !== undefined && fraction(lockLevel) === null) {
      throw new RangeError('Door.spawn: lockLevel is a number from 0 to 1');
    }
    let lockKey = '';
    if (definition.lockKey !== undefined) {
      const item = lookupItemClass(definition.lockKey);
      if (!item) {
        throw new TypeError(`Door.spawn: lockKey '${definition.lockKey}' is not an item class`);
      }
      lockKey = item.id;
    }
    const number = this.nextHung++;
    const guid = SPAWNED_GUID_PREFIX + number.toString(16).padStart(8, '0');
    const locked = definition.locked === true;
    const door = Object.create(this.Door.prototype);
    const fixed = {
      id: this.host.allocateEntityId(),
      guid,
      name,
      dimension,
      lockKey,
      canBePicked: definition.canBePicked ?? true,
      builtLocked: locked,
      builtDifficulty: lockLevel ?? DEFAULT_DIFFICULTY,
    };
    for (const [field, value] of Object.entries(fixed)) {
      Object.defineProperty(door, field, { value, enumerable: true });
    }
    Object.defineProperty(door, 'at', {
      value: { x: position[0], y: position[1], z: position[2] },
    });
    // The map it hangs on (an index of World.maps), and its name.
    Object.defineProperty(door, 'mapIndex', { value: mapIndex });
    const host = this.host;
    Object.defineProperty(door, 'map', {
      enumerable: true,
      get() {
        return host.mapName(mapIndex);
      },
    });
    door.isOpen = false;
    door.swungBack = false;
    door.isLocked = locked;
    door.scriptLock = false;
    door.difficulty = null;
    door.leaf = model.path;
    door.hung = { model, rotation: { x, y, z, w }, lock, lockLevel };
    door[OWNER] = owner?.name ?? null;
    this.hungByGuid.set(guid, door);
    this.byId.set(door.id, door);
    if (this.host.maps.length <= 1) {
      this.host.bridge.send(this.hangMessage(door));
    } else {
      // A server of several maps: only the players on the door's map hang it.
      for (const player of this.host.players.values()) {
        if (this.onDoorMap(player, door)) {
          this.host.bridge.send({ ...this.hangMessage(door), to: player.id });
        }
      }
    }
    return door;
  }

  // Whether the player stands on a hung door's map (always for a level's own door).
  onDoorMap(player, door) {
    return !door.hung || this.host.playerMapIndex(player) === door.mapIndex;
  }

  // The doorSpawn op of a hung door.
  hangMessage(door) {
    const { model, rotation, lock, lockLevel } = door.hung;
    return {
      op: 'doorSpawn',
      guid: door.guid,
      model: model.path,
      position: { ...door.at },
      rotation: { ...rotation },
      animSet: model.animSet,
      helpers: model.helpers,
      smartObjectType: model.smartObjectType,
      rigid: model.rigid,
      pushable: model.pushable,
      locked: door.isLocked,
      lockTypes: LOCK_TYPES[lock],
      canBePicked: door.canBePicked,
      difficulty: lockLevel ?? -1,
      key: door.lockKey,
      dimension: door.dimension,
    };
  }

  // Takes a hung door down on every client and forgets it.
  takeDown(door) {
    if (this.hungByGuid.get(door.guid) !== door) {
      return;
    }
    this.hungByGuid.delete(door.guid);
    this.byId.delete(door.id);
    this.host.bridge.send({ op: 'doorDespawn', guid: door.guid });
  }

  // A player's world became ready: their game hangs the doors of its world again, posed as they
  // are now.
  arrived(player) {
    for (const door of this.hungByGuid.values()) {
      if (sees(player.dimension, door.dimension) && this.onDoorMap(player, door)) {
        this.host.bridge.send({ ...this.hangMessage(door), to: player.id });
        if (door.isOpen) {
          this.sendSet(player, door);
        }
      }
    }
  }

  // The doors a stopping resource hung go with it.
  resourceStopped(name) {
    removeOwned(this.hungByGuid.values(), name, (door) => this.takeDown(door));
  }

  // Players with a ready body in the door's world, except `skip`.
  audience(door, skip) {
    const list = [];
    for (const player of this.host.players.values()) {
      const inWorld = door.hung
        ? sees(player.dimension, door.dimension)
        : player.dimension === door.dimension;
      if (player.ready && player !== skip && inWorld && this.onDoorMap(player, door)) {
        list.push(player);
      }
    }
    return list;
  }

  sendSet(player, door) {
    this.host.bridge.send({
      op: 'doorSet',
      id: player.id,
      guid: door.guid,
      open: door.isOpen,
      side: door.swungBack,
      // A player who needs no key sees it unlocked (player.keyBypass). An open door
      // goes out unlocked: the game locks a door by closing it, so a locked door that a bypass
      // player opened stayed shut in every other game. Its lock comes back once it is closed.
      locked: door.isLocked && !door.isOpen && !player.keyBypass,
      difficulty: door.difficulty ?? -1,
      key: this.effectiveKey(door) === door.lockKey ? '' : this.effectiveKey(door),
    });
  }

  // The key that works a door: its own, else its area's, else the level's.
  effectiveKey(door) {
    if (door.ownKey) {
      return door.ownKey;
    }
    return this.host.areas.keys.keyOfDoor(door.guid, door.position) || door.lockKey;
  }

  setOwnKey(door, item) {
    let own = '';
    if (item !== null) {
      const itemClass = lookupItemClass(item);
      if (!itemClass) {
        throw new TypeError(`door.setKey: '${String(item)}' is not an item class`);
      }
      own = itemClass.id;
    }
    door.ownKey = own;
    this.resend(door);
  }

  // A player whose bypass changed sees every door they know as it now is for them.
  resendAllTo(player) {
    for (const door of this.byId.values()) {
      if (door.dimension === player.dimension) {
        this.sendSet(player, door);
      }
    }
  }

  resend(door) {
    for (const player of this.audience(door)) {
      this.sendSet(player, door);
    }
  }

  // Whether this player may work this door's lock: no key needed, or they carry the door's key.
  holdsKey(player, door) {
    if (player.keyBypass) {
      return true;
    }
    const key = this.effectiveKey(door);
    return key !== '' && this.host.stashes.carriesKey(player, key);
  }

  // Whether a script or an area set the key that door answers to (the level's own is the game's).
  scriptKeyed(door) {
    return this.effectiveKey(door) !== door.lockKey;
  }

  // Door#lockLevel: a script's difficulty (0 to 1), or null for the level's own, told to the
  // players of the door's world.
  setDifficulty(door, value) {
    const next = value === null ? null : fraction(value);
    if (next === null && value !== null) {
      throw new RangeError('lockLevel is a number from 0 to 1, or null');
    }
    if (next === door.difficulty) {
      return;
    }
    door.difficulty = next;
    for (const player of this.audience(door)) this.sendSet(player, door);
  }

  // Applies a change and tells the players in the door's world (but `except`); false when
  // nothing changed. An undefined field keeps its value.
  change(door, { open, side, locked, byScript = false }, except) {
    const nextOpen = open ?? door.isOpen;
    const nextLocked = locked ?? door.isLocked;
    const moved = nextOpen !== door.isOpen;
    if (!moved && nextLocked === door.isLocked) {
      return false;
    }
    door.isOpen = nextOpen;
    // The swing only matters while the leaf moves.
    if (moved && side !== undefined) door.swungBack = side;
    if (nextLocked !== door.isLocked) {
      door.isLocked = nextLocked;
      door.scriptLock = nextLocked && byScript;
    }
    for (const player of this.audience(door, except)) this.sendSet(player, door);
    return true;
  }

  // doorReport: doors that came into view of a player's game. A door the server already changed
  // is set right for that player at once.
  report(player, doors) {
    if (!player?.ready || !Array.isArray(doors)) {
      return;
    }
    for (const entry of doors.slice(0, REPORT_MAX)) {
      const door = this.learn(player, entry);
      if (
        door &&
        (door.isOpen !== (entry.open === true) ||
          door.isLocked !== (entry.locked === true) ||
          door.difficulty !== null)
      ) {
        this.sendSet(player, door);
      }
    }
  }

  // The hiding level edits of a door's entity, by dimension.
  removals(guid) {
    const removals = new Map();
    for (const edit of this.host.levelEdits.edits.values()) {
      if (edit.kind === 'entity' && edit.hidden && edit.guid === levelGuidOf(guid)) {
        removals.set(edit.dimension, edit);
      }
    }
    return removals;
  }

  // Whether a door is out of its dimension: out of that world, or out of the global one.
  removedIn(door) {
    const removals = this.removals(door.guid);
    return removals.has(GLOBAL_WORLD) || removals.has(door.dimension);
  }

  // A reported door of that GUID, of any world (they stand in the same place in every one).
  anyDoor(guid) {
    for (const door of this.byId.values()) {
      if (door.guid === guid) {
        return door;
      }
    }
    return null;
  }

  remove(guid, dimension, where) {
    const key = guidOf(guid);
    if (key === null) {
      throw new TypeError(`${where}: a door GUID is up to sixteen hex digits`);
    }
    if (!validWorld(dimension)) {
      throw new TypeError(`${where}: invalid dimension`);
    }
    const door = this.anyDoor(key);
    if (!door || door.hung) {
      throw new TypeError(`${where}: no player's game has reported a door ${key}`);
    }
    this.host.api.LevelEdit.apply(
      { kind: 'entity', guid: levelGuidOf(key), name: door.name, at: door.at, hidden: true },
      dimension,
    );
  }

  restore(guid, dimension) {
    const key = guidOf(guid);
    if (key === null) {
      throw new TypeError('Door.restore: a door GUID is up to sixteen hex digits');
    }
    if (!validWorld(dimension)) {
      throw new TypeError('Door.restore: invalid dimension');
    }
    const edit = this.removals(key).get(dimension);
    if (!edit) {
      return null;
    }
    this.host.levelEdits.restore(edit.id);
    return this.doors.get(`${dimension}:${key}`) ?? this.anyDoor(key);
  }

  removedRows(dimension) {
    if (dimension !== undefined && !validWorld(dimension)) {
      throw new TypeError('Door.removed: invalid dimension');
    }
    const rows = [];
    const seen = new Set();
    for (const door of this.byId.values()) {
      if (seen.has(door.guid)) {
        continue;
      }
      seen.add(door.guid);
      for (const world of this.removals(door.guid).keys()) {
        if (dimension === undefined || world === dimension) {
          rows.push(
            Object.freeze({
              guid: door.guid,
              name: door.name,
              position: new Vector3(door.at.x, door.at.y, door.at.z),
              dimension: world,
            }),
          );
        }
      }
    }
    return rows;
  }

  // doorUse: the player worked a door. The server refuses what the game's rules forbid, asks the
  // doorInteract handlers about the rest, and puts a refused door back on every client.
  use(player, guid, action, keySide) {
    const key = guidOf(guid);
    const door =
      player?.ready &&
      key &&
      (this.hungDoor(key, player.dimension) ?? this.doors.get(`${player.dimension}:${key}`));
    if (!door || !ACTIONS.has(action) || this.removedIn(door)) {
      return;
    }
    const allowed =
      (action === 'open' && (!door.isLocked || player.keyBypass === true) && !door.isOpen) ||
      (action === 'close' && door.isOpen) ||
      (action === 'unlock' &&
        door.isLocked &&
        (this.scriptKeyed(door) || door.scriptLock ? this.holdsKey(player, door) : true)) ||
      (action === 'lockpick' && door.isLocked && door.canBePicked);
    if (!allowed || !this.host.emit('doorInteract', player, door, action, keySide === true)) {
      for (const seen of this.audience(door)) this.sendSet(seen, door);
      return;
    }
    // The player's own game already applied it; everyone else follows.
    if (action === 'open' || action === 'close') {
      this.change(door, { open: action === 'open', side: keySide === true }, player);
    } else {
      this.change(door, { locked: false }, player);
    }
  }

  // doorUse with an NPC (issue 016): the brain of an NPC the sender's game simulates opened or
  // closed one of the level's doors there. Every other game follows, and a late joiner gets it
  // with the door's state. Taken only from the NPC's simulator, for an NPC alive and at the door
  // (NPC_DOOR_REACH), and only opening and closing: the game's brain does not pick locks.
  npcUse(player, npcId, guid, action) {
    const key = guidOf(guid);
    const door = player?.ready && key ? this.doors.get(`${player.dimension}:${key}`) : null;
    const npc = this.host.npcs?.npcs.get(npcId);
    if (!door || this.removedIn(door) || !npc || !this.host.npcs.live(npc) || !npc.alive) {
      return false;
    }
    if (action !== 'open' && action !== 'close') {
      return false;
    }
    if (npc.simulator !== player) {
      return false;
    }
    const at = npc.position;
    const doorAt = door.at;
    if (at && doorAt) {
      const dx = at.x - doorAt.x;
      const dy = at.y - doorAt.y;
      if (dx * dx + dy * dy > NPC_DOOR_REACH * NPC_DOOR_REACH) {
        return false;
      }
    }
    const open = action === 'open';
    if (door.isOpen === open) {
      return false;
    }
    // The simulator's game opened it already; everyone else follows.
    this.change(door, { open, side: false }, player);
    return true;
  }

  closestTo(player) {
    if (!player?.ready) {
      return Promise.resolve({ found: false, reason: 'no answer', door: null });
    }
    const request = this.nextRequest++;
    this.host.bridge.send({ op: 'doorNearest', id: player.id, request });
    return new Promise((resolve) => this.queries.set(request, { player, resolve }));
  }

  // doorNearestReply: what the player's game has in sight.
  nearestReply(player, request, entry, reason) {
    const query = this.queries.get(request);
    if (!query || query.player !== player) {
      return;
    }
    this.queries.delete(request);
    const door = entry ? this.learn(player, entry) : null;
    if (!door) {
      query.resolve({ found: false, reason: String(reason ?? 'nothing in sight'), door: null });
      return;
    }
    query.resolve({ found: true, reason: '', door, guid: door.guid, name: door.name });
  }

  // A player left: their open questions end.
  left(player) {
    for (const [request, query] of this.queries) {
      if (query.player === player) {
        this.queries.delete(request);
        query.resolve({ found: false, reason: 'player left', door: null });
      }
    }
  }
}

module.exports = { Doors };
