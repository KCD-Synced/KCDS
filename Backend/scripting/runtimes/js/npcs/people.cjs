'use strict';

// The level's own people. The game's own NPCs stay
// switched off on every client (Client/runtime/npcs/npc_suppression.hpp), since each client would
// run different ones. This module spawns them as the server's NPCs from where the level places them
// (npcs/placements.json.gz, by Dev/tools/npcs/build_placements.py), so every player sees the same
// people: a placement within `activationRadius` of a player gets a body, and loses it when the nearest
// player is past `despawnRadius`. At most `maxPeople` live at a time, the placements nearest a player
// first.
//
// A quest's staged people are left out (STAGED_LAYER). They start where the level first places them,
// and walk an unhurried round of the places of up to two neighbours, as a patrol a script may replace
// with any other order, with the game's brain on the simulator's body. The
// level's schedules are not theirs: a schedule's links are level entities, and these bodies are made at
// run time.
//
// With server.json levelPeople on (issue 016) the people are the level's own: every game lets the
// level spawn them, and the NPC this module makes for one only names it (its level GUID); every
// game then uses its own entity of that person as the body, the simulator's with its brain and
// the level's schedule, the others' mirroring it. They get no round to walk and a wider radius,
// so a body does not change hands within a player's sight.
//
// Npc.adopt(placedGuid, options) gives one placement to a script: its body stays whoever is near, and
// the options are those of Npc.create. Settings: Npc.setPopulation at run time, or people-population.json
// in the resources folder. They belong to no resource and survive a resource's restart.

const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const { PeopleLines } = require('./people_lines.cjs');
const { PeopleNight } = require('./people_night.cjs');
const { addLevelPlaces } = require('./object_use.cjs');

const CATALOG = path.join(__dirname, 'placements.json.gz');
const SETTINGS_FILE = 'people-population.json';
const PLACEMENTS_FILE = 'people-placements.json';
const PLACES_FILE = 'level-places.json';
const CELL_METRES = 100;
const TICK_MS = 2000;
const DEFAULTS = Object.freeze({
  enabled: true,
  activationRadius: 80,
  despawnRadius: 120,
  maxPeople: 48,
  // Percent of the level's people who get bodies.
  share: 100,
});
const MOST_PEOPLE = 512;
// The level's own people (levelPeople): near enough to be seen, and how many at once.
const LEVEL_PEOPLE_DEFAULTS = Object.freeze({
  activationRadius: 150,
  despawnRadius: 180,
  maxPeople: 128,
});
const NEIGHBOUR_NEAREST = 6;
// How far up or down a neighbour of an added person may stand to be on their floor, metres.
const FLOOR_METRES = 2;
// How long someone on their round stands still before they are taken as stuck, ms.
const STUCK_MS = 90000;
const NEIGHBOUR_FARTHEST = 45;
const SHORTEST_WAIT_SECONDS = 15;
const LONGEST_EXTRA_WAIT_SECONDS = 30;
// Layers of people a quest stages: an army, a siege, a castle closed for a while, the dead of a battle.
// The level shows them only during that quest, so they are no one's everyday. Npc.adopt still has them.
const STAGED_LAYER =
  /_quest|series_|\/streamed|closedCastle|siege|battle|deadIn|endgame|\/m\d\d_|inM\d\d|prisoners|capturedminers|neoznaceneHroby/i;

// Stand-ins the level brings in only when the person they stand for is away (a replacement
// guard with the guard's own outfit beside him). The game's own people
// (levelPeople) keep them: the game decides there.
const STAND_IN_LAYER = /replacement|standin/i;
// The level keeps many of its people at a storage spot, from where the game's schedule walks
// them to their places at once: 30 in a line in Trosky's lower courtyard, Kutná Hora's street
// extras. The server's own people cannot follow those schedules, so a
// crowd (people of one layer, each within CROWD_METRES of another) gets at most CROWD_KEEP bodies.
const CROWD_METRES = 2;
const CROWD_KEEP = 2;

// The rows to give bodies, each layer's crowds cut to their first CROWD_KEEP rows in the
// catalog's order. Npc.adopt still has every row.
function thinnedCrowds(rows) {
  const byLayer = new Map();
  for (const row of rows) {
    const list = byLayer.get(row.layer) ?? [];
    list.push(row);
    byLayer.set(row.layer, list);
  }
  const kept = new Set();
  for (const list of byLayer.values()) {
    // Each row's crowd: rows closer than CROWD_METRES are joined (union-find).
    const parent = list.map((_, index) => index);
    const crowdOf = (index) => {
      while (parent[index] !== index) {
        parent[index] = parent[parent[index]];
        index = parent[index];
      }
      return index;
    };
    for (let a = 0; a < list.length; a += 1) {
      for (let b = a + 1; b < list.length; b += 1) {
        const one = list[a].position;
        const other = list[b].position;
        if (Math.hypot(one.x - other.x, one.y - other.y) < CROWD_METRES) {
          parent[crowdOf(a)] = crowdOf(b);
        }
      }
    }
    const given = new Map();
    list.forEach((row, index) => {
      const crowd = crowdOf(index);
      const count = given.get(crowd) ?? 0;
      if (count < CROWD_KEEP) {
        kept.add(row);
      }
      given.set(crowd, count + 1);
    });
  }
  return rows.filter((row) => kept.has(row));
}

// A walker's stop around a place: AROUND_NEAREST to AROUND_FARTHEST metres from it, the angle
// and distance from the walker's seed.
const AROUND_NEAREST = 1.5;
const AROUND_FARTHEST = 2.5;

function aroundPlace(position, seed) {
  const angle = ((seed & 0xff) / 256) * 2 * Math.PI;
  const distance = AROUND_NEAREST + (((seed >>> 8) & 0x7) / 7) * (AROUND_FARTHEST - AROUND_NEAREST);
  return {
    x: position.x + Math.cos(angle) * distance,
    y: position.y + Math.sin(angle) * distance,
    z: position.z,
  };
}

let catalog = null;
// People a server adds for a level (people-placements.json in the resources folder), by level:
// a map the game's tables do not place people on, such as the KCD1 map, made from the player's own
// files and never shipped with the server.
const added = new Map();

function placementsOf(level) {
  if (catalog === null) {
    catalog = JSON.parse(zlib.gunzipSync(fs.readFileSync(CATALOG)).toString('utf8'));
  }
  return [...(catalog[level] ?? []), ...(added.get(level) ?? [])];
}

function isNumber(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function isPoint(value) {
  return (
    value !== null && typeof value === 'object' && [value.x, value.y, value.z].every(isNumber)
  );
}

// A placement row as the catalog writes one; null when it is not one.
function checkedPlacement(row, level) {
  if (row === null || typeof row !== 'object') {
    return null;
  }
  const { guid, name, soul, actorClass, layer, position, rotation } = row;
  const known = actorClass === 'NPC' || actorClass === 'NPC_Female';
  if (typeof guid !== 'string' || typeof soul !== 'string' || !known) {
    return null;
  }
  if (!position || !isNumber(position.x) || !isNumber(position.y) || !isNumber(position.z)) {
    return null;
  }
  const turn = rotation ?? { w: 1, x: 0, y: 0, z: 0 };
  if (![turn.w, turn.x, turn.y, turn.z].every(isNumber)) {
    return null;
  }
  return {
    guid,
    name: typeof name === 'string' ? name : guid,
    soul,
    actorClass,
    layer: typeof layer === 'string' ? layer : `${level}/added`,
    // Its round keeps to its own floor (an added level's people are often upstairs in a house).
    added: true,
    position: { x: position.x, y: position.y, z: position.z },
    rotation: { w: turn.w, x: turn.x, y: turn.y, z: turn.z },
    // What the person wears (an outfit preset as Npc.create takes it): the game dresses a level's
    // people by its schedules, which know nothing of added people.
    ...(typeof row.outfit === 'string' && row.outfit !== '' ? { outfit: row.outfit } : {}),
    // Whether looking at the person names them and the use key reaches scripts (npcInteract): the
    // game's HUD names only someone it can interact with.
    ...(row.interactable === true ? { interactable: true } : {}),
    // Their day: the place they work at (they walk between it and their own place) and the bed
    // they sleep in at night (a level place's GUID).
    ...(isPoint(row.work) ? { work: { x: row.work.x, y: row.work.y, z: row.work.z } } : {}),
    ...(typeof row.bed === 'string' && row.bed !== '' ? { bed: row.bed } : {}),
  };
}

// The people of people-placements.json ({ "<level>": [placement, ...] }); rows that are not
// placements are left out. The number of rows taken.
function addPlacements(byLevel) {
  added.clear();
  let count = 0;
  for (const [level, rows] of Object.entries(byLevel ?? {})) {
    if (!Array.isArray(rows)) {
      continue;
    }
    const kept = rows.map((row) => checkedPlacement(row, level)).filter((row) => row !== null);
    added.set(level, kept);
    count += kept.length;
  }
  return count;
}

function cellOf(x, y) {
  return `${Math.floor(x / CELL_METRES)}:${Math.floor(y / CELL_METRES)}`;
}

function checkedSettings(given, base) {
  if (given === null || typeof given !== 'object') {
    throw new TypeError('Npc.setPopulation takes an object');
  }
  const next = { ...base };
  if (given.enabled !== undefined) {
    if (typeof given.enabled !== 'boolean') {
      throw new TypeError('Npc.setPopulation: enabled is true or false');
    }
    next.enabled = given.enabled;
  }
  for (const name of ['activationRadius', 'despawnRadius']) {
    if (given[name] !== undefined) {
      if (typeof given[name] !== 'number' || !(given[name] > 0)) {
        throw new RangeError(`Npc.setPopulation: ${name} is a number of metres above 0`);
      }
      next[name] = given[name];
    }
  }
  if (given.maxPeople !== undefined) {
    const count = given.maxPeople;
    if (!Number.isInteger(count) || count < 0 || count > MOST_PEOPLE) {
      throw new RangeError(
        `Npc.setPopulation: maxPeople is a whole number from 0 to ${MOST_PEOPLE}`,
      );
    }
    next.maxPeople = count;
  }
  if (given.share !== undefined) {
    if (typeof given.share !== 'number' || !(given.share >= 0 && given.share <= 100)) {
      throw new RangeError('Npc.setPopulation: share is a percentage from 0 to 100');
    }
    next.share = given.share;
  }
  if (next.despawnRadius < next.activationRadius) {
    throw new RangeError('Npc.setPopulation: despawnRadius is at least activationRadius');
  }
  return next;
}

// A number of the placement's own, so a person walks the same round for everyone and every time.
function seedOf(guid) {
  return parseInt(guid.slice(-8), 16) || 0;
}

// Whether a placement is in the population's share (percent): the same
// placements for every player and every start, by a number of its own spread evenly over 0-9999.
function inShare(guid, share) {
  if (share >= 100) {
    return true;
  }
  const spread = Math.imul(parseInt(guid.slice(0, 8), 16) || 0, 2654435761) >>> 0;
  return spread % 10000 < share * 100;
}

class People {
  constructor(host) {
    this.host = host;
    // The level's own people (server.json levelPeople, which the server hands its scripts).
    this.levelPeople = host.levelPeople ?? process.env.KCDS_SERVER_LEVEL_PEOPLE === 'on';
    this.settings = this.levelPeople ? { ...DEFAULTS, ...LEVEL_PEOPLE_DEFAULTS } : { ...DEFAULTS };
    // guid -> { npc, adopted }
    this.bodies = new Map();
    // Where each person last was when its body went for being far from every player (guid -> {x,
    // y}): the level's people walk their schedules away from where the level places them, and
    // measured from the placement such a person was spawned again at once and removed again,
    // every tick (one person 346 times).
    this.lastSeen = new Map();
    this.level = null;
    this.byGuid = new Map();
    this.grid = new Map();
    this.timer = null;
    // Where they spend the night (server.json npcHouses): without houses they walk
    // out of the map at night.
    this.night = new PeopleNight({
      houses: host.npcHouses ?? process.env.KCDS_SERVER_NPC_HOUSES !== 'off',
      hour: () => {
        const { World } = this.api();
        return typeof World.hour === 'function' ? World.hour() : World.hour;
      },
      level: () => this.level,
      log: (text) => this.host.log?.(text),
    });
    // What the people say to players near them (people-lines.json).
    this.lines = new PeopleLines({
      bodies: () => this.speakers(),
      log: (text) => this.host.log?.(text),
    });
  }

  // The people who may say a line: not one a script adopted (its lines are the script's).
  speakers() {
    const list = [];
    for (const [guid, body] of this.bodies) {
      if (!body.adopted) {
        list.push({ npc: body.npc, name: this.byGuid.get(guid)?.name ?? '' });
      }
    }
    return list;
  }

  loadFile(root) {
    this.night.loadFile(root);
    this.lines.loadFile(root);
    const places = path.join(root, PLACES_FILE);
    if (fs.existsSync(places)) {
      try {
        const count = addLevelPlaces(JSON.parse(fs.readFileSync(places, 'utf8')));
        this.host.log?.(`${PLACES_FILE}: ${count} places added`);
      } catch (error) {
        this.host.log?.(`${PLACES_FILE}: ${error.message}`);
      }
    }
    const placements = path.join(root, PLACEMENTS_FILE);
    if (fs.existsSync(placements)) {
      try {
        const count = addPlacements(JSON.parse(fs.readFileSync(placements, 'utf8')));
        this.level = undefined;
        this.host.log?.(`${PLACEMENTS_FILE}: ${count} people added`);
      } catch (error) {
        this.host.log?.(`${PLACEMENTS_FILE}: ${error.message}`);
      }
    }
    const file = path.join(root, SETTINGS_FILE);
    if (!fs.existsSync(file)) {
      return;
    }
    try {
      this.settings = checkedSettings(JSON.parse(fs.readFileSync(file, 'utf8')), this.settings);
    } catch (error) {
      this.host.log?.(`${SETTINGS_FILE}: ${error.message}`);
    }
  }

  api() {
    return this.host.api ?? this.host.makeAPI();
  }

  start() {
    if (!this.timer) {
      this.timer = setInterval(() => this.tick(), TICK_MS);
      this.timer.unref?.();
    }
  }

  stop() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }

  // Npc.setPopulation.
  set(given) {
    this.settings = checkedSettings(given, this.settings);
    this.tick();
  }

  // The level's placements in cells, once per level and share.
  index() {
    const level = this.host.areas?.level ?? null;
    const share = this.levelPeople ? 100 : this.settings.share;
    if (level === this.level && share === this.indexedShare) {
      return;
    }
    this.level = level;
    this.indexedShare = share;
    this.byGuid.clear();
    this.grid.clear();
    const shown = [];
    for (const row of placementsOf(level)) {
      this.byGuid.set(row.guid, row);
      if (STAGED_LAYER.test(row.layer)) {
        continue;
      }
      if (!this.levelPeople && STAND_IN_LAYER.test(row.layer)) {
        continue;
      }
      shown.push(row);
    }
    const kept = new Set();
    // The share is taken of the people as they are, crowds thinned: a smaller one only takes
    // people away.
    for (const row of this.levelPeople ? shown : thinnedCrowds(shown)) {
      if (!inShare(row.guid, share)) {
        continue;
      }
      kept.add(row.guid);
      const cell = cellOf(row.position.x, row.position.y);
      const list = this.grid.get(cell) ?? [];
      list.push(row);
      this.grid.set(cell, list);
    }
    // A smaller share takes away the bodies of those left out (not those a script adopted).
    for (const [guid, body] of [...this.bodies]) {
      if (!body.adopted && !kept.has(guid)) {
        body.npc.remove();
        this.bodies.delete(guid);
      }
    }
  }

  // The placements near any player that have no body yet, nearest first, and the distance of the
  // nearest player from each living body.
  tick() {
    this.index();
    const players = [...this.host.players.values()].filter(
      (player) => player.ready && player.position,
    );
    this.removeFar(players);
    if (!this.settings.enabled) {
      return;
    }
    if (this.night.away()) {
      this.leaveForTheNight();
      return;
    }
    const wanted = this.wantedNear(players);
    let room = this.settings.maxPeople - this.populationCount();
    for (const { row } of wanted) {
      if (room <= 0) {
        break;
      }
      if (this.spawn(row)) {
        room -= 1;
      }
    }
    this.keepRoutines();
    this.lines.tick(players);
  }

  // The night of people with a bed of their own (a placement's bed): they lie in it at night and
  // get up in the morning to walk their round again.
  keepRoutines() {
    const night = this.night.isNightNow();
    const now = Date.now();
    for (const [guid, body] of this.bodies) {
      const row = this.byGuid.get(guid);
      if (body.adopted || !row) {
        continue;
      }
      this.unstick(body, row, now);
      if (!row.bed) {
        continue;
      }
      if (night && body.inBed && this.bedOutOfReach(body, now)) {
        // Not even started towards the bed (a door it cannot pass): its round tonight instead.
        body.inBed = false;
        body.noBedTonight = true;
        body.npc.stopUsingObject();
        this.walkRound(body.npc, row, { withoutWork: body.withoutWork === true });
      } else if (night && !body.inBed && !body.noBedTonight) {
        const place = this.host.objectUses?.levelPlaceByGuid(row.bed);
        if (place && place.free) {
          body.inBed = this.useBed(body.npc, place);
        }
      } else if (!night && (body.inBed || body.noBedTonight)) {
        const wasInBed = body.inBed;
        body.inBed = false;
        body.noBedTonight = false;
        if (!wasInBed) {
          continue;
        }
        body.npc.stopUsingObject();
        this.walkRound(body.npc, row, { withoutWork: body.withoutWork === true });
      }
    }
  }

  // Whether someone on their way to their bed has stood still for STUCK_MS.
  bedOutOfReach(body, now) {
    if (!this.host.objectUses?.walking(body.npc)) {
      body.bedWalk = null;
      return false;
    }
    const at = body.npc.position;
    if (!body.bedWalk || Math.hypot(at.x - body.bedWalk.x, at.y - body.bedWalk.y) > 0.5) {
      body.bedWalk = { x: at.x, y: at.y, since: now };
      return false;
    }
    return now - body.bedWalk.since >= STUCK_MS;
  }

  // Someone on their round who has not moved for STUCK_MS (longer than any pause) cannot reach
  // where they go, such as a work place off the paths: they walk the neighbours' round instead.
  unstick(body, row, now) {
    if (body.inBed || body.npc.usingObject) {
      body.still = null;
      return;
    }
    const at = body.npc.position;
    if (!body.still || Math.hypot(at.x - body.still.x, at.y - body.still.y) > 0.5) {
      body.still = { x: at.x, y: at.y, since: now };
      return;
    }
    if (now - body.still.since < STUCK_MS || body.withoutWork) {
      return;
    }
    body.withoutWork = true;
    body.still = { x: at.x, y: at.y, since: now };
    this.walkRound(body.npc, row, { withoutWork: true });
  }

  useBed(npc, place) {
    try {
      return npc.useObject(place) !== false;
    } catch (error) {
      this.host.log?.(`people: ${npc.name} cannot lie in bed ${place.guid}: ${error.message}`);
      return false;
    }
  }

  // The night without houses: everyone not adopted walks to the nearest exit of the map and goes
  // there; nobody is spawned until the morning, when they stand at their places again.
  leaveForTheNight() {
    const walking = [];
    for (const [guid, body] of this.bodies) {
      if (!body.adopted) {
        walking.push({ guid, npc: body.npc });
      }
    }
    const gone = new Set(this.night.tick(walking));
    for (const { guid, npc } of walking) {
      if (gone.has(npc)) {
        npc.remove();
        this.bodies.delete(guid);
        // In the morning they come back to their places, not to the exit.
        this.lastSeen.delete(guid);
      }
    }
  }

  populationCount() {
    let count = 0;
    for (const body of this.bodies.values()) {
      if (!body.adopted) {
        count += 1;
      }
    }
    return count;
  }

  wantedNear(players) {
    const { activationRadius } = this.settings;
    const reach = Math.ceil(activationRadius / CELL_METRES);
    const best = new Map();
    for (const player of players) {
      const { x, y } = player.position;
      const cellX = Math.floor(x / CELL_METRES);
      const cellY = Math.floor(y / CELL_METRES);
      for (let stepX = -reach; stepX <= reach; stepX += 1) {
        for (let stepY = -reach; stepY <= reach; stepY += 1) {
          const rows = this.grid.get(`${cellX + stepX}:${cellY + stepY}`) ?? [];
          for (const row of rows) {
            const at = this.lastSeen.get(row.guid) ?? row.position;
            const gap = Math.hypot(at.x - x, at.y - y);
            if (gap > activationRadius || this.bodies.has(row.guid)) {
              continue;
            }
            const known = best.get(row.guid);
            if (!known || gap < known.gap) {
              best.set(row.guid, { row, gap });
            }
          }
        }
      }
    }
    return [...best.values()].sort((first, second) => first.gap - second.gap);
  }

  removeFar(players) {
    const { despawnRadius, enabled } = this.settings;
    for (const [guid, body] of [...this.bodies]) {
      if (body.adopted) {
        continue;
      }
      const alive = this.host.npcs.npcs.get(body.npc.id) === body.npc;
      const at = alive ? body.npc.position : null;
      const near =
        at &&
        players.some((player) => {
          const gap = Math.hypot(player.position.x - at.x, player.position.y - at.y);
          return gap <= despawnRadius;
        });
      if (alive && (!near || !enabled)) {
        if (at) {
          this.lastSeen.set(guid, { x: at.x, y: at.y });
        }
        body.npc.remove();
      }
      if (!alive || !near || !enabled) {
        this.bodies.delete(guid);
      }
    }
  }

  spawn(row, options = {}, adopted = false) {
    const { Npc } = this.api();
    // The level's people do not talk back: no Talk prompt and no npcInteract, unless a script that
    // adopts one asks for it. Their body is the level's class of them (a woman's soul on the default
    // man's body showed her name on a man).
    const npc = Npc.create({
      name: row.name,
      interactable: row.interactable === true,
      class: row.actorClass === 'NPC_Female' ? 'NPC_Female' : 'NPC',
      ...(row.outfit ? { outfit: row.outfit } : {}),
      ...options,
      soul: row.soul,
      position: options.position ?? row.position,
      rotation: options.rotation ?? row.rotation,
    });
    this.bodies.set(row.guid, { npc, adopted });
    if (this.levelPeople) {
      this.host.npcs.setLevelGuid(npc, row.guid);
    } else if (!adopted) {
      this.walkRound(npc, row);
    }
    return npc;
  }

  // The round of one person: up to two neighbours' places, then their own.
  walkRound(npc, row, { withoutWork = false } = {}) {
    if (row.work && !withoutWork) {
      // Their work and back, pausing at each.
      const seed = seedOf(row.guid);
      npc.patrol([aroundPlace(row.work, seed), row.position], {
        speed: 'walk',
        mode: 'loop',
        waitSeconds: SHORTEST_WAIT_SECONDS + ((seed >>> 16) % LONGEST_EXTRA_WAIT_SECONDS),
      });
      return;
    }
    const neighbours = this.neighboursOf(row);
    if (neighbours.length === 0) {
      return;
    }
    const seed = seedOf(row.guid);
    const first = neighbours[seed % neighbours.length];
    const second = neighbours[(seed >>> 8) % neighbours.length];
    const places = first === second ? [first] : [first, second];
    // Each walker stops at its own spot around a neighbour's place: everyone near shares the same
    // few places, and five stood within a metre of one.
    const route = [
      ...places.map((other, index) => aroundPlace(other.position, seed >>> (index * 11))),
      row.position,
    ];
    npc.patrol(route, {
      speed: 'walk',
      mode: 'loop',
      waitSeconds: SHORTEST_WAIT_SECONDS + ((seed >>> 16) % LONGEST_EXTRA_WAIT_SECONDS),
    });
  }

  neighboursOf(row) {
    const { x, y } = row.position;
    const cellX = Math.floor(x / CELL_METRES);
    const cellY = Math.floor(y / CELL_METRES);
    const found = [];
    for (let stepX = -1; stepX <= 1; stepX += 1) {
      for (let stepY = -1; stepY <= 1; stepY += 1) {
        for (const other of this.grid.get(`${cellX + stepX}:${cellY + stepY}`) ?? []) {
          const gap = Math.hypot(other.position.x - x, other.position.y - y);
          const sameFloor = !row.added || Math.abs(other.position.z - row.position.z) <= FLOOR_METRES;
          if (gap >= NEIGHBOUR_NEAREST && gap <= NEIGHBOUR_FARTHEST && sameFloor) {
            found.push(other);
          }
        }
      }
    }
    return found;
  }

  // Npc.adopt: one placement for a script; the same one again gives the same NPC.
  adopt(placedGuid, options = {}) {
    if (typeof placedGuid !== 'string') {
      throw new TypeError('Npc.adopt takes a level GUID, as Npc.placements lists it');
    }
    if (options === null || typeof options !== 'object') {
      throw new TypeError('Npc.adopt options must be an object');
    }
    this.index();
    const guid = placedGuid.toLowerCase().replace(/^0x/, '').replace(/-/g, '');
    const row = this.byGuid.get(guid);
    if (!row) {
      return null;
    }
    const body = this.bodies.get(guid);
    if (body && this.host.npcs.npcs.get(body.npc.id) === body.npc) {
      body.adopted = true;
      return body.npc;
    }
    return this.spawn(row, options, true);
  }
}

module.exports = { People, placementsOf };
