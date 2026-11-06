'use strict';

// The status effects on a player's body as the server keeps them from the player's
// own reports, and as the player's own client runtime keeps them from its native client:
// Player.buffs, hasBuff and the changes playerBuffAdded / playerBuffRemoved report.
//
// The kept form: a list of { buff (catalog entry), instance, server, since } where `since` is
// the clock (seconds) at which the buff was first seen.

const { buffInfo, lookupBuff } = require('./catalog.cjs');

const MAX_BUFFS = 64;

function invalid(what) {
  return new TypeError(`Invalid native player buffs: ${what}`);
}

// From a native report ([[GUID, instance, server]], the instance a 64-bit id in decimal text);
// a buff the catalog does not carry is left out.
function buffsFromReport(raw) {
  if (!Array.isArray(raw) || raw.length > MAX_BUFFS) {
    throw invalid('shape');
  }
  const entries = [];
  for (const item of raw) {
    const [id, instance, server] = Array.isArray(item) ? item : [];
    if (
      typeof id !== 'string' ||
      typeof instance !== 'string' ||
      !/^[0-9]{1,20}$/.test(instance) ||
      typeof server !== 'boolean'
    ) {
      throw invalid('entry');
    }
    const buff = lookupBuff(id);
    if (buff) {
      entries.push({ buff, instance, server });
    }
  }
  return entries;
}

function keyOf(entry) {
  return `${entry.buff.id}:${entry.instance}`;
}

// The kept buffs after a report taken at `now`: an instance that was already there keeps when
// it was first seen. `added` and `removed` are kept entries.
function applyReport(previous, reported, now) {
  const before = new Map((previous ?? []).map((entry) => [keyOf(entry), entry]));
  const kept = reported.map((entry) => before.get(keyOf(entry)) ?? { ...entry, since: now });
  const nowKeys = new Set(kept.map(keyOf));
  return {
    kept,
    added: kept.filter((entry) => !before.has(keyOf(entry))),
    removed: (previous ?? []).filter((entry) => !nowKeys.has(keyOf(entry))),
  };
}

// A knock-out's own state: `knockedOut`, setHealth(0) and revive() rule it, not the buff list. A
// script that saved the list and gave it back at the next login would hold the player down for good.
function isKnockOutBuff(buff) {
  return buffInfo(buff).class === 'unconsciousness';
}

// Player.buffs: BuffState for each kept buff at `now`.
function buffStates(kept, now) {
  return (kept ?? [])
    .filter((entry) => !isKnockOutBuff(entry.buff))
    .map((entry) => ({
      ...buffInfo(entry.buff),
      since: Math.max(0, Math.round((now - entry.since) * 1000) / 1000),
      source: entry.server ? 'server' : 'native',
    }));
}

// Player.hasBuff: whether the kept buffs carry it (a name or GUID).
function hasBuff(kept, query) {
  if (typeof query !== 'string') {
    throw new TypeError('hasBuff takes a buff name or GUID');
  }
  const buff = lookupBuff(query);
  return !!buff && (kept ?? []).some((entry) => entry.buff === buff);
}

// The game's injury of each limb: the buff its injuries are, by Injuries' names.
const INJURY_BUFFS = Object.freeze({
  head: 'injured_head',
  torso: 'injured_torso',
  leftArm: 'injured_left_arm',
  rightArm: 'injured_right_arm',
  leftLeg: 'injured_left_leg',
  rightLeg: 'injured_right_leg',
});

// Player.injuries: which limbs the kept buffs injure, all false without a report.
function injuriesOf(kept) {
  const names = new Set((kept ?? []).map((entry) => entry.buff.name));
  const injuries = {};
  for (const [limb, buff] of Object.entries(INJURY_BUFFS)) {
    injuries[limb] = names.has(buff);
  }
  return Object.freeze(injuries);
}

// The limbs injured in `after` and not in `before`, and those healed.
function injuryChanges(before, after) {
  const limbs = Object.keys(INJURY_BUFFS);
  return {
    injured: limbs.filter((limb) => after[limb] && !before[limb]),
    healed: limbs.filter((limb) => before[limb] && !after[limb]),
  };
}

module.exports = {
  buffsFromReport,
  applyReport,
  buffStates,
  hasBuff,
  isKnockOutBuff,
  injuriesOf,
  injuryChanges,
};
