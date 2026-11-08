'use strict';

// Herb gathering and regrowth: Gathering, gatheringHarvest and gatheringHarvested.
//
// A player picks the level's own plants as in single player. Their game hides the plants it
// picked (the pick's kind within its radius) and asks the server instead of giving the herb
// (op gatherReport). The server checks the player stands within reach, in the world the plants
// are picked in, that they were not picked already there and that the player does not pick too
// fast; gatheringHarvest may refuse it. A pick that stands gives the herb (change reason
// `gather`) and 5 Survival XP, hides the same plants on every game in the player's dimension
// (the picker's learns the pick's id), and raises gatheringHarvested after the
// playerInventoryChanged it caused. A refused
// pick's plants come back in the picker's game (op gatherVerdict).
//
// Picked plants grow back on the world clock (game hours): by default each kind in one fifth of
// the game's own time for it (kinds.json, built by Dev/tools/gathering/build_kinds.py), or within
// the bounds of Gathering.setRegrowth. Gathering.respawn and regrowEverywhere bring plants back at
// once. Nothing about gathering is saved: a restart brings every plant back.
//
// Each player's game gets the picks of its dimension when it enters one (op gatherPlants,
// full) and every pick and regrowth there after.
//
// The world's natural plants can be switched off, every kind or one (Gathering.setNaturalSpawns):
// a kind switched off is one pick of it around the origin covering the whole map, in
// every dimension, that never grows back on its own; every game hides those plants (and those
// that stream in later) and no player can pick them. Switched back on, the pick grows back.
//
// The level's own herbs (authored_herbs.cjs): Gathering.herbsWithin and
// Area.herbsWithin list the plants the level grows that are there to pick in a world, without a
// player near them; clearHerbs takes them away until Gathering.respawn, regrowEverywhere or
// Area.regrowHerbs brings them back. A removed plant is a pick of its own (that plant's kind, a
// hand's breadth around it) that never grows back on its own, so every game hides it as it hides
// a pick, late joiners too.

const catalog = require('./kinds.json');
const { loadAuthoredHerbs, AuthoredHerbs } = require('./authored_herbs.cjs');
const { Vector3 } = require('../math.cjs');
const { mapOf, onMap } = require('../maps/server_maps.cjs');
const { lookupItemClass } = require('../inventory/catalog.cjs');
const { propsOf } = require('../inventory/inventories.cjs');

// How far from the plants a player may stand to pick them, in metres.
const PICK_REACH = 5;
// The fewest real milliseconds between two picks of one player.
const PICK_INTERVAL_MS = 900;
// Survival XP a pick gives.
const PICK_XP = 5;
// The game's own regrowth time is divided by this by default.
const DEFAULT_SPEEDUP = 5;
// The longest regrowth time a script may set, in game hours.
const MAX_HOURS = 87600;
const SECONDS_PER_HOUR = 3600;
// Picks one world keeps, and picks or regrowths sent in one message.
const MAX_PICKS = 100000;
const PER_MESSAGE = 512;
const WORLD_REACH = 100000;
const GATHER_REASON = 'gather';
// A world left out is the global one, where every player starts (4294967295 in KCDS).
const GLOBAL_WORLD = 0xffffffff;
// A switched-off kind's pick: around the origin, over the whole map (kcd2s/integration/
// gathering.hpp kMaxGatherPickRadius).
const SWITCH_RADIUS = 100000;
// A removed plant's pick reaches this far around it (metres): the catalog keeps a plant's place
// to the centimetre.
const REMOVED_RADIUS = 0.1;

const KINDS = new Map(
  catalog.map((kind) => [
    kind.id,
    Object.freeze({ id: kind.id, name: kind.name, item: kind.item, hours: kind.respawnHours }),
  ]),
);

function finitePosition(position) {
  return (
    position &&
    [position.x, position.y, position.z].every(
      (value) => Number.isFinite(value) && Math.abs(value) <= WORLD_REACH,
    )
  );
}

function distance(a, b) {
  return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

class Gathering {
  // host: players, emit, bridge, worldState (the clock), playerInventories, areas (the level);
  // random() in [0, 1); loadHerbs(level): the level's AuthoredHerbs.
  constructor(host, random = Math.random, now = () => Date.now(), loadHerbs = loadAuthoredHerbs) {
    this.host = host;
    this.random = random;
    this.now = now;
    this.loadHerbs = loadHerbs;
    // Each map's level's own herbs: map index -> { level, herbs }, read when first asked.
    this.authoredByMap = new Map();
    // dimension -> Map(plant index -> the id of the pick that removed it)
    this.removedHerbs = new Map();
    // dimension -> Map(id -> { id, kind, position, radius, due (game seconds) or null })
    this.worlds = new Map();
    this.nextId = 1;
    // null: the kinds' defaults; else { minHours, maxHours }
    this.policy = null;
    // player id -> real time of their last pick
    this.lastPick = new Map();
    // player id -> the dimension their game was last given the picks of
    this.shownWorld = new Map();
    // kind id -> the pick that switches its natural plants off everywhere
    this.switchedOff = new Map();
  }

  // The kind ids a request names: one kind by name or id, or every kind for undefined/null.
  kindsOf(kind, where) {
    if (kind === undefined || kind === null) {
      return [...KINDS.keys()];
    }
    for (const entry of KINDS.values()) {
      if (entry.id === kind || entry.name === kind) {
        return [entry.id];
      }
    }
    throw new TypeError(`${where}: unknown herb kind ${String(kind)}`);
  }

  // Gathering.setNaturalSpawns: switches the natural plants of a kind (or every kind) on or off for
  // every player. How many kinds changed.
  setNaturalSpawns(enabled, kind) {
    const where = 'Gathering.setNaturalSpawns';
    if (typeof enabled !== 'boolean') {
      throw new TypeError(`${where}: enabled must be true or false`);
    }
    const picked = [];
    const grown = [];
    for (const id of this.kindsOf(kind, where)) {
      const pick = this.switchedOff.get(id);
      if (!enabled && !pick) {
        const off = {
          id: this.nextId++,
          kind: id,
          position: { x: 0, y: 0, z: 0 },
          radius: SWITCH_RADIUS,
          due: null,
        };
        this.switchedOff.set(id, off);
        picked.push(off);
      } else if (enabled && pick) {
        this.switchedOff.delete(id);
        grown.push(pick.id);
      }
    }
    if (picked.length > 0 || grown.length > 0) {
      for (const player of this.host.players.values()) {
        if (player.ready && this.shownWorld.has(player.id)) {
          this.send(player, false, picked, grown);
        }
      }
    }
    return picked.length + grown.length;
  }

  gameSeconds() {
    return this.host.worldState.clock.seconds();
  }

  picksOf(world) {
    let picks = this.worlds.get(world);
    if (!picks) {
      picks = new Map();
      this.worlds.set(world, picks);
    }
    return picks;
  }

  // When a pick of this kind grows back, in game seconds, or null when it does not on its own.
  dueFor(kind) {
    let hours = 0;
    if (this.policy === null) {
      hours = (KINDS.get(kind)?.hours ?? 0) / DEFAULT_SPEEDUP;
    } else {
      const { minHours, maxHours } = this.policy;
      hours = minHours + (maxHours - minHours) * this.random();
    }
    if (!(hours > 0)) {
      return null;
    }
    return this.gameSeconds() + hours * SECONDS_PER_HOUR;
  }

  // The world a player's picks are kept in: their world on their map (the native value), so a
  // pick on one map never hides a plant at the same place of another map's level.
  worldOfPlayer(player) {
    return onMap(this.host.playerMapIndex?.(player) ?? 0, player.dimension);
  }

  playersIn(world) {
    return [...this.host.players.values()].filter(
      (player) => player.ready && this.worldOfPlayer(player) === world,
    );
  }

  pickEntry(pick) {
    return [pick.id, pick.kind, pick.position.x, pick.position.y, pick.position.z, pick.radius];
  }

  // Plants picked and grown back in a world, to every player there.
  tell(world, picked, grown) {
    for (const player of this.playersIn(world)) {
      this.send(player, false, picked, grown);
    }
  }

  send(player, full, picked, grown) {
    let first = true;
    for (let start = 0; first || start < picked.length; start += PER_MESSAGE) {
      this.host.bridge.send({
        op: 'gatherPlants',
        id: player.id,
        full: full && first,
        picked: picked.slice(start, start + PER_MESSAGE).map((pick) => this.pickEntry(pick)),
        grown: first ? grown : [],
      });
      first = false;
    }
  }

  // The picks of the player's world to their game, whose world is ready (again).
  // A game that was shown no world before hides nothing yet, so an empty world needs no message.
  arrived(player) {
    const world = this.worldOfPlayer(player);
    const shownBefore = this.shownWorld.has(player.id);
    const picks = [...this.picksOf(world).values(), ...this.switchedOff.values()];
    this.shownWorld.set(player.id, world);
    if (!shownBefore && picks.length === 0) {
      return;
    }
    this.send(player, true, picks, []);
  }

  // A player who moved to another dimension gets its picks.
  worldChanged(player) {
    if (player.ready && this.shownWorld.get(player.id) !== this.worldOfPlayer(player)) {
      this.arrived(player);
    }
  }

  left(player) {
    this.lastPick.delete(player.id);
    this.shownWorld.delete(player.id);
  }

  // Whether a plant of this kind at `position` lies in a pick of the world that has not grown back.
  picked(world, kind, position) {
    if (this.switchedOff.has(kind)) {
      return true;
    }
    for (const pick of this.picksOf(world).values()) {
      if (pick.kind === kind && distance(pick.position, position) <= pick.radius + 0.05) {
        return true;
      }
    }
    return false;
  }

  // The player's game picked plants (op gatherReport): { request, item, amount, kind, position,
  // radius, plants }. Whether the pick stands; the verdict goes to their game either way.
  report(player, report) {
    if (!player || this.host.players.get(player.id) !== player || !player.ready) {
      return false;
    }
    const verdict = (granted) => {
      this.host.bridge.send({
        op: 'gatherVerdict',
        id: player.id,
        request: report.request,
        granted,
      });
      return granted;
    };
    const kind = KINDS.get(report.kind);
    const position = Array.isArray(report.position)
      ? { x: report.position[0], y: report.position[1], z: report.position[2] }
      : report.position;
    const itemClass = lookupItemClass(report.item);
    const world = this.worldOfPlayer(player);
    if (
      !kind ||
      !itemClass ||
      itemClass.id !== kind.item ||
      !finitePosition(position) ||
      !Number.isInteger(report.amount) ||
      report.amount < 1 ||
      !Number.isFinite(report.radius) ||
      report.radius < 0
    ) {
      return verdict(false);
    }
    const now = this.now();
    const last = this.lastPick.get(player.id);
    if (last !== undefined && now - last < PICK_INTERVAL_MS) {
      return verdict(false);
    }
    if (
      !finitePosition(player.position) ||
      distance(player.position, position) > PICK_REACH ||
      this.picked(world, kind.id, position) ||
      this.picksOf(world).size >= MAX_PICKS
    ) {
      return verdict(false);
    }
    const proposal = Object.freeze({
      item: itemClass.id,
      amount: report.amount,
      kind: kind.id,
      position: Object.freeze([position.x, position.y, position.z]),
      dimension: player.dimension,
    });
    if (!this.host.emit('gatheringHarvest', player, proposal)) {
      return verdict(false);
    }
    const { props } = propsOf(itemClass, undefined);
    const given = this.host.playerInventories.operate(
      player,
      (inventory) => inventory.addUnits(itemClass, props, report.amount, 0, GATHER_REASON),
      { notify: true },
    );
    if (!given.ok) {
      return verdict(false);
    }
    this.lastPick.set(player.id, now);
    const pick = {
      id: this.nextId++,
      kind: kind.id,
      position: Object.freeze({ ...position }),
      radius: report.radius,
      due: this.dueFor(kind.id),
    };
    this.picksOf(world).set(pick.id, pick);
    verdict(true);
    player.addXp?.('survival', PICK_XP);
    // Every game in the world, the picker's too: it learns the pick's id for its regrowth.
    this.tell(world, [pick], []);
    this.host.playerInventories.flush((entry) => entry.player === player);
    this.host.emit(
      'gatheringHarvested',
      player,
      Object.freeze({ ...proposal, items: Object.freeze([...given.items]) }),
    );
    return true;
  }

  // The level's own herbs (an empty catalog when the level has none or it cannot be read). The
  // host reads them when it learns its level; how long that took and how many there are go to
  // the log.
  // A server of several maps keeps one catalog for each map's level (`mapIndex`, the first by
  // default).
  herbs(mapIndex = 0) {
    const level =
      mapIndex === 0
        ? this.host.areas?.level ?? null
        : this.host.areas?.levels?.get(mapIndex) ?? null;
    const kept = this.authoredByMap.get(mapIndex);
    if (kept && kept.level === level) {
      return kept.herbs;
    }
    const started = Date.now();
    let herbs;
    try {
      herbs = level === null ? new AuthoredHerbs() : this.loadHerbs(level);
    } catch (error) {
      this.host.bridge?.log?.('host', `Herbs: ${error.message}`);
      herbs = new AuthoredHerbs();
    }
    if (level !== null) {
      this.host.bridge?.log?.(
        'host',
        `Herbs: ${herbs.size} plants of ${level} read in ${Date.now() - started} ms`,
      );
    }
    this.authoredByMap.set(mapIndex, { level, herbs });
    return herbs;
  }

  // Whether a plant is taken in a world: picked and not grown back, removed, or of a kind switched
  // off. The test keeps the world's picks by kind for the plants it is asked about.
  takenIn(world) {
    const byKind = new Map();
    for (const pick of this.picksOf(world).values()) {
      let list = byKind.get(pick.kind);
      if (!list) {
        list = [];
        byKind.set(pick.kind, list);
      }
      list.push(pick);
    }
    return (kind, position) =>
      this.switchedOff.has(kind) ||
      (byKind.get(kind) ?? []).some(
        (pick) => distance(pick.position, position) <= pick.radius + 0.05,
      );
  }

  // The plants at these indices that are there to pick in a world, as GatheringHerb.
  available(world, indices) {
    // The indices are of the catalog of the world's map.
    const herbs = this.herbs(mapOf(world));
    const taken = this.takenIn(world);
    const found = [];
    for (const index of indices) {
      const kind = KINDS.get(herbs.kindOf(index));
      const at = herbs.positionOf(index);
      if (!kind || taken(kind.id, at)) {
        continue;
      }
      found.push(
        Object.freeze({ kind: kind.id, item: kind.item, position: new Vector3(at.x, at.y, at.z) }),
      );
    }
    return found;
  }

  // Removes the plants at these indices in a world, picked or not, until they are respawned: each
  // a pick that never grows back on its own, hidden on every game there. How many were not removed
  // already.
  clearHerbs(world, indices) {
    const herbs = this.herbs(mapOf(world));
    let removed = this.removedHerbs.get(world);
    if (!removed) {
      removed = new Map();
      this.removedHerbs.set(world, removed);
    }
    const picks = this.picksOf(world);
    const added = [];
    for (const index of indices) {
      const kind = herbs.kindOf(index);
      if (removed.has(index) || !KINDS.has(kind) || picks.size >= MAX_PICKS) {
        continue;
      }
      const pick = {
        id: this.nextId++,
        kind,
        position: Object.freeze(herbs.positionOf(index)),
        radius: REMOVED_RADIUS,
        due: null,
        herb: index,
      };
      picks.set(pick.id, pick);
      removed.set(index, pick.id);
      added.push(pick);
    }
    for (let start = 0; start < added.length; start += PER_MESSAGE) {
      this.tell(world, added.slice(start, start + PER_MESSAGE), []);
    }
    return added.length;
  }

  // Picks of a world that grow back now, `which` choosing them: the plants come back on every
  // game there, removed plants too. How many.
  grow(world, which) {
    const picks = this.picksOf(world);
    const grown = [];
    for (const pick of [...picks.values()]) {
      if (which(pick)) {
        picks.delete(pick.id);
        grown.push(pick.id);
        if (pick.herb !== undefined) {
          this.removedHerbs.get(world)?.delete(pick.herb);
        }
      }
    }
    if (grown.length > 0) {
      for (let start = 0; start < grown.length; start += PER_MESSAGE) {
        this.tell(world, [], grown.slice(start, start + PER_MESSAGE));
      }
    }
    return grown.length;
  }

  // Picks whose time has come grow back.
  tick() {
    const now = this.gameSeconds();
    for (const world of this.worlds.keys()) {
      this.grow(world, (pick) => pick.due !== null && pick.due <= now);
    }
  }

  // Every pick's regrowth starts again from now under the current policy; removed plants stay
  // removed.
  restartAll() {
    for (const picks of this.worlds.values()) {
      for (const pick of picks.values()) {
        if (pick.herb === undefined) {
          pick.due = this.dueFor(pick.kind);
        }
      }
    }
  }

  api() {
    const gathering = this;
    const worldOf = (world, where) => {
      if (world === undefined) {
        return GLOBAL_WORLD;
      }
      if (!Number.isInteger(world) || world < 0 || world > 0xffffffff) {
        throw new TypeError(`${where}: dimension must be a whole number`);
      }
      return world;
    };
    const sphereOf = (position, radius, where) => {
      if (!finitePosition(position)) {
        throw new TypeError(`${where}: position must be a { x, y, z } within ±${WORLD_REACH}`);
      }
      if (!Number.isFinite(radius) || radius < 0 || radius > WORLD_REACH) {
        throw new TypeError(`${where}: radius must be from 0 to ${WORLD_REACH}`);
      }
      return gathering.herbs().within(position, radius);
    };
    return Object.freeze({
      // the level's own plants within the sphere that are there to pick in the world.
      herbsWithin(position, radius, dimension) {
        const where = 'Gathering.herbsWithin';
        const plants = sphereOf(position, radius, where);
        return gathering.available(worldOf(dimension, where), plants);
      },
      // The level's own plants within the sphere taken away in the world, picked ones too, until a
      // respawn. How many were not removed already.
      clearHerbs(position, radius, dimension) {
        const where = 'Gathering.clearHerbs';
        const plants = sphereOf(position, radius, where);
        return gathering.clearHerbs(worldOf(dimension, where), plants);
      },
      get regrowTime() {
        return gathering.policy === null ? null : Object.freeze({ ...gathering.policy });
      },
      setRegrowth(minHours, maxHours) {
        const valid =
          Number.isFinite(minHours) &&
          Number.isFinite(maxHours) &&
          minHours >= 0 &&
          minHours <= maxHours &&
          maxHours <= MAX_HOURS &&
          (maxHours === 0 || maxHours * SECONDS_PER_HOUR * 1000 >= 1);
        if (!valid) {
          return false;
        }
        gathering.policy = { minHours, maxHours };
        gathering.restartAll();
        return true;
      },
      resetRegrowth() {
        gathering.policy = null;
        gathering.restartAll();
      },
      respawn(position, radius, dimension) {
        const where = 'Gathering.respawn';
        if (!finitePosition(position)) {
          throw new TypeError(`${where}: position must be a { x, y, z } within ±${WORLD_REACH}`);
        }
        if (!Number.isFinite(radius) || radius < 0 || radius > WORLD_REACH) {
          throw new TypeError(`${where}: radius must be from 0 to ${WORLD_REACH}`);
        }
        const world = worldOf(dimension, where);
        return gathering.grow(world, (pick) => distance(pick.position, position) <= radius);
      },
      regrowEverywhere(dimension) {
        return gathering.grow(worldOf(dimension, 'Gathering.regrowEverywhere'), () => true);
      },
      // the world's natural plants of a kind (name or id; every kind when left out) on
      // or off for every player, late joiners too. How many kinds changed.
      setNaturalSpawns(enabled, kind) {
        return gathering.setNaturalSpawns(enabled, kind);
      },
      // The kinds whose natural plants are off, by name.
      naturalSpawnsOff() {
        return [...gathering.switchedOff.keys()].map((id) => KINDS.get(id).name);
      },
    });
  }
}

module.exports = { Gathering, KINDS, PICK_REACH };
