'use strict';

// Lines the level's people say to players near them: now and then the server picks
// one of the people near a player and has them say a line in the game's speech bubble, the same
// for every player there (Npc.say). The game's own barks and reactions come from each person's
// brain on the game that runs them (server.json levelPeople, issue 016); these lines are the
// server owner's, from people-lines.json in the resources folder:
//
//   {
//     "enabled": true,          // false: no lines at all
//     "everySeconds": 60,       // about how often one person near a player speaks to them
//     "radius": 8,              // how near (metres) the person must be
//     "seconds": 5,             // how long the bubble shows
//     "lines": ["Good day, {player}.", "Fine weather."],
//     "byName": { "Guard": ["Move along."] }   // the lines of the people of a name, instead
//   }
//
// {player} in a line is the player's nickname. A person speaks at most once in `everySeconds`,
// and a player hears at most one line in that time. Without the file, or with no lines, nobody
// speaks.

const fs = require('node:fs');
const path = require('node:path');

const FILE = 'people-lines.json';
const MAX_LINES = 512;
const MAX_LINE_BYTES = 240;
const DEFAULTS = Object.freeze({
  enabled: true,
  everySeconds: 60,
  radius: 8,
  seconds: 5,
  lines: [],
  byName: {},
});

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function numberIn(value, fallback, low, high, name) {
  if (value === undefined) {
    return fallback;
  }
  if (typeof value !== 'number' || !Number.isFinite(value) || value < low || value > high) {
    throw new RangeError(`${FILE}: ${name} is a number from ${low} to ${high}`);
  }
  return value;
}

function linesOf(value, name) {
  if (value === undefined) {
    return [];
  }
  if (!Array.isArray(value) || value.length > MAX_LINES) {
    throw new TypeError(`${FILE}: ${name} is a list of at most ${MAX_LINES} lines`);
  }
  for (const line of value) {
    if (typeof line !== 'string' || line === '' || Buffer.byteLength(line) > MAX_LINE_BYTES) {
      throw new TypeError(`${FILE}: every line of ${name} is text of 1 to ${MAX_LINE_BYTES} bytes`);
    }
  }
  return [...value];
}

// The settings from the file's JSON; throws for one it cannot use.
function settingsOf(given) {
  if (!isObject(given)) {
    throw new TypeError(`${FILE} holds an object`);
  }
  const enabled = given.enabled ?? true;
  if (typeof enabled !== 'boolean') {
    throw new TypeError(`${FILE}: enabled is true or false`);
  }
  const byName = {};
  if (given.byName !== undefined) {
    if (!isObject(given.byName)) {
      throw new TypeError(`${FILE}: byName is an object of names and their lines`);
    }
    for (const [name, lines] of Object.entries(given.byName)) {
      byName[name] = linesOf(lines, `byName.${name}`);
    }
  }
  return {
    enabled,
    everySeconds: numberIn(given.everySeconds, DEFAULTS.everySeconds, 5, 3600, 'everySeconds'),
    radius: numberIn(given.radius, DEFAULTS.radius, 1, 50, 'radius'),
    seconds: numberIn(given.seconds, DEFAULTS.seconds, 1, 30, 'seconds'),
    lines: linesOf(given.lines, 'lines'),
    byName,
  };
}

class PeopleLines {
  // bodies(): the people's NPC handles with their names, [{ npc, name }].
  constructor({ bodies, now = () => Date.now(), random = Math.random, log = () => {} }) {
    this.bodies = bodies;
    this.now = now;
    this.random = random;
    this.log = log;
    this.settings = { ...DEFAULTS };
    // player id -> when they may hear the next line; npc id -> when they may speak again (ms).
    this.playerNext = new Map();
    this.npcNext = new Map();
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

  linesFor(name) {
    const own = this.settings.byName[name];
    return own && own.length > 0 ? own : this.settings.lines;
  }

  // A pause of about everySeconds: from half of it to one and a half times it.
  pause() {
    return this.settings.everySeconds * 1000 * (0.5 + this.random());
  }

  // Each tick of the people: a player whose time came hears a line from the nearest person near
  // them who may speak.
  tick(players) {
    const { enabled, radius, seconds } = this.settings;
    if (!enabled) {
      return;
    }
    const now = this.now();
    const people = this.bodies().filter(({ name }) => this.linesFor(name).length > 0);
    if (people.length === 0) {
      return;
    }
    for (const player of players) {
      const due = this.playerNext.get(player.id);
      if (due === undefined) {
        this.playerNext.set(player.id, now + this.pause());
        continue;
      }
      if (now < due) {
        continue;
      }
      let best = null;
      let bestGap = Infinity;
      for (const person of people) {
        const at = person.npc.position;
        if (!at || now < (this.npcNext.get(person.npc.id) ?? 0)) {
          continue;
        }
        const gap = Math.hypot(at.x - player.position.x, at.y - player.position.y);
        if (gap <= radius && gap < bestGap) {
          best = person;
          bestGap = gap;
        }
      }
      if (best === null) {
        continue;
      }
      const lines = this.linesFor(best.name);
      const line = lines[Math.floor(this.random() * lines.length) % lines.length];
      const text = line.split('{player}').join(player.nickname ?? '');
      let said = false;
      try {
        said = best.npc.say(text, { duration: Math.round(seconds * 1000) });
      } catch (error) {
        this.log(`${FILE}: "${line}" cannot be said: ${error.message}`);
      }
      if (said) {
        this.playerNext.set(player.id, now + this.pause());
        this.npcNext.set(best.npc.id, now + this.settings.everySeconds * 1000);
      }
    }
  }

  // A player left: their timer goes.
  forget(playerId) {
    this.playerNext.delete(playerId);
  }
}

module.exports = { PeopleLines, settingsOf, LINES_FILE: FILE };
