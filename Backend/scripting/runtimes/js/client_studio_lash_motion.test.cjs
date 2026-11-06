'use strict';

// A whip's lash made from the hand's motion (Client/resources/studio/lash_motion.cjs):
// the lash simulated on the recorded handle's path and kept as a lash track, and lashQuality,
// which measures how a lash plays (its length, folds, jumps, the loop's seam).
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  lashMotion,
  bestLashMotion,
  lashQuality,
  lashSeen,
  TRACK_RATE,
  TRACK_SEGMENTS,
} = require('../../../../Client/resources/studio/lash_motion.cjs');
const { designLash, DESIGN } = require('../../../../Client/resources/studio/lash_design.cjs');
const { Animation } = require('../../../../Client/resources/studio/animation.cjs');

const SECONDS = 2.4;
const RATE = 30;

// A made-up overhead crack, recorded as the studio records it: the hand goes slowly back and up,
// fast forward and down, then back to rest, a round every 2.4 s; the handle points out of the hand
// along the swing.
function swing() {
  const frames = [];
  const handles = [];
  for (let index = 0; index < 80; index += 1) {
    const time = (index / RATE) % SECONDS;
    let angle;
    if (time < 1.6) {
      angle = -0.3 - 1.4 * Math.sin((time / 1.6) * (Math.PI / 2));
    } else if (time < 2.0) {
      angle = -1.7 + 2.6 * Math.sin(((time - 1.6) / 0.4) * (Math.PI / 2));
    } else {
      angle = 0.9 - 1.2 * ((time - 2.0) / 0.4);
    }
    frames.push([32 + 0.5 * Math.sin(angle), 32, 33.3 + 0.4 * Math.cos(angle), 0, 0, 0]);
    handles.push([Math.sin(angle), 0, Math.cos(angle), -Math.cos(angle), 0, Math.sin(angle)]);
  }
  return { rate: RATE, frames, handles };
}

function animationWith(lashTrack) {
  return {
    keys: [
      { time: 0, pose: {} },
      { time: SECONDS, pose: {} },
    ],
    loop: true,
    lashTrack,
  };
}

test('the lash made from the hand: its own frames, its whole length, no kinks, a crack', () => {
  // As the editor makes it: the best of its tries (one setting alone may fold this swing).
  const options = { length: 1.6, seconds: SECONDS, loop: true, ground: 32.01 };
  const made = bestLashMotion(swing(), options);
  assert.equal(made.error, undefined);
  assert.equal(made.track.rate, TRACK_RATE);
  assert.equal(made.track.frames.length, Math.round(SECONDS * TRACK_RATE));
  assert.equal(made.track.frames[0].length, TRACK_SEGMENTS * 2);
  // Whole degrees, the format's.
  assert.ok(made.track.frames.flat().every((part) => Number.isInteger(part)));
  // The wave from the hand speeds up along the lighter lash: its tip far faster than the hand
  // (this swing's hand is at most about 5 m/s).
  assert.ok(made.tipSpeed > 12, `${made.tipSpeed}`);
  const quality = lashQuality(animationWith(made.track), 1.6);
  // It rolls out straight at the strike; between strikes this swing's slow hand lets it dangle,
  // which the check tells (and nothing else).
  assert.ok(quality.reach.most >= 0.95, `${quality.reach.most}`);
  assert.deepEqual(
    quality.problems.filter((text) => !text.includes('balled up')),
    [],
  );
  assert.deepEqual(quality.length, [1.6, 1.6]);
  assert.ok(quality.fold.most <= 110, `${quality.fold.most}`);
  assert.equal(quality.shapedBy, `a lash track, ${TRACK_RATE} frames a second`);
});

test('the best of a few frictions, stiffnesses and drags is kept, unless the options give them', () => {
  const options = { length: 1.6, seconds: SECONDS, loop: true, ground: 32.01 };
  const best = bestLashMotion(swing(), options);
  assert.equal(best.tried.length, 12);
  const least = Math.min(...best.tried.map((one) => one.badness));
  const chosen = best.tried.find((one) => one.settings === best.chosen);
  assert.equal(chosen.badness, least);
  assert.equal(bestLashMotion(swing(), { ...options, stiffTip: 0.1 }).tried.length, 1);
});

test('the designed lash cracks on the throw it finds and never whirls or balls up', () => {
  const made = designLash(swing(), { length: 1.6, seconds: SECONDS, loop: true, ground: 32.01 });
  // The swing's fast part is from 1.6 to 2.0 s, forward and down; the hand's fastest near 1.8 s.
  assert.ok(made.throw.start >= 1.55 && made.throw.end <= 2.1, JSON.stringify(made.throw));
  assert.equal(made.track.frames.length, Math.round(SECONDS * TRACK_RATE));
  const quality = lashQuality(animationWith(made.track), 1.6);
  // This made-up hand turns straight back into the lash at the top of its swing, so the lash
  // folds at the handle there (a recorded crack's hand does not); it never balls up or jumps,
  // rolls out straight and keeps its length.
  assert.deepEqual(
    quality.problems.filter((text) => !text.includes('folds')),
    [],
  );
  assert.equal(quality.reach.coiled, 0);
  assert.deepEqual(quality.jumps, []);
  assert.ok(quality.reach.most >= 0.95, `${quality.reach.most}`);
  assert.deepEqual(quality.length, [1.6, 1.6]);
});

test('a lash that turns all of itself in a frame snaps; the designed roll does not', () => {
  // Hanging down for half a second, then pointing straight out in the next frame.
  const frames = [];
  for (let frame = 0; frame < 40; frame += 1) {
    frames.push(frame < 20 ? [0, -90, 0, -90] : [0, 0, 0, 0]);
  }
  const snapping = lashQuality(animationWith({ rate: 40, length: 1.6, frames }), 1.6);
  assert.ok(snapping.snaps.length > 0);
  assert.match(snapping.problems.join(), /snaps into its next shape/);
  const options = { length: 1.6, seconds: SECONDS, loop: true, ground: 32.01 };
  const made = designLash(swing(), options);
  assert.deepEqual(lashQuality(animationWith(made.track), 1.6).snaps, []);
  // With the body's frame in the recording the track is kept in it (the wrist's turns do not
  // swing the lash); the studio's animation keeps that.
  const withBody = designLash({ ...swing(), body: [0, 1, 0, 0, 0, 1] }, options);
  assert.equal(withBody.track.space, 'body');
  assert.equal(made.track.space, undefined);
  const animation = new Animation();
  animation.restore(animationWith(withBody.track));
  assert.equal(animation.toJSON().lashTrack.space, 'body');
  // The design's numbers: given ones kept in their range, the rest the defaults.
  const tuned = designLash(swing(), { ...options, design: { turnSeconds: 9, rollPower: 2 } });
  assert.equal(tuned.design.turnSeconds, DESIGN.turnSeconds.range[1]);
  assert.equal(tuned.design.rollPower, 2);
  assert.equal(tuned.design.fallSpeed, DESIGN.fallSpeed.value);
});

test('the lash as drawn: a whole-lash turn at once is a snap, a frame drawn twice counts once', () => {
  // Three points (two pieces) a frame, 30 a second: still, then all of it turned 90° at once.
  const down = [0, 0, 2, 0, 0, 1, 0, 0, 0];
  const out = [0, 0, 2, 1, 0, 2, 2, 0, 2];
  const frames = [down, down, down, down, out, out, out];
  const seen = lashSeen({ rate: 30, frames });
  assert.equal(seen.snaps.length, 1);
  assert.match(seen.problems.join(), /snaps at 1 frames/);
  // Turning a little each frame is no snap.
  const turning = [0, 1, 2, 3, 4, 5].map((step) => {
    const angle = (step * 5 * Math.PI) / 180;
    return [
      0,
      0,
      2,
      Math.sin(angle),
      0,
      2 - Math.cos(angle),
      2 * Math.sin(angle),
      0,
      2 - 2 * Math.cos(angle),
    ];
  });
  assert.deepEqual(lashSeen({ rate: 30, frames: turning }).snaps, []);
});

test('a recording without the handle or shorter than the loop is no lash', () => {
  const { rate, frames } = swing();
  const options = { length: 1.6, seconds: 2.4, loop: true };
  assert.match(lashMotion({ rate, frames }, options).error, /handle/);
  const short = swing();
  short.frames = short.frames.slice(0, 30);
  short.handles = short.handles.slice(0, 30);
  assert.match(lashMotion(short, options).error, /recording is/);
});

test('the check finds what was wrong: few keys, a shrinking lash, a fold, a jump', () => {
  const straight = [0.4, 0.8, 1.2, 1.6].map((along) => [along, 0, 0]);
  // Folded back on itself at its middle.
  const folded = [
    [0.4, 0, 0],
    [0.8, 0, 0],
    [0.4, 0, 0.01],
    [0, 0, 0.02],
  ];
  const keys = [
    { time: 0, pose: {}, lash: straight },
    { time: 0.4, pose: {}, lash: folded },
    { time: 0.8, pose: {}, lash: straight },
  ];
  const quality = lashQuality({ keys, loop: true }, 1.6);
  assert.equal(quality.shapedBy, 'its keys');
  const said = quality.problems.join('\n');
  assert.match(said, /shaped only at 3 keys/);
  assert.match(said, /folds 1\d\d° at piece 2/);
  // A lash that is 1 m long against a 1.6 m whip.
  const short = [0.25, 0.5, 0.75, 1].map((along) => [along, 0, 0]);
  const shortKeys = [
    { time: 0, pose: {}, lash: short },
    { time: 0.04, pose: {}, lash: short },
  ];
  assert.match(lashQuality({ keys: shortKeys, loop: true }, 1.6).problems.join(), /1 to 1 m long/);
  // A track whose one frame swings the lash round and back: a jump.
  const frames = [];
  for (let frame = 0; frame < 40; frame += 1) {
    frames.push(frame === 20 ? [90, 0, 90, 0] : [0, 0, 0, 0]);
  }
  const jumping = lashQuality(animationWith({ rate: 40, length: 1.6, frames }), 1.6);
  assert.match(jumping.problems.join(), /jumps at/);
  assert.equal(lashQuality({ keys: [{ time: 0, pose: {} }] }, 1.6), null);
});
