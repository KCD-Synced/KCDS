'use strict';

// The checks a particle effect's name, point and tuning go through, shared by the
// server's Vfx (vfx.cjs) and the client's (Client/resources/vfx.cjs).

const { vector } = require('../math.cjs');
const catalog = require('./catalog.json');

const GLOBAL_WORLD = 0xffffffff;
// A point farther out than this lies outside any level.
const WORLD_LIMIT = 1e6;
// Library.group.name, as the game's particle libraries spell it (more dots may follow).
const EFFECT_NAME = /^[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+){2,}$/;
// An effect draws at most this far, so a burst reaches nobody beyond it.
const BURST_RANGE = 250;
const BURST_MS_DEFAULT = 2000;
const BURST_MS_MAX = 60000;
const OFFSET_MAX = 8;
// A body joint's name a burst hangs on, at most this long.
const JOINT_MAX = 64;
// name -> [min, max, default] of the tuning values every effect carries.
const TUNING = {
  scale: [0.01, 32, 1],
  countScale: [0.01, 16, 1],
  speedScale: [0, 16, 1],
  timeScale: [0.01, 16, 1],
  strength: [-1, 1, -1],
  pulsePeriod: [0, 600, 0],
};

function validWorld(world) {
  return Number.isSafeInteger(world) && world >= 0 && world <= GLOBAL_WORLD;
}

// The game's particle libraries the catalog lists completely, and their effects.
const KNOWN_EFFECTS = new Set(catalog.effects);
const KNOWN_LIBRARIES = new Set(catalog.effects.map((name) => name.split('.')[0]));

// Whether a name of a catalogued library is one of its effects, or a part of one (more dots).
function knownEffect(effect) {
  const parts = effect.split('.');
  for (let length = parts.length; length >= 3; length -= 1) {
    if (KNOWN_EFFECTS.has(parts.slice(0, length).join('.'))) {
      return true;
    }
  }
  return false;
}

function effectOf(effect) {
  if (typeof effect !== 'string' || !EFFECT_NAME.test(effect)) {
    throw new TypeError('Vfx effect must be a name like Library.group.effect');
  }
  // A library the game has but no such effect in it: the game would show nothing.
  // Libraries the catalog does not list (another pak's) are left to the game.
  if (KNOWN_LIBRARIES.has(effect.split('.')[0]) && !knownEffect(effect)) {
    throw new RangeError(`Unknown Vfx effect ${effect} (Vfx.list() names the game's effects)`);
  }
  return effect;
}

function worldOf(dimension) {
  const world = dimension ?? GLOBAL_WORLD;
  if (!validWorld(world)) {
    throw new TypeError('Invalid dimension');
  }
  return world;
}

// One tuning value clamped into its range; `undefined` takes the default.
function tuned(name, value) {
  const [min, max, fallback] = TUNING[name];
  if (value === undefined) return fallback;
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError(`Vfx ${name} must be a finite number`);
  }
  return Math.min(max, Math.max(min, value));
}

// All tuning values of an options object, as a plain record.
function tuning(options) {
  const out = {};
  for (const name of Object.keys(TUNING)) out[name] = tuned(name, options[name]);
  return out;
}

function pointOf(position, what) {
  const at = vector(position);
  if (![at.x, at.y, at.z].every((n) => Number.isFinite(n) && Math.abs(n) <= WORLD_LIMIT)) {
    throw new RangeError(`Invalid ${what}`);
  }
  return { x: at.x, y: at.y, z: at.z };
}

// The joint a burst on a body hangs on: '' (the body itself) when none is given.
function jointOf(joint) {
  if (joint === undefined) {
    return '';
  }
  if (typeof joint !== 'string') {
    throw new TypeError('Vfx joint must be a string');
  }
  if (joint.length === 0 || joint.length > JOINT_MAX || !/^[\x20-\x7e]+$/.test(joint)) {
    throw new RangeError(`Vfx joint must be 1 to ${JOINT_MAX} printable characters`);
  }
  return joint;
}

module.exports = {
  JOINT_MAX,
  jointOf,
  GLOBAL_WORLD,
  WORLD_LIMIT,
  EFFECT_NAME,
  BURST_RANGE,
  BURST_MS_DEFAULT,
  BURST_MS_MAX,
  OFFSET_MAX,
  TUNING,
  validWorld,
  effectOf,
  worldOf,
  tuned,
  tuning,
  pointOf,
};
