'use strict';

// The dice badges: badges.json, built from a game installation by
// Dev/tools/dice/build_badges.py, and what each badge does in a match. The numbers are the game's
// own (WHGame 1.5.5: the badge table its C_DiceBadge fills; the Formations score
// cvars wh_pl_Dice*FormationsScore; the badge rules of its dice model's context).
//
// Played by themselves:
// - headStart: the match starts with a share of the target score (tin 5%, silver 10%, gold 20%).
// - formations: one more combination a selection may be read with: tin the Cut (3+5) for 200,
//   silver the Gallows (4-5-6) for 400, gold the Eye (1-3-5) for 500.
// - null (Defence): the other player's badge of its tier or lower does nothing.
// - extraValue (the Emperor's): a face's three of a kind (and more) scores three times as much:
//   tin 2s, silver 5s, gold 1s (the game ships the gold one).
// - boostSixes (Tyche's): a selection with three sixes or more scores 600 for each six past two
//   that many times more (gold twice).
// Used by the player in their turn, as many times a match as usesOf says:
// - doubleTake (Doppelganger): banking, the banked selection scores twice.
// - multiplier (Warlord): banking, the whole turn scores 1.25, 1.5 or 2 times.
// - extraDice (Might): after a throw, a seventh die is thrown for the rest of the turn.
// - rerollDice (Fortune): after a throw, up to 1, 2 or 3 selected dice are thrown again.
// - rerollPips (Swap-out): after a throw, up to 1 (silver) or 2 (gold) selected dice of one face
//   are thrown again.
// - setDice (Transmutation): after a throw, a selected die shows a 3, a 5 or a 1 (gold: the joker
//   on a joker die).
// - balatro: after a throw, a selected joker die not showing its joker shows it.
// - antiBust (Resurrection): after a bust, the dice are thrown again or the turn is banked.

const badgeRows = require('./badges.json');
const { NO_RULES, JOKER } = require('./scoring.cjs');

const TIERS = Object.freeze(['tin', 'silver', 'gold']);
const EFFECTS = Object.freeze([
  'headStart',
  'formations',
  'null',
  'extraValue',
  'antiBust',
  'doubleTake',
  'multiplier',
  'extraDice',
  'rerollDice',
  'setDice',
  'rerollPips',
  'boostSixes',
  'balatro',
]);

const HEAD_START = Object.freeze([0.05, 0.1, 0.2]);
const FORMATIONS = Object.freeze([
  Object.freeze({ name: 'cut', faces: Object.freeze([3, 5]), points: 200 }),
  Object.freeze({ name: 'gallows', faces: Object.freeze([4, 5, 6]), points: 400 }),
  Object.freeze({ name: 'eye', faces: Object.freeze([1, 3, 5]), points: 500 }),
]);
const EXTRA_VALUE = 3;
const EXTRA_VALUE_FACES = Object.freeze([2, 5, 1]);
const BOOSTS = Object.freeze([1.25, 1.5, 2]);
// The banked selection's factor with the Doppelganger badge (the game's DiceDoubleTakeMultiplier).
const DOUBLE_TAKE = 2;
// How many dice a reroll badge throws again at most, and the face a Transmutation badge sets.
const REROLL_DICE = Object.freeze([1, 2, 3]);
const REROLL_PIPS = Object.freeze([0, 1, 2]);
const SET_DICE = Object.freeze([3, 5, 1]);
// The effects used as many times a match as their tier's number (1, 2, 3); the others once.
const COUNTED = new Set(['antiBust', 'doubleTake', 'extraDice', 'balatro']);

const BADGES = Object.freeze(
  badgeRows.map((row) =>
    Object.freeze({
      ...row,
      effectIndex: EFFECTS.indexOf(row.effect),
      tierIndex: TIERS.indexOf(row.tier),
    }),
  ),
);
const BADGE_BY_KEY = new Map(
  BADGES.flatMap((badge) => [
    [badge.id, badge],
    [badge.name.toLowerCase(), badge],
  ]),
);

// A badge by id or name (any case); null for none (undefined or null).
function checkBadge(value, option) {
  if (value === undefined || value === null) {
    return null;
  }
  const badge = typeof value === 'string' ? BADGE_BY_KEY.get(value.toLowerCase()) : undefined;
  if (!badge) {
    throw new TypeError(`Dice.start: ${option}: ${String(value)} is not a dice badge`);
  }
  return badge;
}

// The badge a player plays with: none when the other player's Defence badge is of its tier or
// higher.
function inEffect(own, other) {
  if (!own) {
    return null;
  }
  const defence = other && other.effect === 'null' && own.effect !== 'null';
  if (defence && other.tierIndex >= own.tierIndex) {
    return null;
  }
  return own;
}

// How many times a player may use their badge in a match.
function usesOf(badge) {
  if (!badge) {
    return 0;
  }
  return COUNTED.has(badge.effect) ? badge.tierIndex + 1 : 1;
}

// The points a player starts with.
function headStart(badge, target) {
  if (!badge || badge.effect !== 'headStart') {
    return 0;
  }
  // As the game reckons it, in single precision.
  return Math.trunc(Math.fround(Math.fround(HEAD_START[badge.tierIndex]) * target));
}

// The scoring rules a player's selections are read by (scoring.cjs).
function rulesOf(badge) {
  if (!badge) {
    return NO_RULES;
  }
  if (badge.effect === 'formations') {
    return Object.freeze({
      formations: Object.freeze([FORMATIONS[badge.tierIndex]]),
      boosts: NO_RULES.boosts,
    });
  }
  if (badge.effect === 'extraValue') {
    return Object.freeze({
      formations: NO_RULES.formations,
      boosts: Object.freeze({ [EXTRA_VALUE_FACES[badge.tierIndex]]: EXTRA_VALUE }),
    });
  }
  if (badge.effect === 'boostSixes') {
    return Object.freeze({
      formations: NO_RULES.formations,
      boosts: NO_RULES.boosts,
      sixes: BOOSTS[badge.tierIndex],
    });
  }
  return NO_RULES;
}

// The points a turn banks with a Doppelganger or Warlord badge used: the selection banked and the
// turn's points so far.
function bankedWith(badge, points, turnScore) {
  if (badge.effect === 'doubleTake') {
    return turnScore + Math.trunc(points * DOUBLE_TAKE);
  }
  return Math.trunc((points + turnScore) * Math.fround(BOOSTS[badge.tierIndex]));
}

// What a badge does to the selected dice of a throw (their faces, JOKER a joker; jokerDice whether
// each has a joker side): null when they are thrown again, else the faces they show now.
// Undefined when it may not change them.
function changeOf(badge, faces, jokerDice) {
  const count = faces.length;
  if (badge.effect === 'rerollDice') {
    if (count >= 1 && count <= REROLL_DICE[badge.tierIndex]) {
      return null;
    }
    return undefined;
  }
  if (badge.effect === 'rerollPips') {
    const same = faces.every((face) => face === faces[0]);
    if (count >= 1 && count <= REROLL_PIPS[badge.tierIndex] && same) {
      return null;
    }
    return undefined;
  }
  if (count !== 1) {
    return undefined;
  }
  if (badge.effect === 'setDice') {
    const joker = badge.tierIndex === 2 && jokerDice[0];
    return [joker ? JOKER : SET_DICE[badge.tierIndex]];
  }
  if (badge.effect === 'balatro' && jokerDice[0] && faces[0] !== JOKER) {
    return [JOKER];
  }
  return undefined;
}

// What the start state tells a client of a badge: its item class, effect and tier; null for none.
function wireOf(badge) {
  if (!badge) {
    return null;
  }
  return { item: badge.id, effect: badge.effectIndex, tier: badge.tierIndex };
}

module.exports = {
  BADGES,
  EFFECTS,
  TIERS,
  checkBadge,
  inEffect,
  usesOf,
  headStart,
  rulesOf,
  bankedWith,
  changeOf,
  wireOf,
};
