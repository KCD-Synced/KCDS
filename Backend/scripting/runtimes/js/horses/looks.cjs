'use strict';

// How a horse looks: the character components its body is built from, as a
// person's (Dev/tools/horses/build_looks.py): `body` the coat, `head` the breed, `hair` the mane
// and `tail`. The server chooses them, so every client builds the horse the same; a horse a
// resource did not describe gets a random common look. In the horse's `look` field they travel as
// "<body>|<head>|<hair>|<tail>".

const looks = require('./looks.json');

const PARTS = Object.freeze(['body', 'head', 'hair', 'tail']);

const known = Object.fromEntries(PARTS.map((part) => [part, new Set(looks[part])]));

function pick(list, random) {
  return list[Math.floor(random() * list.length)];
}

// A random common look.
function randomLook(random = Math.random) {
  const look = {};
  for (const part of PARTS) {
    look[part] = pick(looks.common[part], random);
  }
  return look;
}

// What a horse wearing `current` looks like after setLook(request): a part left out stays; null
// when a name is not in the catalog. Throws for a request that is not an object of part names.
function lookOf(current, request) {
  if (request === null || typeof request !== 'object' || Array.isArray(request)) {
    throw new TypeError('setLook takes an object of horse look parts');
  }
  for (const key of Object.keys(request)) {
    if (!PARTS.includes(key)) {
      throw new TypeError(`unknown horse look part ${key}`);
    }
    if (request[key] !== undefined && typeof request[key] !== 'string') {
      throw new TypeError(`horse look ${key} must be a string`);
    }
  }
  const next = { ...current };
  for (const part of PARTS) {
    if (request[part] !== undefined) {
      next[part] = request[part];
    }
  }
  for (const part of PARTS) {
    if (!known[part].has(next[part])) {
      return null;
    }
  }
  return next;
}

// The look a client builds the horse's body from, '' for none: a horse with a soul of the game's
// tables keeps that soul's own coat, mane and tail.
function lookField(look) {
  if (!look) {
    return '';
  }
  return PARTS.map((part) => look[part]).join('|');
}

// The game's generic horse soul (Horse.lua's), what a horse without a soul of its own is.
const GENERIC_HORSE_SOUL = 'fc0e6251-b314-4398-b83f-3a66a52961ee';

// Whether a Horse.spawn soul is one of the game's named horses (a soul GUID of its tables other
// than the generic horse), spawned with its own look.
function isGameHorseSoul(soul) {
  return (
    typeof soul === 'string' &&
    /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(soul) &&
    soul.toLowerCase() !== GENERIC_HORSE_SOUL
  );
}

// The HorseLooks global: the names a resource can choose from.
const HorseLooks = Object.freeze({
  options(part) {
    if (!PARTS.includes(part)) {
      throw new TypeError(`horse look part must be one of ${PARTS.join(', ')}`);
    }
    return [...looks[part]];
  },
  common(part) {
    if (!PARTS.includes(part)) {
      throw new TypeError(`horse look part must be one of ${PARTS.join(', ')}`);
    }
    return [...looks.common[part]];
  },
});

module.exports = { HorseLooks, PARTS, randomLook, lookOf, lookField, isGameHorseSoul };
