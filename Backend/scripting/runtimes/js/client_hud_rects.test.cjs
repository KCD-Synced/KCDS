'use strict';

// Hud.rect (Client/resources/hud_rects.cjs): a HUD part's place on the screen from the game's
// HUD movie, through its clips' positions and scales.

const test = require('node:test');
const assert = require('node:assert/strict');
const { hudRect } = require('../../../../Client/resources/hud_rects.cjs');

// The game's answers in a 960 x 540 window (read live): the bottom centre at (480, 540) at half
// scale, the frame 60 up from it.
function game(overrides = {}) {
  const clips = {
    '_root.bc': { x: 480, y: 540, scaleX: 0.5, scaleY: 0.5, visible: true },
    '_root.bc.mc_StatsContainer': { x: 0, y: 0, scaleX: 1, scaleY: 1, visible: true },
    '_root.bc.mc_StatsContainer.mc_StatsEle': { x: 0, y: -60, scaleX: 1, scaleY: 1, visible: true },
    ...overrides,
  };
  return (request) =>
    request.op === 'elementGetClip' && request.name === 'hud' ? clips[request.clip] ?? null : null;
}

test('the vitals frame where the game draws it, at any HUD scale', () => {
  assert.deepEqual(hudRect('vitals', game()), { x: 385, y: 495, width: 190, height: 27 });
  // A 2560 x 1440 window: the game scales the bottom centre by 1440 / 1080.
  const large = game({
    '_root.bc': { x: 1280, y: 1440, scaleX: 4 / 3, scaleY: 4 / 3, visible: true },
  });
  assert.deepEqual(hudRect('vitals', large), { x: 1027, y: 1320, width: 507, height: 72 });
});

test('a hidden or missing HUD gives null, an unknown part throws', () => {
  const hidden = game({
    '_root.bc.mc_StatsContainer': { x: 0, y: 0, scaleX: 1, scaleY: 1, visible: false },
  });
  assert.equal(hudRect('vitals', hidden), null);
  assert.equal(
    hudRect('vitals', () => null),
    null,
  );
  assert.throws(() => hudRect('compass', game()), TypeError);
  assert.throws(() => hudRect('toString', game()), TypeError);
});
