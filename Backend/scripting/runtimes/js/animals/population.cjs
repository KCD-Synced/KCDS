'use strict';

// The server's animal population. A KCDS addition, not part of the
// reference API.
//
// The game's own animals are switched off on every client. This module spawns the world's animals
// itself, from the game's spawn points (animals/spawn_points.json, made by
// Dev/tools/animals/extract_spawns.py), so every player sees the same ones, under settings the
// server controls (Npc.setAnimalPopulation: how much spawns, in percent, per kind).
//
// Which spawn points count: the entries of type "spawner" and "placed" that are always in the
// world. Flock entries (chickens, rats ...) and entries a quest layer streams in ("streamed") are
// left out. A spawner that has a "master" (it shares the master's area) uses its own position.
//
// The soul: the native server builds an animal's body from a soul guid. Each animal gets the
// entry's `soulGuid`, else the `defaultSoulGuids` of its kind (the file's header; placed entries
// carry no soul of their own), else none. A point without any soul is not spawned; the first time
// a player is near one, the host log names its kind.
//
// How much spawns: every point has a fixed number from 0 up to 100 (its "bucket"), the 32-bit
// FNV-1a hash of its guid, mixed once more (murmur3's finalizer) and scaled to [0, 100). A point is
// used for its kind while its bucket is below the kind's `percent`. The hash does not change, so
// the same points stay on when the percent goes up, and 0 turns a kind off and 100 uses all.
//
// When animals spawn: every 2 seconds (one interval for the whole population), for every player a
// point within `activationRadius` metres (2D) that has no animals and does not wait for its
// respawn spawns its `count` animals (at most 8) around its position, at most 2 m from it. A point
// whose nearest player is further than `despawnRadius` loses its animals (they are removed) and
// is free again at once. At most `maxAnimals` live at a time.
//
// When all animals of a point died, the point waits `respawnMinutes` of its kind before it spawns
// again; the bodies lie until then, or until the point is despawned. A partial loss is not
// remembered: a point that is despawned with some animals dead spawns its full count again.
//
// Settings without code: a server owner puts animal-population.json in the resources folder (see
// loadFile); a resource changes them at run time with Npc.setAnimalPopulation,
// Npc.setAnimalPopulationEnabled and Npc.setAnimalSpecies. Nothing runs per animal: a change of
// settings is looked at by the one tick, and only the points it touches change.
//
// Cost: the points sit in a grid of 100 m cells. A tick looks only at the cells around each player
// (and, for the despawn check, at a grid of the players), so it does not grow with the points
// times the players.
//
// The animals belong to no resource: they survive a resource's stop or restart. They are NPCs like
// any other (Npc.getById, the npcDeath and npcDestroy events work for them). Npc.removeAll() and
// npc.remove() remove them too; the point then spawns anew as soon as a player is near. They stop
// spawning when the host shuts down.

const fs = require('node:fs');
const path = require('node:path');
const spawnData = require('./spawn_points.json');
const { POPULATION_CLASSES, GLOBAL_WORLD, positionProblem } = require('../npcs/npc_values.cjs');

const LEVELS = ['trosecko', 'kutnohorsko'];
const TICK_MS = 2000;
const CELL_SIZE = 100;
// The most animals one point spawns.
const MAX_PER_POINT = 8;
// The animals of a group stand within this many metres of their point.
const GROUP_RADIUS = 2;
// A point whose spawn failed tries again after this long (ms), so one bad point does not fill the
// log every tick.
const RETRY_MS = 60000;
const MINUTE_MS = 60000;
// The longest respawn wait a setting may name: a week.
const MAX_RESPAWN_MINUTES = 10080;
// The file in the resources folder that holds the server owner's settings (see loadFile).
const CONFIG_FILE = 'animal-population.json';
const GOLDEN_ANGLE = 2.399963229728653;
const SOUL_GUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

const DEFAULT_KIND = { enabled: true, percent: 100, respawnMinutes: 30 };
const CONFIG_KEYS = [
  'enabled',
  'level',
  'activationRadius',
  'despawnRadius',
  'maxAnimals',
  'maxPerPoint',
  'kinds',
];
// A call may also name `respawnMinutes`: the wait of every kind that does not name its own. It is
// not stored as such, it lands in the kinds.
const PATCH_KEYS = [...CONFIG_KEYS, 'respawnMinutes'];
const KIND_KEYS = ['enabled', 'percent', 'respawnMinutes'];

// The settings a population starts with.
function defaultConfig() {
  const kinds = {};
  for (const kind of POPULATION_CLASSES) {
    kinds[kind] = { ...DEFAULT_KIND };
  }
  return {
    // Off until a resource switches it on: the clients make animal bodies only from 0.2.0's
    // native animal support on.
    enabled: false,
    // null: the points of every level (a server runs one level; set this to its name).
    level: null,
    activationRadius: 120,
    despawnRadius: 160,
    maxAnimals: 300,
    // The most animals one spawn point (an area) holds; a point asks for fewer, never for more.
    maxPerPoint: MAX_PER_POINT,
    kinds,
  };
}

function plainObject(value, what) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new TypeError(`${what} must be an object`);
  }
  return value;
}

function unknownKeys(given, allowed, what) {
  for (const key of Object.keys(given)) {
    if (!allowed.includes(key)) {
      throw new RangeError(`${what} has no setting "${key}"`);
    }
  }
}

function booleanSetting(value, what) {
  if (typeof value !== 'boolean') {
    throw new TypeError(`${what} must be true or false`);
  }
  return value;
}

// A finite number from `least` to `most` (`least` itself only when `includeLeast`).
function numberSetting(value, what, least, most, includeLeast) {
  if (typeof value !== 'number') {
    throw new TypeError(`${what} must be a number`);
  }
  const high = includeLeast ? value >= least : value > least;
  if (!(high && value <= most)) {
    const from = includeLeast ? 'from' : 'above';
    throw new RangeError(`${what} must be a number ${from} ${least} to ${most}`);
  }
  return value;
}

function integerSetting(value, what, least, most) {
  numberSetting(value, what, least, most, true);
  if (!Number.isInteger(value)) {
    throw new RangeError(`${what} must be a whole number`);
  }
  return value;
}

// The kinds part of a partial config: { [kind]: partial kind settings }, checked.
function kindsPatchOf(given) {
  const kinds = plainObject(given, 'Npc.setAnimalPopulation kinds');
  const patch = {};
  for (const [kind, settings] of Object.entries(kinds)) {
    if (!POPULATION_CLASSES.includes(kind)) {
      throw new RangeError(`Npc.setAnimalPopulation kinds has no animal kind "${kind}"`);
    }
    const what = `Npc.setAnimalPopulation kinds.${kind}`;
    // A plain number is the kind's percent: { Wolf: 20 }.
    if (typeof settings === 'number') {
      patch[kind] = { percent: numberSetting(settings, `${what} percent`, 0, 100, true) };
      continue;
    }
    plainObject(settings, what);
    unknownKeys(settings, KIND_KEYS, what);
    const one = {};
    if (settings.enabled !== undefined) {
      one.enabled = booleanSetting(settings.enabled, `${what}.enabled`);
    }
    if (settings.percent !== undefined) {
      one.percent = numberSetting(settings.percent, `${what}.percent`, 0, 100, true);
    }
    if (settings.respawnMinutes !== undefined) {
      one.respawnMinutes = numberSetting(
        settings.respawnMinutes,
        `${what}.respawnMinutes`,
        0,
        MAX_RESPAWN_MINUTES,
        true,
      );
    }
    patch[kind] = one;
  }
  return patch;
}

// The checked parts of a partial config. Throws TypeError / RangeError like Npc.create does.
function configPatchOf(given) {
  if (given === null || typeof given !== 'object' || Array.isArray(given)) {
    throw new TypeError('Npc.setAnimalPopulation takes a settings object');
  }
  const config = given;
  unknownKeys(config, PATCH_KEYS, 'Npc.setAnimalPopulation');
  const patch = {};
  if (config.enabled !== undefined) {
    patch.enabled = booleanSetting(config.enabled, 'Npc.setAnimalPopulation enabled');
  }
  if (config.level !== undefined) {
    if (config.level !== null && !LEVELS.includes(config.level)) {
      throw new RangeError(`Npc.setAnimalPopulation level must be null or one of ${LEVELS}`);
    }
    patch.level = config.level;
  }
  if (config.activationRadius !== undefined) {
    const what = 'Npc.setAnimalPopulation activationRadius';
    patch.activationRadius = numberSetting(config.activationRadius, what, 0, 2000, false);
  }
  if (config.despawnRadius !== undefined) {
    const what = 'Npc.setAnimalPopulation despawnRadius';
    patch.despawnRadius = numberSetting(config.despawnRadius, what, 0, 2000, false);
  }
  if (config.maxAnimals !== undefined) {
    patch.maxAnimals = integerSetting(
      config.maxAnimals,
      'Npc.setAnimalPopulation maxAnimals',
      0,
      5000,
    );
  }
  if (config.maxPerPoint !== undefined) {
    patch.maxPerPoint = integerSetting(
      config.maxPerPoint,
      'Npc.setAnimalPopulation maxPerPoint',
      1,
      MAX_PER_POINT,
    );
  }
  if (config.kinds !== undefined) {
    patch.kinds = kindsPatchOf(config.kinds);
  }
  if (config.respawnMinutes !== undefined) {
    // One wait for every kind; a kind that names its own keeps it.
    const minutes = numberSetting(
      config.respawnMinutes,
      'Npc.setAnimalPopulation respawnMinutes',
      0,
      MAX_RESPAWN_MINUTES,
      true,
    );
    patch.kinds = patch.kinds ?? {};
    for (const kind of POPULATION_CLASSES) {
      patch.kinds[kind] = { respawnMinutes: minutes, ...patch.kinds[kind] };
    }
  }
  return patch;
}

// `current` with the patch merged in (a new object); throws when the result is not allowed.
function mergedConfig(current, patch) {
  const next = { ...current, kinds: {} };
  for (const kind of POPULATION_CLASSES) {
    next.kinds[kind] = { ...current.kinds[kind], ...(patch.kinds?.[kind] ?? {}) };
  }
  for (const key of CONFIG_KEYS) {
    if (key !== 'kinds' && patch[key] !== undefined) {
      next[key] = patch[key];
    }
  }
  if (next.despawnRadius < next.activationRadius) {
    throw new RangeError(
      'Npc.setAnimalPopulation despawnRadius must not be smaller than activationRadius',
    );
  }
  return next;
}

// A deep, frozen copy of a config, for the reads.
function frozenCopy(config) {
  const kinds = {};
  for (const kind of POPULATION_CLASSES) {
    kinds[kind] = Object.freeze({ ...config.kinds[kind] });
  }
  return Object.freeze({ ...config, kinds: Object.freeze(kinds) });
}

// The stable bucket of a point: a number from 0 up to (not including) 100 from its id. The hash
// is FNV-1a over the id's characters (all ASCII: a guid like "c0d4f2a8-d045-4f2b"), then murmur3's
// finalizer so that the high and low bits mix, scaled from 32 bits to [0, 100).
function bucketOf(id) {
  let hash = 0x811c9dc5;
  for (let index = 0; index < id.length; index += 1) {
    hash ^= id.charCodeAt(index) & 0xff;
    hash = Math.imul(hash, 0x01000193);
  }
  hash ^= hash >>> 16;
  hash = Math.imul(hash, 0x85ebca6b);
  hash ^= hash >>> 13;
  hash = Math.imul(hash, 0xc2b2ae35);
  hash ^= hash >>> 16;
  return ((hash >>> 0) / 4294967296) * 100;
}

function cellOf(metres) {
  return Math.floor(metres / CELL_SIZE);
}

// A number that names the cell: the cells of a 16 km level are 0 to 163, a little room around.
function cellKey(cellX, cellY) {
  return (cellX + 512) * 1024 + (cellY + 512);
}

function clampCell(cell) {
  return Math.max(-512, Math.min(511, cell));
}

// Calls `visit(key)` for the cells that a circle of `radius` around (x, y) touches.
function forCellsAround(x, y, radius, visit) {
  const first = clampCell(cellOf(x - radius));
  const last = clampCell(cellOf(x + radius));
  const firstRow = clampCell(cellOf(y - radius));
  const lastRow = clampCell(cellOf(y + radius));
  for (let cellX = first; cellX <= last; cellX += 1) {
    for (let cellY = firstRow; cellY <= lastRow; cellY += 1) {
      visit(cellKey(cellX, cellY));
    }
  }
}

// The soul guid an entry's animals spawn with: its own `soulGuid` when that is a guid, else the
// default of its kind, else '' (no soul: the point is not spawned).
function soulGuidOf(entry, defaultSouls) {
  if (SOUL_GUID.test(entry.soulGuid ?? '')) {
    return entry.soulGuid.toLowerCase();
  }
  const fallback = defaultSouls?.[entry.kind];
  if (SOUL_GUID.test(fallback ?? '')) {
    return fallback.toLowerCase();
  }
  return '';
}

// The fixed facts of one spawn point, or null for an entry the population does not use.
function pointOf(entry, defaultSouls) {
  const usable = entry.type === 'spawner' || entry.type === 'placed';
  if (!usable || entry.streamed === true || !POPULATION_CLASSES.includes(entry.kind)) {
    return null;
  }
  if (!Array.isArray(entry.pos) || entry.pos.length !== 3) {
    return null;
  }
  const [x, y, z] = entry.pos;
  if (positionProblem({ x, y, z }) !== null || typeof entry.guid !== 'string') {
    return null;
  }
  const count = Number.isInteger(entry.count) ? entry.count : 1;
  const bucket = bucketOf(entry.guid);
  return {
    id: entry.guid,
    level: entry.level,
    kind: entry.kind,
    soul: soulGuidOf(entry, defaultSouls),
    x,
    y,
    z,
    count: Math.max(1, Math.min(MAX_PER_POINT, count)),
    bucket,
    // Turns the group's pattern, so the groups of different points do not all look alike.
    phase: (bucket / 100) * 2 * Math.PI,
  };
}

// Where animal `index` of a group of `count` stands around its point (metres from the point):
// a sunflower pattern within GROUP_RADIUS, the same every time.
function offsetOf(point, index, count) {
  if (count === 1) {
    return { x: 0, y: 0 };
  }
  const angle = point.phase + index * GOLDEN_ANGLE;
  const distance = GROUP_RADIUS * Math.sqrt((index + 0.5) / count);
  return { x: Math.cos(angle) * distance, y: Math.sin(angle) * distance };
}

class AnimalPopulation {
  // `registry`: the Npcs registry. `entries`: the spawn points (the game's, unless a test gives
  // its own).
  constructor(registry, entries = spawnData.entries) {
    this.registry = registry;
    this.config = defaultConfig();
    this.timer = null;
    this.setSpawnpoints(entries);
  }

  // Replaces the spawn points the population uses (entries as in spawn_points.json) and forgets
  // the animals' bookkeeping; the animals already spawned stay as plain NPCs. `defaultSouls`:
  // { kind: soul guid } for the entries that carry no soulGuid. Meant for tests and tools: the
  // game's own points and default souls are loaded by default.
  setSpawnpoints(entries, defaultSouls = spawnData.defaultSoulGuids) {
    // Animals still alive from before are removed so none is left without a point.
    this.clearAll();
    // cell key -> points
    this.grid = new Map();
    this.points = [];
    // The points that hold animals (living or dead).
    this.occupied = new Set();
    // NPC id -> point
    this.byNpc = new Map();
    // The kinds whose missing soul was logged already.
    this.noSoulLogged = new Set();
    for (const entry of entries) {
      const fixed = pointOf(entry, defaultSouls);
      if (fixed === null) {
        continue;
      }
      // waitingUntil: the time (ms) the point may spawn again, or null while it waits for nothing.
      const point = { ...fixed, animals: [], waitingUntil: null };
      this.points.push(point);
      const key = cellKey(clampCell(cellOf(point.x)), clampCell(cellOf(point.y)));
      if (!this.grid.has(key)) {
        this.grid.set(key, []);
      }
      this.grid.get(key).push(point);
    }
  }

  // Npc.animalPopulation: a frozen copy of the settings.
  get settings() {
    return frozenCopy(this.config);
  }

  // Npc.setAnimalPopulation: merges the checked partial settings in (all or nothing).
  configure(given) {
    this.config = mergedConfig(this.config, configPatchOf(given));
    return this.settings;
  }

  // Npc.setAnimalPopulationEnabled: switches the whole population on or off. Off removes the
  // animals at the next tick, on spawns them around the players again.
  setEnabled(enabled) {
    return this.configure({ enabled: booleanSetting(enabled, 'Npc.setAnimalPopulationEnabled') });
  }

  // Npc.setAnimalSpecies: changes one kind. `settings`: a percent, or { enabled, percent,
  // respawnMinutes } of which a call changes only what it names.
  setSpecies(kind, settings) {
    if (typeof kind !== 'string') {
      throw new TypeError('Npc.setAnimalSpecies takes the animal kind as a string');
    }
    return this.configure({ kinds: { [kind]: settings } });
  }

  // Reads the server owner's settings file (CONFIG_FILE in `folder`, the resources folder) and
  // applies it as Npc.setAnimalPopulation would: the same keys, a kind may be just its percent.
  //   { "enabled": true, "level": "trosecko", "maxAnimals": 200, "maxPerPoint": 4,
  //     "respawnMinutes": 20, "kinds": { "Wolf": 10, "Hare": { "percent": 50 } } }
  // A missing file changes nothing. A broken file or a setting the population refuses is logged
  // and leaves the settings as they were, so a typo cannot stop the server. Returns whether the
  // file was applied.
  loadFile(folder) {
    const file = path.join(folder, CONFIG_FILE);
    if (!fs.existsSync(file)) {
      return false;
    }
    try {
      this.configure(JSON.parse(fs.readFileSync(file, 'utf8')));
      return true;
    } catch (error) {
      this.registry.host.bridge.log('host', `animal population: ${CONFIG_FILE}: ${error}`);
      return false;
    }
  }

  // Starts the tick (once; a second call does nothing).
  start() {
    if (this.timer) {
      return;
    }
    this.timer = setInterval(() => this.run(), TICK_MS);
    this.timer.unref?.();
  }

  // Stops the tick. The animals stay.
  stop() {
    clearInterval(this.timer);
    this.timer = null;
  }

  // One tick, outside any resource (the animals belong to none) and never throwing: an error in
  // a timer would take the whole server down.
  run() {
    const host = this.registry.host;
    host.context.exit(() => {
      try {
        this.tick(Date.now());
      } catch (error) {
        host.bridge.log('host', `animal population: ${error?.stack ?? error}`);
      }
    });
  }

  // The positions of the players the animals are for: with a body, in the global world.
  playerPositions() {
    const found = [];
    for (const player of this.registry.host.players.values()) {
      if (!player.ready || player.dimension !== GLOBAL_WORLD) {
        continue;
      }
      const { x, y } = player.position;
      if (Number.isFinite(x) && Number.isFinite(y)) {
        found.push({ x, y });
      }
    }
    return found;
  }

  // Whether the settings want animals at this point at all.
  wanted(point) {
    const { enabled, level, kinds } = this.config;
    const kind = kinds[point.kind];
    return (
      enabled &&
      (level === null || level === point.level) &&
      kind.enabled &&
      point.bucket < kind.percent
    );
  }

  // The living animals of the population.
  aliveCount() {
    let alive = 0;
    for (const point of this.occupied) {
      for (const npc of point.animals) {
        if (npc.alive) {
          alive += 1;
        }
      }
    }
    return alive;
  }

  // Whether the point may spawn now: no animals (or only bodies) and no respawn wait left.
  free(point, now) {
    if (point.waitingUntil === null) {
      return point.animals.length === 0;
    }
    return now >= point.waitingUntil;
  }

  tick(now) {
    const players = this.playerPositions();
    this.despawnFar(players);
    if (this.config.enabled && players.length > 0) {
      this.spawnNear(players, now);
    }
  }

  // Removes the animals of the points no player is within `despawnRadius` of (or that the
  // settings no longer want).
  despawnFar(players) {
    if (this.occupied.size === 0) {
      return;
    }
    const grid = new Map();
    for (const player of players) {
      const key = cellKey(clampCell(cellOf(player.x)), clampCell(cellOf(player.y)));
      if (!grid.has(key)) {
        grid.set(key, []);
      }
      grid.get(key).push(player);
    }
    const radius = this.config.despawnRadius;
    for (const point of [...this.occupied]) {
      if (!this.wanted(point) || !playerWithin(grid, point, radius)) {
        this.clear(point);
      }
    }
  }

  // Spawns the points a player is within `activationRadius` of.
  spawnNear(players, now) {
    const { activationRadius, maxAnimals, maxPerPoint } = this.config;
    const squared = activationRadius * activationRadius;
    let alive = this.aliveCount();
    for (const player of players) {
      forCellsAround(player.x, player.y, activationRadius, (key) => {
        for (const point of this.grid.get(key) ?? []) {
          if (!this.free(point, now) || !this.wanted(point)) {
            continue;
          }
          const dx = point.x - player.x;
          const dy = point.y - player.y;
          const count = Math.min(point.count, maxPerPoint);
          if (dx * dx + dy * dy > squared || alive + count > maxAnimals) {
            continue;
          }
          if (point.soul === '') {
            this.logNoSoul(point.kind);
            continue;
          }
          alive += this.spawn(point, now);
        }
      });
    }
  }

  // Logs, once per kind, that its points are skipped because the kind has no soul guid.
  logNoSoul(kind) {
    if (this.noSoulLogged.has(kind)) {
      return;
    }
    this.noSoulLogged.add(kind);
    const text = `animal population: ${kind} has no soul guid, its spawn points are skipped`;
    this.registry.host.bridge.log('host', text);
  }

  // Spawns a point's animals; returns how many. The point's bodies from an earlier spawn go first.
  spawn(point, now) {
    this.clear(point);
    point.waitingUntil = null;
    const registry = this.registry;
    const spawned = [];
    const count = Math.min(point.count, this.config.maxPerPoint);
    try {
      for (let index = 0; index < count; index += 1) {
        const offset = offsetOf(point, index, count);
        const npc = registry.create(registry.Npc, {
          class: point.kind,
          soul: point.soul,
          name: '',
          position: { x: point.x + offset.x, y: point.y + offset.y, z: point.z },
          // A carcass is gutted, not searched: no Search beside Gut on its body. A
          // script that wants it searched sets npc.lootable = true.
          lootable: false,
        });
        spawned.push(npc);
        this.byNpc.set(npc.id, point);
      }
    } catch (error) {
      registry.host.bridge.log('host', `animal population: ${point.kind} ${point.id}: ${error}`);
      point.waitingUntil = now + RETRY_MS;
    }
    point.animals = spawned;
    if (spawned.length > 0) {
      this.occupied.add(point);
    }
    return spawned.length;
  }

  // Removes all animals of the point, living or dead. A respawn wait stays.
  clear(point) {
    const animals = point.animals;
    point.animals = [];
    this.occupied.delete(point);
    for (const npc of animals) {
      this.byNpc.delete(npc.id);
      this.registry.destroy(npc);
    }
  }

  clearAll() {
    for (const point of [...(this.occupied ?? [])]) {
      this.clear(point);
    }
  }

  // Npc.animals: the NPCs the population spawned (living and dead).
  animals() {
    return [...this.byNpc.keys()]
      .map((id) => this.registry.npcs.get(id))
      .filter((npc) => npc !== undefined);
  }

  // From the registry: an NPC died. When it was the last living animal of its point, the point
  // waits for its respawn.
  animalDied(npc) {
    const point = this.byNpc.get(npc.id);
    if (!point || point.animals.some((animal) => animal.alive)) {
      return;
    }
    const minutes = this.config.kinds[point.kind].respawnMinutes;
    point.waitingUntil = Date.now() + minutes * MINUTE_MS;
  }

  // From the registry: a dead animal was revived, so its point has a living animal again.
  animalRevived(npc) {
    const point = this.byNpc.get(npc.id);
    if (point) {
      point.waitingUntil = null;
    }
  }

  // From the registry: an NPC was removed (by a resource, or by this module's own clear, which
  // has forgotten it already).
  animalGone(npc) {
    const point = this.byNpc.get(npc.id);
    if (!point) {
      return;
    }
    this.byNpc.delete(npc.id);
    point.animals = point.animals.filter((animal) => animal !== npc);
    if (point.animals.length === 0) {
      this.occupied.delete(point);
    }
  }

  // Npc.animalSpots: a copy of the game's entries, those of `level` when one is given.
  static spawnpoints(level) {
    if (level !== undefined && !LEVELS.includes(level)) {
      throw new RangeError(`Npc.animalSpots level must be one of ${LEVELS}`);
    }
    const entries = spawnData.entries.filter(
      (entry) => level === undefined || entry.level === level,
    );
    return structuredClone(entries);
  }
}

// Whether a player of the cell grid is within `radius` metres (2D) of the point.
function playerWithin(grid, point, radius) {
  const squared = radius * radius;
  let found = false;
  forCellsAround(point.x, point.y, radius, (key) => {
    if (found) {
      return;
    }
    for (const player of grid.get(key) ?? []) {
      const dx = player.x - point.x;
      const dy = player.y - point.y;
      if (dx * dx + dy * dy <= squared) {
        found = true;
        return;
      }
    }
  });
  return found;
}

module.exports = { AnimalPopulation, LEVELS, bucketOf, defaultConfig, TICK_MS };
