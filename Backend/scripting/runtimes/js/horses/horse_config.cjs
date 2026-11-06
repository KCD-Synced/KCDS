'use strict';

// The server's horse config: horse-config.json in the server's resources folder,
// beside item-config.json.
//
//   { "maxCarryWeight": 150,
//     "breeds": { "warhorse": { "look": { "head": "..." }, "maxCarryWeight": 200, "knockOut": true } } }
//
// maxCarryWeight: the most a spawned horse's saddlebags carry, kg (0 to 100000); left
// out, the game's own (the horse's soul's inventory capacity). Horse.maxCarryWeight changes one
// horse.
// breeds: what Horse.spawn({ breed }) gives a horse: its look (any of body, head, hair,
// tail from HorseLooks; a part left out is chosen at random as for any horse), maxCarryWeight,
// knockOut, stamina (the game's MaxStamina), speed (as the inspect screen shows it, 30 to 60, the
// game's horses about 35) and health (hit points, 100 a horse of the game's); the call's own
// options go over the breed's. A wrong entry is named in the log and left out.

const fs = require('node:fs');
const path = require('node:path');

const { lookOf, randomLook } = require('./looks.cjs');

const CONFIG_FILE = 'horse-config.json';
const MAX_CARRY = 100000;
// The ranges of a horse's stamina, speed and hit points.
const MAX_STAMINA = 10000;
const MIN_SPEED = 30;
const MAX_SPEED = 60;
const MAX_HIT_POINTS = 10000;

// A stamina, speed or hit points a horse may take: a number above `minimum` (speed: from it) up
// to its maximum, or null for the game's own (hit points: 100).
function checkHorseStat(value, maximum, what, minimum = 0) {
  if (value === null) {
    return null;
  }
  const low = minimum > 0 ? value < minimum : value <= 0;
  if (typeof value !== 'number' || !Number.isFinite(value) || low || value > maximum) {
    throw new TypeError(
      `${what} is ${
        minimum > 0 ? `${minimum}` : 'above 0'
      } to ${maximum}, or null for the game's own`,
    );
  }
  return value;
}

// A carry weight a horse may take: a number 0 to MAX_CARRY, or null for the game's own.
function checkCarryWeight(value, what) {
  if (value === null) {
    return null;
  }
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0 || value > MAX_CARRY) {
    throw new TypeError(`${what} is kg, 0 to ${MAX_CARRY}, or null for the game's own`);
  }
  return value;
}

// A breed as the config or a resource describes it, checked: { look, maxCarryWeight, knockOut }.
function checkBreed(name, breed) {
  if (breed === null || typeof breed !== 'object' || Array.isArray(breed)) {
    throw new TypeError(`breed ${name} is an object { look, maxCarryWeight, knockOut, ... }`);
  }
  for (const key of Object.keys(breed)) {
    if (!['look', 'maxCarryWeight', 'knockOut', 'stamina', 'speed', 'health'].includes(key)) {
      throw new TypeError(`breed ${name}: unknown field ${key}`);
    }
  }
  const look = breed.look ?? {};
  if (lookOf(randomLook(), look) === null) {
    throw new TypeError(`breed ${name}: a look part is not in HorseLooks`);
  }
  if (breed.knockOut !== undefined && typeof breed.knockOut !== 'boolean') {
    throw new TypeError(`breed ${name}: knockOut is true or false`);
  }
  return Object.freeze({
    look: Object.freeze({ ...look }),
    maxCarryWeight:
      breed.maxCarryWeight === undefined
        ? undefined
        : checkCarryWeight(breed.maxCarryWeight, `breed ${name}: maxCarryWeight`),
    knockOut: breed.knockOut,
    stamina:
      breed.stamina === undefined
        ? undefined
        : checkHorseStat(breed.stamina, MAX_STAMINA, `breed ${name}: stamina`),
    speed:
      breed.speed === undefined
        ? undefined
        : checkHorseStat(breed.speed, MAX_SPEED, `breed ${name}: speed`, MIN_SPEED),
    health:
      breed.health === undefined
        ? undefined
        : checkHorseStat(breed.health, MAX_HIT_POINTS, `breed ${name}: health`),
  });
}

class HorseConfig {
  constructor(log) {
    this.log = log ?? (() => {});
    this.maxCarryWeight = null;
    // Breed name -> its checked description.
    this.breeds = new Map();
  }

  load(root) {
    const file = path.join(root, CONFIG_FILE);
    if (!fs.existsSync(file)) {
      return;
    }
    let config;
    try {
      config = JSON.parse(fs.readFileSync(file, 'utf8'));
    } catch (error) {
      this.log(`${CONFIG_FILE}: ${error.message}`);
      return;
    }
    if (!config || typeof config !== 'object' || Array.isArray(config)) {
      this.log(`${CONFIG_FILE}: an object { maxCarryWeight }`);
      return;
    }
    if (config.maxCarryWeight !== undefined) {
      try {
        this.maxCarryWeight = checkCarryWeight(config.maxCarryWeight, 'maxCarryWeight');
      } catch (error) {
        this.log(`${CONFIG_FILE}: ${error.message}`);
      }
    }
    const breeds = config.breeds ?? {};
    if (breeds === null || typeof breeds !== 'object' || Array.isArray(breeds)) {
      this.log(`${CONFIG_FILE}: breeds is an object of name -> breed`);
      return;
    }
    for (const [name, breed] of Object.entries(breeds)) {
      try {
        this.breeds.set(name, checkBreed(name, breed));
      } catch (error) {
        this.log(`${CONFIG_FILE}: ${error.message}`);
      }
    }
    if (this.breeds.size > 0) {
      this.log(`${CONFIG_FILE}: ${this.breeds.size} horse breeds`);
    }
  }

  // The breed of that name, or a TypeError.
  breed(name) {
    const breed = typeof name === 'string' ? this.breeds.get(name) : undefined;
    if (!breed) {
      throw new TypeError(`Unknown horse breed: ${String(name)}`);
    }
    return breed;
  }
}

module.exports = {
  HorseConfig,
  checkCarryWeight,
  checkBreed,
  checkHorseStat,
  CONFIG_FILE,
  MAX_STAMINA,
  MIN_SPEED,
  MAX_SPEED,
  MAX_HIT_POINTS,
};
