'use strict';

// The local player's progression events: progressionXpGained,
// progressionLevelUp, progressionPerkAdded and progressionPerkRemoved from consecutive
// directories (the player's own entry carries XP and points; the first one is a baseline), and
// progressionPerkRefused for a server refusal of a perk the player confirmed on the perk
// screen (the native client's progressionPerkRefused message).

const {
  progressionChanges,
} = require('../../Backend/scripting/runtimes/js/progression/player_progression.cjs');
const { lookupPerk } = require('../../Backend/scripting/runtimes/js/progression/catalog.cjs');

const REFUSALS = new Set([
  'points',
  'level',
  'parent',
  'exclusive',
  'blocked',
  'script',
  'owned',
  'hidden',
]);

class ResourceProgression {
  constructor({ emit }) {
    this.emit = emit;
    this.localId = 0;
    this.previous = null;
  }
  // The local player's progression as a new directory carries it (null before the first report).
  receiveDirectory(localId, progression) {
    const own = progression && progression.mainPoints !== null ? progression : null;
    if (localId !== this.localId) {
      this.localId = localId;
      this.previous = own;
      return;
    }
    const changes = progressionChanges(this.previous, own);
    if (own) {
      this.previous = own;
    }
    for (const { track, xp } of changes.xp) {
      this.emit('progressionXpGained', [track, xp]);
    }
    for (const { track, level, perkPoints } of changes.levels) {
      this.emit('progressionLevelUp', [track, level, perkPoints]);
    }
    for (const perk of changes.added) {
      this.emit('progressionPerkAdded', [perk]);
    }
    for (const perk of changes.removed) {
      this.emit('progressionPerkRemoved', [perk]);
    }
  }
  // { perk: GUID, reason } from the native client; anything else is dropped.
  receiveRefusal({ perk, reason }) {
    const info = lookupPerk(perk);
    if (!info || !REFUSALS.has(reason)) {
      return;
    }
    this.emit('progressionPerkRefused', [info.name, reason]);
  }
  reset() {
    this.localId = 0;
    this.previous = null;
  }
}

module.exports = { ResourceProgression };
