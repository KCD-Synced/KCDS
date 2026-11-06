'use strict';

// The server's rules on progression: what a player's own game may earn from play
// (Progression.setGameXp, setXpGain, setXpCap, setMaxLevel), the perks no player may obtain
// (Progression.lockPerks), and the ledger that catches progression the server never granted
// (playerProgressionRejected).
//
// Every XP gain of a player's game is held back on their client and asked for (XP_GAINING); the
// server grants what these rules allow with an AddXp order. The ledger counts, per player and
// track, the XP the server allowed (its own orders and the gains it granted, with the most the
// player's multipliers could make of them); a report that rose further is rejected.

const { MAIN_BANK, TRACKS, requirePerk, requireTrack, totalXp } = require('./catalog.cjs');

const ALL = '*';
// The most a player's own XP multipliers (perks, potions) can make of a gain. A generous bound:
// the ledger is there to catch a client that writes its own levels, not to price perks.
const MAX_MULTIPLIER = 3;
// Rounding the ledger forgives (fixed point XP, multipliers).
const TOLERANCE = 1;
const MAX_RATE = 1000;
const MAX_BUDGET = 1000000;

const TRACK_INDEX = new Map(TRACKS.map((track, index) => [track.name, index]));

function trackOrAll(name) {
  if (name !== ALL) requireTrack(name);
  return name;
}

// A perk of the perk screen: shown there, paid for from a tree, never granted by the game itself.
function perkScreenPerk(perk) {
  return perk.track !== null && perk.visibility === 'visible' && !perk.autolearnable;
}

// The unspent points of the tree that pays for a perk.
function treePoints(progression, perk) {
  if (perk.track === MAIN_BANK) return progression.mainPoints;
  return progression.tracks[TRACK_INDEX.get(perk.track)].points;
}

class ProgressionRules {
  constructor({ clock = () => performance.now() / 1000 } = {}) {
    this.clock = clock;
    this.gameXp = true;
    this.rates = new Map();
    this.budgets = new Map();
    this.caps = new Map();
    // Per player: the XP each track may still rise by, and each track's budget bucket.
    this.allowances = new Map();
    this.buckets = new Map();
    // The perks no player may obtain but from the server's own orders, by name.
    this.blocked = new Set();
  }

  setGameXp(enabled) {
    if (typeof enabled !== 'boolean') throw new TypeError('setGameXp takes a boolean');
    const changed = this.gameXp !== enabled;
    this.gameXp = enabled;
    return changed;
  }
  setXpGain(track, rate) {
    trackOrAll(track);
    if (typeof rate !== 'number' || !Number.isFinite(rate) || rate < 0 || rate > MAX_RATE) {
      throw new RangeError(`setXpGain takes a rate from 0 to ${MAX_RATE}`);
    }
    this.rates.set(track, rate);
  }
  // The rate a track's gains from play get: its own, else the one for all tracks, else 1.
  xpRateOf(track) {
    trackOrAll(track);
    return this.ruleOf(this.rates, track, 1);
  }
  setXpCap(track, xpPerMinute) {
    trackOrAll(track);
    if (
      typeof xpPerMinute !== 'number' ||
      !Number.isFinite(xpPerMinute) ||
      xpPerMinute < 0 ||
      xpPerMinute > MAX_BUDGET
    ) {
      throw new RangeError(`setXpCap takes 0 (unlimited) to ${MAX_BUDGET} XP per minute`);
    }
    this.budgets.set(track, xpPerMinute);
    // A new budget starts full.
    if (track === ALL) {
      this.buckets.clear();
      return;
    }
    for (const buckets of this.buckets.values()) buckets.delete(track);
  }
  setMaxLevel(track, level) {
    const cap = track === ALL ? 30 : requireTrack(track).cap;
    trackOrAll(track);
    if (!Number.isInteger(level) || level < 0 || level > cap) {
      throw new RangeError(`setMaxLevel takes a level from 0 to ${cap}`);
    }
    this.caps.set(track, level);
  }

  // Blocks (or unblocks) perks by name or GUID; true when the list changed. Throws for an unknown
  // perk, before anything changes.
  setBlocked(perks, blocked) {
    if (!Array.isArray(perks)) {
      throw new TypeError(`${blocked ? 'lockPerks' : 'unlockPerks'} takes an array of perks`);
    }
    const names = perks.map((perk) => requirePerk(perk).name);
    let changed = false;
    for (const name of names) {
      if (blocked && !this.blocked.has(name)) {
        this.blocked.add(name);
        changed = true;
      } else if (!blocked && this.blocked.delete(name)) {
        changed = true;
      }
    }
    return changed;
  }
  get lockedPerks() {
    return [...this.blocked].sort();
  }
  isBlocked(name) {
    return this.blocked.has(name);
  }

  ruleOf(map, track, fallback) {
    return map.get(track) ?? map.get(ALL) ?? fallback;
  }

  // What the server grants of a gain the player's game asked for (already accepted by the
  // resources): { xp, multipliers } or null for nothing. `state`: the track as last reported.
  grant(playerId, trackName, xp, multipliers, state) {
    const track = requireTrack(trackName);
    let amount = xp * this.ruleOf(this.rates, trackName, 1);
    const budget = this.ruleOf(this.budgets, trackName, 0);
    if (budget > 0) amount = this.spend(playerId, trackName, budget, amount);
    const capLevel = Math.min(this.ruleOf(this.caps, trackName, track.cap), track.cap);
    if (state) {
      if (state.level >= capLevel) return null;
      const left = totalXp(track, capLevel) - (totalXp(track, state.level) + state.xp);
      // No multiplier may carry the track past its cap: a gain close to it is granted exactly
      // what is left.
      const most = multipliers ? amount * MAX_MULTIPLIER : amount;
      if (most >= left) {
        amount = Math.min(amount, left);
        multipliers = false;
      }
    }
    if (!(amount >= 1 / 128)) return null;
    this.allow(playerId, trackName, multipliers ? amount * MAX_MULTIPLIER : amount);
    return { xp: amount, multipliers };
  }

  // The token bucket of a player's track: a minute's worth, refilled steadily.
  spend(playerId, trackName, perMinute, amount) {
    const now = this.clock();
    let buckets = this.buckets.get(playerId);
    if (!buckets) {
      buckets = new Map();
      this.buckets.set(playerId, buckets);
    }
    const bucket = buckets.get(trackName) ?? { tokens: perMinute, at: now };
    bucket.tokens = Math.min(perMinute, bucket.tokens + ((now - bucket.at) * perMinute) / 60);
    bucket.at = now;
    const granted = Math.min(amount, bucket.tokens);
    bucket.tokens -= granted;
    buckets.set(trackName, bucket);
    return granted;
  }

  // The ledger: XP a track may rise by because the server granted or ordered it.
  allow(playerId, trackName, xp) {
    let allowances = this.allowances.get(playerId);
    if (!allowances) {
      allowances = new Map();
      this.allowances.set(playerId, allowances);
    }
    allowances.set(trackName, (allowances.get(trackName) ?? 0) + xp);
  }

  // A report's XP gains against the ledger: the reasons of the tracks that rose further than the
  // server allowed (their allowance starts over). Fencing, which the game derives from the
  // weapon skills, is not counted.
  check(playerId, gains) {
    const allowances = this.allowances.get(playerId) ?? new Map();
    this.allowances.set(playerId, allowances);
    const reasons = [];
    for (const { track, xp } of gains) {
      if (requireTrack(track).induced) continue;
      const left = (allowances.get(track) ?? 0) - xp;
      if (left < -TOLERANCE) {
        reasons.push(`${track} rose by ${round(xp)} XP, more than this server granted`);
        allowances.set(track, 0);
      } else {
        allowances.set(track, Math.max(left, 0));
      }
    }
    return reasons;
  }

  // A report's perks the game added by itself (neither learnt with the server's leave nor
  // ordered) against the rules: the reasons of a blocked perk, and of a perk-screen perk whose
  // tree spent a point for it, that is one learnt on the perk screen without asking.
  checkPerks(previous, current, nativelyAdded) {
    const reasons = [];
    for (const name of nativelyAdded) {
      const perk = requirePerk(name);
      if (this.blocked.has(name)) {
        reasons.push(`${name} is blocked on this server`);
      } else if (
        perkScreenPerk(perk) &&
        previous &&
        treePoints(current, perk) < treePoints(previous, perk)
      ) {
        reasons.push(`${name} was learnt without this server`);
      }
    }
    return reasons;
  }

  forget(playerId) {
    this.allowances.delete(playerId);
    this.buckets.delete(playerId);
  }
}

function round(xp) {
  return Math.round(xp * 100) / 100;
}

module.exports = {
  ProgressionRules,
  MAX_MULTIPLIER,
  TRACK_NAMES: TRACKS.map((t) => t.name),
  perkScreenPerk,
};
