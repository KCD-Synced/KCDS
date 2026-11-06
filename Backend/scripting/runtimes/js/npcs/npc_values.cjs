'use strict';

// The values a resource gives an NPC (Npc.create, Npc.teleport and the setters), checked before
// they reach the native server, which checks them again
// (Backend/integration/include/kcd2s/integration/npc.hpp).

const { Appearances, PARTS } = require('../appearance/appearances.cjs');
const { Quaternion, Vector3, rotation } = require('../math.cjs');

const GLOBAL_WORLD = 0xffffffff;
// The animal classes of the server's animal population (a KCDS addition): the kinds of
// the spawner and placed entries in animals/spawn_points.json. The flock kinds (Chickens, Rats ...)
// are not here: the population does not spawn flocks.
const ANIMAL_CLASSES = [
  'Boar',
  'CattleBull',
  'CattleCow',
  'Dog',
  'Hare',
  'Pig',
  'Raven',
  'RedDeerDoe',
  'RedDeerStag',
  'RoeDeerBuck',
  'RoeDeerHind',
  'SheepEwe',
  'SheepRam',
  'WildDog',
  'Wolf',
];
const ACTOR_CLASSES = ['NPC', 'NPC_Female', ...ANIMAL_CLASSES];
const SOUL_GUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
// The longest name in UTF-8 bytes (kNpcNameMax).
const NAME_MAX_BYTES = 128;
// A level lies in a square of this side, between 1 km below and 10 km above sea level (metres).
const WORLD_EXTENT = 16384;
const WORLD_BOTTOM = -1000;
const WORLD_TOP = 10000;

// A text's length in UTF-8 bytes (the client runtime's worker has no Buffer).
const encoder = new TextEncoder();
function utf8Length(text) {
  return encoder.encode(text).length;
}

function finiteNumber(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function validWorld(world) {
  return Number.isSafeInteger(world) && world >= 0 && world <= GLOBAL_WORLD;
}

// A soul GUID text in lower case, or '' for none.
function soulOf(value) {
  if (value === undefined || value === '') {
    return '';
  }
  if (typeof value !== 'string') {
    throw new TypeError('Npc soul must be a role name or a GUID string');
  }
  if (!SOUL_GUID.test(value)) {
    throw new RangeError(
      'Npc soul must be a role from Npc.roles() or a GUID (8-4-4-4-12 hex digits)',
    );
  }
  return value.toLowerCase();
}

function actorClassOf(value) {
  if (value === undefined) {
    return 'NPC';
  }
  if (typeof value !== 'string') {
    throw new TypeError('Npc class must be a string');
  }
  if (!ACTOR_CLASSES.includes(value)) {
    throw new RangeError(`Npc class must be one of ${ACTOR_CLASSES.join(', ')}`);
  }
  return value;
}

function nameOf(value) {
  if (value === undefined) {
    return '';
  }
  if (typeof value !== 'string') {
    throw new TypeError('Npc name must be a string');
  }
  if (utf8Length(value) > NAME_MAX_BYTES) {
    throw new RangeError(`Npc name is longer than ${NAME_MAX_BYTES} bytes`);
  }
  if (/[\x00-\x1f\x7f]/.test(value)) {
    throw new RangeError('Npc name must not hold a line break or control character');
  }
  return value;
}

function worldOf(value) {
  if (value === undefined) {
    return GLOBAL_WORLD;
  }
  if (!validWorld(value)) {
    throw new TypeError('Invalid dimension');
  }
  return value;
}

// What is wrong with a position: 'type' without finite x, y and z, 'range' outside the world, or
// null for a usable one.
function positionProblem(value) {
  if (value === null || typeof value !== 'object') {
    return 'type';
  }
  const { x, y, z } = value;
  if (!finiteNumber(x) || !finiteNumber(y) || !finiteNumber(z)) {
    return 'type';
  }
  const inside =
    x >= 0 &&
    x <= WORLD_EXTENT &&
    y >= 0 &&
    y <= WORLD_EXTENT &&
    z >= WORLD_BOTTOM &&
    z <= WORLD_TOP;
  return inside ? null : 'range';
}

// The position as a Vector3; a TypeError or RangeError for an unusable one.
function positionOf(value, what) {
  const problem = positionProblem(value);
  if (problem === 'type') {
    throw new TypeError(`${what} takes a position with finite x, y and z`);
  }
  if (problem === 'range') {
    throw new RangeError(`${what} position lies outside the world`);
  }
  return new Vector3(value.x, value.y, value.z);
}

// A unit Quaternion from Euler degrees or a Quaternion (a component left out is 0), or
// null for anything else.
function facingOrNull(value) {
  if (value === null || typeof value !== 'object') {
    return null;
  }
  const quaternion = 'w' in value;
  const parts = quaternion ? [value.x, value.y, value.z, value.w] : [value.x, value.y, value.z];
  const usable = parts.every((part) => finiteNumber(part) || (!quaternion && part === undefined));
  if (!usable) {
    return null;
  }
  const q = rotation(value);
  const length = Math.hypot(q.w, q.x, q.y, q.z);
  if (!Number.isFinite(length) || length < 1e-6) {
    return null;
  }
  return new Quaternion(q.w / length, q.x / length, q.y / length, q.z / length);
}

// The rotation as a unit Quaternion; a TypeError for an unusable one.
function facingOf(value, what) {
  const facing = facingOrNull(value);
  if (!facing) {
    throw new TypeError(`${what} takes a Quaternion or finite Euler angles in degrees`);
  }
  return facing;
}

function booleanOf(value, what) {
  if (typeof value !== 'boolean') {
    throw new TypeError(`${what} must be a boolean`);
  }
  return value;
}

// The longest clip name playAnimation takes (kNpcAnimationMax less its sequence and loop).
const NPC_CLIP_MAX = 256;
// The longest fragment name and tags of a fragment request, as a player's playAnimation takes.
const NPC_FRAGMENT_MAX = 64;
const NPC_TAGS_MAX = 128;
// say: the longest text in UTF-8 bytes (kNpcSpeechMax less its sequence and duration), and how
// long a bubble stays: by default 2 s and 60 ms a character up to 10 s; from 0.5 s to a minute
// when given.
const SPEECH_MAX_BYTES = 400;
const SPEECH_BASE_MS = 2000;
const SPEECH_SHORTEST_MS = 500;
const SPEECH_MS_PER_CHARACTER = 60;
const SPEECH_LONGEST_MS = 10000;
const SPEECH_LONGEST_ALLOWED_MS = 60000;

// True for a text of printable ASCII without '|' (the field's separator).
function fieldText(text) {
  return /^[\x20-\x7e]*$/.test(text) && !text.includes('|');
}

// playAnimation(fragment, { clip, tags, loop }): { clip, loop } for a clip of the game's own
// animations; { fragment, tags, loop } for a Mannequin fragment by its name, with its tags
// separated by '+'; null for a request with neither (an empty fragment, or a fragment
// id). Throws for a malformed request.
function animationRequestOf(fragment, options) {
  if (typeof fragment !== 'string' && typeof fragment !== 'number') {
    throw new TypeError('playAnimation takes a fragment name or id');
  }
  if (options === null || typeof options !== 'object') {
    throw new TypeError('playAnimation options must be an object');
  }
  const { clip, tags = '', loop = false } = options;
  if (typeof loop !== 'boolean') {
    throw new TypeError('playAnimation loop must be a boolean');
  }
  if (typeof clip === 'string' && clip.length > 0) {
    if (clip.length > NPC_CLIP_MAX || !fieldText(clip)) {
      throw new RangeError(
        `playAnimation takes a clip of 1 to ${NPC_CLIP_MAX} printable characters`,
      );
    }
    return { clip, loop };
  }
  if (typeof fragment !== 'string' || fragment.length === 0) {
    return null;
  }
  if (fragment.length > NPC_FRAGMENT_MAX || !fieldText(fragment)) {
    throw new RangeError(
      `playAnimation takes a fragment of 1 to ${NPC_FRAGMENT_MAX} printable characters`,
    );
  }
  if (typeof tags !== 'string') {
    throw new TypeError('playAnimation tags must be a string');
  }
  if (tags.length > NPC_TAGS_MAX || !fieldText(tags)) {
    throw new RangeError(
      `playAnimation takes tags of at most ${NPC_TAGS_MAX} printable characters`,
    );
  }
  return { fragment, tags, loop };
}

// say(text, { duration }): the text and how many milliseconds it shows. Throws for a text that is
// not one line of at most SPEECH_MAX_BYTES or a duration out of range.
function speechOf(text, options) {
  if (typeof text !== 'string') {
    throw new TypeError('say takes a text');
  }
  if (options === null || typeof options !== 'object') {
    throw new TypeError('say options must be an object');
  }
  if (utf8Length(text) > SPEECH_MAX_BYTES || /[\x00-\x1f\x7f]/.test(text)) {
    throw new RangeError(
      `say takes one line of at most ${SPEECH_MAX_BYTES} bytes without control characters`,
    );
  }
  const defaultDuration = Math.min(
    SPEECH_LONGEST_MS,
    SPEECH_BASE_MS + SPEECH_MS_PER_CHARACTER * text.length,
  );
  const { duration = defaultDuration } = options;
  const validDuration =
    Number.isSafeInteger(duration) &&
    duration >= SPEECH_SHORTEST_MS &&
    duration <= SPEECH_LONGEST_ALLOWED_MS;
  if (!validDuration) {
    throw new RangeError(
      `say duration must be whole milliseconds from ${SPEECH_SHORTEST_MS} to ${SPEECH_LONGEST_ALLOWED_MS}`,
    );
  }
  return { text, duration };
}

// The orders (moveTo, patrol, follow, flee, lookAt, attack): their options checked here, their
// texts built by the registry (npc.hpp NpcIntent: "<sequence>|<kind>|<arguments...>").
const SPEEDS = ['walk', 'jog', 'run'];
const PATHFINDING_MODES = ['auto', 'game', 'server'];
const PATROL_MODES = ['once', 'loop', 'pingPong'];
const WEAPONS = ['melee', 'bow'];
// Metres. moveTo and patrol arrive at 1.5 m; KCDS keeps within 3 m of a followed body and runs
// until 30 m away.
const ARRIVAL_RADIUS = 1.5;
const FOLLOW_RADIUS = 3;
const FLEE_RADIUS = 30;
const RADIUS_MAX = 1000;
// attack reaches a target within this many metres.
const ATTACK_RANGE = 128;
// A use press on an NPC and a harvest of its carcass count within this many metres of where the
// NPC was last reported.
const USE_RANGE = 6;
// A horse's patrol order must fit the HorseIntent text (2 KB) of one snapshot.
const PATROL_POINTS_MAX = 64;
// An NPC's patrol is stepped by the server, one leg a move order, so a route is not bound by it.
const PATROL_ROUTE_POINTS_MAX = 256;
const PATROL_WAIT_MAX = 3600;

function choiceOf(value, choices, fallback, what) {
  if (value === undefined) {
    return fallback;
  }
  if (typeof value !== 'string') {
    throw new TypeError(`${what} must be a string`);
  }
  if (!choices.includes(value)) {
    throw new RangeError(`${what} must be one of ${choices.join(', ')}`);
  }
  return value;
}

function radiusOf(value, fallback, what) {
  if (value === undefined) {
    return fallback;
  }
  if (typeof value !== 'number') {
    throw new TypeError(`${what} radius must be a number`);
  }
  if (!(value > 0 && value <= RADIUS_MAX)) {
    throw new RangeError(`${what} radius must be above 0 and at most ${RADIUS_MAX} metres`);
  }
  return value;
}

// The options object of an order; {} when left out. Throws for anything but an object.
function orderOptionsOf(options, what) {
  if (options === undefined) {
    return {};
  }
  if (options === null || typeof options !== 'object') {
    throw new TypeError(`${what} options must be an object`);
  }
  return options;
}

// moveTo's options: { speed, radius, pathfinding }, the defaults filled in.
function moveOptionsOf(options, what) {
  const given = orderOptionsOf(options, what);
  return {
    speed: choiceOf(given.speed, SPEEDS, 'walk', `${what} speed`),
    radius: radiusOf(given.radius, ARRIVAL_RADIUS, what),
    pathfinding: choiceOf(given.pathfinding, PATHFINDING_MODES, 'auto', `${what} pathfinding`),
  };
}

// A wait at a waypoint in seconds, from 0 to PATROL_WAIT_MAX; `fallback` when left out. Throws
// for a number out of range; null for a value that is not a number, which is answered with false
// rather than a throw.
function waitOf(value, fallback, what) {
  if (value === undefined) {
    return fallback;
  }
  if (!finiteNumber(value)) {
    return null;
  }
  if (!(value >= 0 && value <= PATROL_WAIT_MAX)) {
    throw new RangeError(`${what} waitSeconds must be from 0 to ${PATROL_WAIT_MAX}`);
  }
  return value;
}

// patrol's options as the call gave them: { speed, mode, radius, waitSeconds, pathfinding }, each
// undefined when left out, so that a named route's own defaults can show through. `waitSeconds` is
// null for a value that is not a number. `loop` and `mode` are not combined.
function patrolOverridesOf(options) {
  const given = orderOptionsOf(options, 'patrol');
  if (given.loop !== undefined && given.mode !== undefined) {
    throw new TypeError('patrol takes mode or loop, not both');
  }
  let mode = choiceOf(given.mode, PATROL_MODES, undefined, 'patrol mode');
  if (given.loop !== undefined) {
    mode = booleanOf(given.loop, 'patrol loop') ? 'loop' : 'once';
  }
  const radius = given.radius === undefined ? undefined : radiusOf(given.radius, 0, 'patrol');
  return {
    speed: choiceOf(given.speed, SPEEDS, undefined, 'patrol speed'),
    mode,
    radius,
    waitSeconds: waitOf(given.waitSeconds, undefined, 'patrol'),
    pathfinding: choiceOf(given.pathfinding, PATHFINDING_MODES, undefined, 'patrol pathfinding'),
  };
}

// patrol's options: { speed, mode, radius, waitSeconds, pathfinding }, the defaults filled in
// (walk, loop, zero wait). `waitSeconds` is null for a value that is not a number.
function patrolOptionsOf(options) {
  const given = patrolOverridesOf(options);
  return {
    speed: given.speed ?? 'walk',
    mode: given.mode ?? 'loop',
    radius: given.radius ?? ARRIVAL_RADIUS,
    waitSeconds: given.waitSeconds === undefined ? 0 : given.waitSeconds,
    pathfinding: given.pathfinding ?? 'auto',
  };
}

// follow's options: { speed, radius }; flee's differ in their defaults (run, 30 m).
function followOptionsOf(options) {
  const given = orderOptionsOf(options, 'follow');
  return {
    speed: choiceOf(given.speed, SPEEDS, 'walk', 'follow speed'),
    radius: radiusOf(given.radius, FOLLOW_RADIUS, 'follow'),
  };
}

function fleeOptionsOf(options) {
  const given = orderOptionsOf(options, 'flee');
  return {
    speed: choiceOf(given.speed, SPEEDS, 'run', 'flee speed'),
    radius: radiusOf(given.radius, FLEE_RADIUS, 'flee'),
  };
}

// attack's options: { weapon }, melee by default.
function attackOptionsOf(options) {
  const given = orderOptionsOf(options, 'attack');
  return { weapon: choiceOf(given.weapon, WEAPONS, 'melee', 'attack weapon') };
}

// A place an order names as { x, y, z }, a component left out keeping the one in `current`; null
// when it is not usable (no object, not finite, outside the world).
function placeOf(value, current) {
  if (value === null || typeof value !== 'object') {
    return null;
  }
  const place = { x: value.x ?? current.x, y: value.y ?? current.y, z: value.z ?? current.z };
  return positionProblem(place) === null ? place : null;
}

// A number as plain decimal text at centimetre precision, never in exponent form.
function decimal(value) {
  return String(Number(value.toFixed(2)));
}

// An NPC's character components before any choice: every part the soul's own.
function soulAppearance() {
  return { body: '', head: '', hair: '', beard: '' };
}

// What an NPC of `gender` wearing `current` wears after setAppearance(request); null when a name
// is not in the gender's catalog or the face cannot wear the beard. Throws for a request that is
// not an object of part names; an NPC's gender is its class and cannot be asked for.
function npcAppearanceOf(current, request, gender) {
  if (request === null || typeof request !== 'object' || Array.isArray(request)) {
    throw new TypeError('setAppearance takes an object of appearance parts');
  }
  for (const key of Object.keys(request)) {
    if (key === 'gender') {
      throw new TypeError("an NPC's gender is its class (NPC or NPC_Female)");
    }
    if (!PARTS.includes(key)) {
      throw new TypeError(`unknown appearance part ${key}`);
    }
    if (request[key] !== undefined && typeof request[key] !== 'string') {
      throw new TypeError(`appearance ${key} must be a string`);
    }
  }
  const next = { ...current };
  for (const part of PARTS) {
    if (request[part] !== undefined) {
      next[part] = request[part];
    }
  }
  for (const part of PARTS) {
    if (next[part] !== '' && Appearances.find(part, next[part], gender) === null) {
      return null;
    }
  }
  // A named face says which beards fit; the soul's own face is the game's to fit.
  const fits = Appearances.beards(next.head, gender).some((option) => option.name === next.beard);
  if (next.head !== '' && next.beard !== '' && !fits) {
    return null;
  }
  return next;
}

module.exports = {
  GLOBAL_WORLD,
  ANIMAL_CLASSES,
  NAME_MAX_BYTES,
  validWorld,
  soulOf,
  actorClassOf,
  nameOf,
  worldOf,
  positionProblem,
  positionOf,
  facingOrNull,
  facingOf,
  booleanOf,
  soulAppearance,
  npcAppearanceOf,
  animationRequestOf,
  speechOf,
  ATTACK_RANGE,
  USE_RANGE,
  PATROL_POINTS_MAX,
  PATROL_ROUTE_POINTS_MAX,
  PATROL_MODES,
  SPEEDS,
  ARRIVAL_RADIUS,
  utf8Length,
  choiceOf,
  radiusOf,
  waitOf,
  moveOptionsOf,
  patrolOverridesOf,
  patrolOptionsOf,
  followOptionsOf,
  fleeOptionsOf,
  attackOptionsOf,
  placeOf,
  decimal,
};
