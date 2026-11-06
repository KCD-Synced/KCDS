'use strict';
// The game's character-component catalog: every body, face, hairstyle and beard a
// player's body can be given, built from a game installation by
// Dev/tools/appearance/build_catalog.py. Rows are [name, group] and, for a head,
// [name, group, indexes of the beards modelled for it].
const catalog = require('./catalog.json');

const PARTS = ['body', 'head', 'hair', 'beard'];
const GENDERS = ['male', 'female'];
// The face of a body nobody chose one for: the player's own.
const DEFAULT_HEAD = 'henry';

function checkPart(part) {
  if (!PARTS.includes(part)) {
    throw new TypeError(`appearance part must be one of ${PARTS.join(', ')}`);
  }
  return part;
}

function checkGender(gender = 'male') {
  if (!GENDERS.includes(gender)) {
    throw new TypeError(`appearance gender must be one of ${GENDERS.join(', ')}`);
  }
  return gender;
}

function makeOption(row, part, gender) {
  return Object.freeze({ name: row[0], part, gender, group: row[1] ?? null });
}

// One gender's catalog: the options of every part and a name index per part.
class GenderCatalog {
  constructor(gender, data) {
    this.gender = gender;
    this.rows = {};
    this.index = {};
    for (const part of PARTS) {
      this.rows[part] = data[part];
      this.index[part] = new Map(data[part].map((row, at) => [row[0], at]));
    }
  }
  options(part) {
    return this.rows[part].map((row) => makeOption(row, part, this.gender));
  }
  find(part, name) {
    const at = this.index[part].get(name);
    return at === undefined ? null : makeOption(this.rows[part][at], part, this.gender);
  }
  has(part, name) {
    return this.index[part].has(name);
  }
  // The beards the face can wear; empty for a face this catalog does not carry.
  beards(head) {
    const at = this.index.head.get(head || DEFAULT_HEAD);
    if (at === undefined) {
      return [];
    }
    return this.rows.head[at][2].map((beard) =>
      makeOption(this.rows.beard[beard], 'beard', this.gender),
    );
  }
  canWear(head, beard) {
    return this.beards(head).some((option) => option.name === beard);
  }
}

const genders = {
  male: new GenderCatalog('male', catalog.male),
  female: new GenderCatalog('female', catalog.female),
};

function pick(list, random) {
  return list[Math.floor(random() * list.length)];
}

// The default body: every part the game's own.
function defaultAppearance() {
  return { gender: 'male', body: '', head: '', hair: '', beard: '' };
}

// The appearance a published look reports; the default body for anything else.
function appearanceFrom(data) {
  const appearance = defaultAppearance();
  if (data && GENDERS.includes(data.gender)) {
    appearance.gender = data.gender;
    for (const part of PARTS) {
      appearance[part] = typeof data[part] === 'string' ? data[part] : '';
    }
  }
  return Object.freeze(appearance);
}

// What a player wearing `current` would wear after setAppearance(request), with the face and
// beard always named together; null when a name is not in the catalog, belongs to the other
// gender or the face cannot wear the beard. Throws for a request that is not an object of
// strings.
function resolveRequest(current, request) {
  if (request === null || typeof request !== 'object' || Array.isArray(request)) {
    throw new TypeError('setAppearance takes an object of appearance parts');
  }
  for (const key of Object.keys(request)) {
    if (key !== 'gender' && !PARTS.includes(key)) {
      throw new TypeError(`unknown appearance part ${key}`);
    }
    if (request[key] !== undefined && typeof request[key] !== 'string') {
      throw new TypeError(`appearance ${key} must be a string`);
    }
  }
  const gender = request.gender === undefined ? current.gender : request.gender;
  if (!GENDERS.includes(gender)) {
    return null;
  }
  // Names from the other gender's tree mean nothing on this one: a gender change starts from the
  // game's own parts.
  const kept = gender === current.gender ? current : defaultAppearance();
  const next = { gender };
  for (const part of PARTS) {
    next[part] = request[part] === undefined ? kept[part] : request[part];
  }
  const tree = genders[gender];
  for (const part of ['body', 'head', 'hair']) {
    if (next[part] !== '' && !tree.has(part, next[part])) {
      return null;
    }
  }
  if (next.beard !== '' && !tree.canWear(next.head, next.beard)) {
    return null;
  }
  const named = { head: next.head, beard: next.beard };
  if (request.gender !== undefined || gender !== current.gender) {
    named.gender = gender;
  }
  for (const part of ['body', 'hair']) {
    if (request[part] !== undefined || gender !== current.gender) {
      named[part] = next[part];
    }
  }
  return named;
}

// The Appearances global of the server API.
const Appearances = Object.freeze({
  options(part, gender) {
    return genders[checkGender(gender)].options(checkPart(part));
  },
  beards(head = '', gender) {
    if (typeof head !== 'string') {
      throw new TypeError('head must be a string');
    }
    return genders[checkGender(gender)].beards(head);
  },
  find(part, name, gender) {
    if (typeof name !== 'string') {
      throw new TypeError('name must be a string');
    }
    return genders[checkGender(gender)].find(checkPart(part), name);
  },
  random(gender, random = Math.random) {
    const tree = genders[checkGender(gender)];
    const head = pick(tree.options('head'), random).name;
    const beards = tree.beards(head);
    return {
      gender: tree.gender,
      body: pick(tree.options('body'), random).name,
      head,
      hair: pick(tree.options('hair'), random).name,
      beard: beards.length === 0 ? '' : pick(beards, random).name,
    };
  },
});

module.exports = { Appearances, appearanceFrom, defaultAppearance, resolveRequest, PARTS };
