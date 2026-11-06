'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceProgression } = require('../../../../Client/resources/progression.cjs');
const {
  progressionFromReport,
  progressionFromDirectory,
  directoryProgression,
} = require('./progression/player_progression.cjs');
const { progressionCatalog } = require('./progression/catalog.cjs');

const PACIFIST = '2b9f3c56-d5f4-49b0-a259-8a89a67c8f0d';
const WANDERER = 'd8b59369-03a5-4c53-adcf-78530a00dcca';

// The local player's progression as their own directory carries it: every track at level 5,
// survival as given.
function ownDirectory(survival, perks) {
  const tracks = progressionCatalog
    .tracks()
    .map((track) => (track.name === 'survival' ? survival : [5, 0, 0]));
  const report = progressionFromReport({ tracks, mainPoints: 1, perks });
  return progressionFromDirectory(directoryProgression(report, true));
}

function fixture() {
  const events = [];
  const progression = new ResourceProgression({
    emit: (name, args) => events.push([name, ...args]),
  });
  return { progression, events };
}

test('client progression events: a first directory is a baseline; XP, levels, perks after', () => {
  const { progression, events } = fixture();
  progression.receiveDirectory(7, null);
  progression.receiveDirectory(7, ownDirectory([19, 50, 7], [PACIFIST]));
  assert.deepEqual(events, []);
  // Survival from level 19 with 50 XP to level 20 with 10 XP (620 - 50 + 10 XP), a point spent on
  // Wanderer and Pacifist taken away.
  progression.receiveDirectory(7, ownDirectory([20, 10, 7], [WANDERER]));
  assert.deepEqual(events, [
    ['progressionXpGained', 'survival', 580],
    ['progressionLevelUp', 'survival', 20, 7],
    ['progressionPerkAdded', 'Wanderer'],
    ['progressionPerkRemoved', 'Pacifist'],
  ]);
  // The same directory again changes nothing; a new local player starts a new baseline.
  progression.receiveDirectory(7, ownDirectory([20, 10, 7], [WANDERER]));
  progression.receiveDirectory(9, ownDirectory([3, 0, 0], []));
  progression.reset();
  progression.receiveDirectory(9, ownDirectory([20, 10, 7], [WANDERER]));
  assert.equal(events.length, 4);
});

test('progressionPerkRefused names the perk; unknown perks and reasons are dropped', () => {
  const { progression, events } = fixture();
  progression.receiveRefusal({ perk: WANDERER, reason: 'blocked' });
  progression.receiveRefusal({ perk: PACIFIST.toUpperCase(), reason: 'script' });
  progression.receiveRefusal({ perk: 'not-a-perk', reason: 'script' });
  progression.receiveRefusal({ perk: WANDERER, reason: 'because' });
  assert.deepEqual(events, [
    ['progressionPerkRefused', 'Wanderer', 'blocked'],
    ['progressionPerkRefused', 'Pacifist', 'script'],
  ]);
});
