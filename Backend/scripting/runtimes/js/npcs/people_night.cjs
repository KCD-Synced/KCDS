'use strict';

// Where the level's people spend the night (server.json npcHouses):
//
//   npcHouses true (the default): they own houses. The game's own people (levelPeople) go home at
//   night by their own schedules, as in the game; nothing here acts.
//   npcHouses false: they own none. At night they walk out of the map: each person walks to the
//   nearest of the level's exits (the levelExit_<n> points where its roads leave the map,
//   npcs/level_exits.json by Dev/tools/npcs/build_level_exits.py) and goes once there. Until the
//   morning nobody of the level's people comes; then they are at their places again.
//
// The night runs from leaveHour to returnHour of the world's clock (World.hour): 21 to 6 by
// default, or as people-night.json in the resources folder sets them: { "leaveHour": 21,
// "returnHour": 6 }.

const fs = require('node:fs');
const path = require('node:path');

const FILE = 'people-night.json';
const EXITS = require('./level_exits.json');
const DEFAULTS = Object.freeze({ leaveHour: 21, returnHour: 6 });
// How near an exit a person goes, and how often their walk there is asked again (a body that
// changed games, or an order the game dropped).
const ARRIVED_METRES = 6;
const WALK_AGAIN_MS = 15000;

function hourIn(value, fallback, name) {
  if (value === undefined) {
    return fallback;
  }
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0 || value >= 24) {
    throw new RangeError(`${FILE}: ${name} is an hour from 0 up to 24`);
  }
  return value;
}

function settingsOf(given) {
  if (given === null || typeof given !== 'object' || Array.isArray(given)) {
    throw new TypeError(`${FILE} holds an object`);
  }
  return {
    leaveHour: hourIn(given.leaveHour, DEFAULTS.leaveHour, 'leaveHour'),
    returnHour: hourIn(given.returnHour, DEFAULTS.returnHour, 'returnHour'),
  };
}

// Whether the hour is in the night from leave to return (which may run over midnight).
function isNight(hour, { leaveHour, returnHour }) {
  if (leaveHour === returnHour) {
    return false;
  }
  if (leaveHour < returnHour) {
    return hour >= leaveHour && hour < returnHour;
  }
  return hour >= leaveHour || hour < returnHour;
}

function nearest(exits, at) {
  let best = null;
  let bestGap = Infinity;
  for (const exit of exits) {
    const gap = Math.hypot(exit.position.x - at.x, exit.position.y - at.y);
    if (gap < bestGap) {
      best = exit;
      bestGap = gap;
    }
  }
  return best === null ? null : { exit: best, gap: bestGap };
}

class PeopleNight {
  // houses: server.json npcHouses; hour(): the world's hour; level(): the level's name.
  constructor({ houses, hour, level, now = () => Date.now(), log = () => {} }) {
    this.houses = houses;
    this.hour = hour;
    this.level = level;
    this.now = now;
    this.log = log;
    this.settings = { ...DEFAULTS };
    // npc id -> when their walk to the exit was last asked (ms).
    this.walking = new Map();
  }

  loadFile(root) {
    const file = path.join(root, FILE);
    if (!fs.existsSync(file)) {
      return;
    }
    try {
      this.settings = settingsOf(JSON.parse(fs.readFileSync(file, 'utf8')));
    } catch (error) {
      this.log(`${FILE}: ${error.message}`);
    }
  }

  set(given) {
    this.settings = settingsOf(given);
  }

  exits() {
    return EXITS[this.level()] ?? [];
  }

  // Whether it is night now (leaveHour to returnHour).
  isNightNow() {
    return isNight(this.hour(), this.settings);
  }

  // Whether the people are away for the night: nobody of them is spawned now.
  away() {
    return !this.houses && this.exits().length > 0 && isNight(this.hour(), this.settings);
  }

  // Each tick of the people, at night without houses: everyone walks to their nearest exit and
  // goes there. bodies: [{ npc }] of the level's people a script did not adopt. Returns the npcs
  // that reached an exit, for the people to let go.
  tick(bodies) {
    if (!this.away()) {
      this.walking.clear();
      return [];
    }
    const exits = this.exits();
    const now = this.now();
    const gone = [];
    for (const { npc } of bodies) {
      const at = npc.position;
      if (!at) {
        continue;
      }
      const target = nearest(exits, at);
      if (target.gap <= ARRIVED_METRES) {
        gone.push(npc);
        this.walking.delete(npc.id);
        continue;
      }
      const asked = this.walking.get(npc.id);
      if (asked !== undefined && now - asked < WALK_AGAIN_MS) {
        continue;
      }
      try {
        npc.moveTo(target.exit.position, { speed: 'walk', radius: ARRIVED_METRES / 2 });
        this.walking.set(npc.id, now);
      } catch (error) {
        this.log(`people night: ${npc.id} cannot walk out: ${error.message}`);
      }
    }
    return gone;
  }
}

module.exports = { PeopleNight, isNight, settingsOf, NIGHT_FILE: FILE };
