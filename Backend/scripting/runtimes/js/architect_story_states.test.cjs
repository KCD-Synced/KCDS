'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  statePath,
  storyStates,
} = require('../../../../Client/resources/architect/story_states.cjs');

const yard = 'Main/tneb_nebakov/fortress/series_Predhradi';

test("a layer's state is its place in the tree up to its first helper part", () => {
  assert.equal(statePath(`${yard}/beforeSiege/_enviro`), `${yard}/beforeSiege`);
  assert.equal(statePath(`${yard}/beforeSiege/script/crime`), `${yard}/beforeSiege`);
  assert.equal(statePath(`${yard}/beforeSiege`), `${yard}/beforeSiege`);
  assert.equal(statePath('Main/_quest/main/m11/streamed/x'), 'Main');
  assert.equal(statePath(''), '');
});

test("one state of a place gathers its layers, a meshes-only layer of the state's name too", () => {
  const layer = (name, path, brushes, shown, on = true) => ({
    name: `${name}_00000000-0000-0000-0000-00000000000${brushes % 10}`,
    short: name,
    path,
    on,
    brushes,
    shown,
    at: { x: brushes, y: 0, z: 0 },
  });
  const states = storyStates([
    layer('_enviro', `${yard}/afterSiege/_enviro`, 320, 0),
    layer('_enviro', `${yard}/beforeSiege/_enviro`, 248, 248),
    layer('_common', `${yard}/beforeSiege/_common`, 21, 21),
    layer('beforeSiege', `${yard}/beforeSiege`, 104, 104),
    layer('afterSiege', '', 14, 0),
    layer('crime', `${yard}/beforeSiege/script/crime`, 0, 0, false),
    layer('lonely', '', 3, 3),
  ]);
  assert.deepEqual(
    states.map((state) => [state.short, state.where, state.layers.length, state.on, state.shown]),
    [
      ['afterSiege', 'tneb_nebakov / fortress / series_Predhradi', 2, false, 0],
      ['beforeSiege', 'tneb_nebakov / fortress / series_Predhradi', 4, true, 373],
      ['lonely', '', 1, true, 3],
    ],
  );
  assert.equal(states[0].brushes, 334);
});
