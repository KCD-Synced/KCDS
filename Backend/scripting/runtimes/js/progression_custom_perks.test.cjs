'use strict';

// A resource's own perks: its perk table patch joins the server's progression
// catalog, and a player's perks travel in the directory with them.
const test = require('node:test');
const assert = require('node:assert/strict');

const {
  progressionCatalog,
  addPerkTablePatch,
  isCustomPerk,
  lookupPerk,
  TRACKS,
} = require('./progression/catalog.cjs');
const {
  progressionFromReport,
  directoryProgression,
  progressionFromDirectory,
} = require('./progression/player_progression.cjs');

const PERK_ID = '6b1f3c2e-951a-4d7e-9a51-c0de00000951';
const PATCH = `<?xml version="1.0" encoding="us-ascii"?>
<database name="barbora">
  <perks version="1">
    <perk autolearnable="false" icon_id="perk_onslaught" level="3" perk_id="${PERK_ID}" perk_name="MyServerPerk" perk_ui_name="perk_onslaught_name" skill_selector="17" visibility="2" />
    <perk autolearnable="false" perk_id="5693169d-c28a-4e18-af95-770927d363cd" perk_name="Adept of mystic arts" visibility="2" />
  </perks>
</database>
`;

function report(perks) {
  const tracks = progressionCatalog.tracks().map(() => [5, 0, 0]);
  return { tracks, mainPoints: 0, perks };
}

test("a resource's perk table patch adds its perks to the catalog, not the game's own again", () => {
  const skill = TRACKS.find((track) => track.kind === 'skill');
  assert.deepEqual(
    addPerkTablePatch(PATCH.replace('skill_selector="17"', `skill_selector="${skill.id}"`)),
    ['MyServerPerk'],
  );
  const perk = progressionCatalog.perkNamed('MyServerPerk');
  assert.equal(perk.id, PERK_ID);
  assert.equal(perk.track, skill.name);
  assert.equal(perk.level, 3);
  assert.equal(perk.visibility, 'visible');
  assert.equal(isCustomPerk(lookupPerk(PERK_ID)), true);
  assert.equal(isCustomPerk(lookupPerk('Adept of mystic arts')), false);
  assert.ok(progressionCatalog.perks({ track: skill.name }).some((entry) => entry.id === PERK_ID));
  // The same patch again adds nothing.
  assert.deepEqual(addPerkTablePatch(PATCH), []);
});

test("a player's own perks travel in the directory after the game's perk bits", () => {
  const progression = progressionFromReport(
    report([PERK_ID, '5693169d-c28a-4e18-af95-770927d363cd']),
  );
  assert.deepEqual(progression.perks.map((perk) => perk.name).sort(), [
    'Adept of mystic arts',
    'MyServerPerk',
  ]);
  const entry = directoryProgression(progression, false);
  assert.match(entry.perks, new RegExp(`^[0-9a-f]+\\+${PERK_ID}$`));
  const back = progressionFromDirectory(entry);
  assert.deepEqual(back.perks.map((perk) => perk.name).sort(), [
    'Adept of mystic arts',
    'MyServerPerk',
  ]);
  // A viewer whose catalog lacks the perk gets it by its GUID.
  const stranger = progressionFromDirectory({
    ...entry,
    perks: `${entry.perks.split('+')[0]}+0f0f0f0f-0000-4000-8000-000000000001`,
  });
  assert.ok(stranger.perks.some((perk) => perk.name === '0f0f0f0f-0000-4000-8000-000000000001'));
  assert.throws(() => progressionFromDirectory({ ...entry, perks: `${entry.perks}+not-a-guid` }));
});
