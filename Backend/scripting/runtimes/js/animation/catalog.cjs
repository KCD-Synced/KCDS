'use strict';

// The game's animations on a human body and the props it puts in people's hands (Animations),
// from catalog.json (Dev/tools/animations/build_catalog.py: the game's own
// actor_anim_action rows of the male and female bodies and its NPCTool items).
//
//   Animations.list(search?) -> AnimationInfo[]   one per fragment and tags, sorted by fragment;
//                                                  `search` keeps the rows whose fragment or tags
//                                                  contain it, ignoring case
//   Animations.props()       -> AnimationPropInfo[] sorted by name
//
// Every call answers fresh objects, so a script changing one changes nothing here. playsOn tells
// Player.playAnimation that a listed fragment is not for this player's body, which it refuses
// instead of playing nothing: a man plays the male rows; a woman the rows a player's body has
// every player body, a woman's too, plays with the player's own animation database,
// which has the men's fragments and none that only the game's women have (a bartender's).

const catalog = require('./catalog.json');
const { resolveMesh } = require('../props/catalog.cjs');

const ONE_SHOT = 1;
const FULL_BODY = 2;
const MALE = 4;
const FEMALE = 8;
const SLAVE_BODY = 16;
const PLAYER = 32;

function infoOf(row) {
  const [fragment, tags, duration, flags, rightHand, leftHand, sequence, phase, partner, clip] =
    row;
  return {
    fragment,
    tags,
    duration,
    oneShot: (flags & ONE_SHOT) !== 0,
    fullBody: (flags & FULL_BODY) !== 0,
    rightHand,
    leftHand,
    male: (flags & MALE) !== 0,
    female: (flags & FEMALE) !== 0,
    sequence,
    phase,
    partner,
    slaveBody: (flags & SLAVE_BODY) !== 0,
    // A player's body has it as a fragment (a woman plays the female rows by their clip).
    player: (flags & PLAYER) !== 0,
    // The fragment's first clip, by its name in the body's animation set ('' for none).
    clip: clip ?? '',
  };
}

// Whether a player's body of that gender plays the row: a man the male rows, a woman
// the player's rows and the rows only the game's women have, by their clip.
function playableBy(row, gender) {
  if (gender === 'male') {
    return (row[3] & MALE) !== 0;
  }
  return (row[3] & PLAYER) !== 0 || (Boolean(row[9]) && (row[3] & FEMALE) !== 0);
}

function list(search, options = {}) {
  if (search !== undefined && search !== null && typeof search !== 'string') {
    throw new TypeError('Animations.list takes a search text');
  }
  if (options === null || typeof options !== 'object') {
    throw new TypeError('Animations.list options are { gender }');
  }
  const { gender } = options;
  if (gender !== undefined && gender !== 'male' && gender !== 'female') {
    throw new TypeError("Animations.list gender is 'male' or 'female'");
  }
  const needle = (search ?? '').toLowerCase();
  const rows = catalog.animations.filter(
    (row) =>
      (!needle || row[0].toLowerCase().includes(needle) || row[1].toLowerCase().includes(needle)) &&
      (gender === undefined || playableBy(row, gender)),
  );
  return rows.map(infoOf);
}

// The props with their mesh in the form playAnimation's `model` takes (objects/....cgf), when the
// prop catalog carries it.
function props() {
  return catalog.props.map(([name, model, hand]) => ({
    name,
    model: resolveMesh(model) ?? model,
    hand,
  }));
}

// The prop of the game's animation props by its name (any case), or null.
function propNamed(name) {
  const needle = name.toLowerCase();
  const row = catalog.props.find(([propName]) => propName.toLowerCase() === needle);
  return row ? { name: row[0], model: resolveMesh(row[1]) ?? row[1], hand: row[2] } : null;
}

// Rows by fragment, for playsOn.
let byFragment = null;

// Whether a player's body of this gender plays the fragment (with these tags when the catalog
// lists them): a man's the male rows, a woman's the player's rows. A fragment the catalog does not
// list is the game's to judge, so true.
function playsOn(fragment, tags, gender) {
  if (typeof fragment !== 'string' || fragment === '') {
    return true;
  }
  if (!byFragment) {
    byFragment = new Map();
    for (const row of catalog.animations) {
      const rows = byFragment.get(row[0]) ?? [];
      rows.push(row);
      byFragment.set(row[0], rows);
    }
  }
  const rows = byFragment.get(fragment);
  if (!rows) {
    return true;
  }
  const tagged = typeof tags === 'string' ? rows.filter((row) => row[1] === tags) : [];
  const flag = gender === 'female' ? PLAYER : MALE;
  return (tagged.length > 0 ? tagged : rows).some((row) => (row[3] & flag) !== 0);
}

// What a woman's playAnimation plays for a fragment only the game's women have: its
// clip with these tags (else its first) and whether it repeats, null for any other fragment.
function femaleClipOf(fragment, tags) {
  const rows = catalog.animations.filter(
    (row) => row[0] === fragment && row[9] && (row[3] & FEMALE) !== 0 && (row[3] & PLAYER) === 0,
  );
  const row = rows.find((candidate) => candidate[1] === tags) ?? rows[0];
  return row ? { clip: row[9], loop: (row[3] & ONE_SHOT) === 0 } : null;
}

// Whether the catalog lists the fragment (a paired animation's Slave).
function listed(fragment) {
  playsOn(fragment, undefined, 'male');
  return byFragment?.has(fragment) === true;
}

// Animations.clipLength(name): how long one of the game's clips runs, in
// seconds, by its name or its path (animations/.../name.caf, any case), as the game's own rows
// that play it say; null for a clip no row plays (a resource's own .caf, or one only a quest uses).
function clipLength(name) {
  if (typeof name !== 'string' || name === '') {
    throw new TypeError('Animations.clipLength takes a clip name');
  }
  const wanted = name
    .replace(/\\/g, '/')
    .split('/')
    .pop()
    .replace(/\.caf$/i, '')
    .toLowerCase();
  const row = catalog.animations.find((each) => (each[9] ?? '').toLowerCase() === wanted);
  return row ? row[2] : null;
}

function api() {
  return Object.freeze({ list, props, clipLength });
}

// The clip a fragment with these tags plays (its option with them, else its first), '' for none:
// what playAnimation's upperBody plays on a layer of its own.
function clipOf(fragment, tags) {
  const rows = catalog.animations.filter((row) => row[0] === fragment && row[9]);
  const exact = rows.find((row) => row[1] === tags);
  return (exact ?? rows[0])?.[9] ?? '';
}

module.exports = {
  api,
  list,
  props,
  propNamed,
  playsOn,
  clipOf,
  femaleClipOf,
  listed,
  clipLength,
};
