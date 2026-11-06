'use strict';

// The assistant's animation tools (Client/resources/studio/animation_tools.cjs): some
// keys or joints changed, whole tracks, the timing, checks, comparisons and a lash's shape.
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  patchAnimation,
  trackAnimation,
  retimeAnimation,
  checkAnimation,
  compareAnimations,
  lashShape,
} = require('../../../../Client/resources/studio/animation_tools.cjs');

function animation() {
  return {
    keys: [
      { time: 0, pose: { RightArm: [10, 0, 0], LeftFoot: [100, 0, 0] }, lift: -0.05 },
      { time: 0.5, pose: { RightArm: [40, 20, 0], LeftFoot: [120, 0, 0] } },
      { time: 1, pose: { RightArm: [10, 0, 0], LeftFoot: [100, 0, 0] } },
    ],
    blend: 'spline',
    loop: true,
  };
}

test('a patch changes only the keys and joints it names', () => {
  const { animation: changed, keys } = patchAnimation(animation(), {
    times: [0.5],
    pose: { LeftFoot: null, Head: [5, 0, 0] },
    add: { RightArm: [0, 0, 15] },
    lift: -0.1,
  });
  assert.equal(keys, 1);
  assert.deepEqual(changed.keys[1].pose, { RightArm: [40, 20, 15], Head: [5, 0, 0] });
  assert.equal(changed.keys[1].lift, -0.1);
  // The others as they were; the given animation unchanged.
  assert.deepEqual(changed.keys[0], animation().keys[0]);
  assert.deepEqual(animation().keys[1].pose.LeftFoot, [120, 0, 0]);
  const removed = patchAnimation(animation(), { remove: ['LeftFoot'] }).animation;
  assert.ok(removed.keys.every((key) => !key.pose.LeftFoot));
});

test('tracks copy, mirror, scale, offset, smooth and go', () => {
  const mirrored = trackAnimation(animation(), { op: 'mirror', joint: 'LeftArm' }).animation;
  assert.deepEqual(mirrored.keys[1].pose.LeftArm, [40, -20, 0]);
  const all = trackAnimation(animation(), { op: 'mirror', joint: 'all', from: 'Left' }).animation;
  assert.deepEqual(all.keys[1].pose.RightFoot, [120, 0, 0]);
  const scaled = trackAnimation(animation(), { op: 'scale', joint: 'RightArm', by: 0.5 }).animation;
  assert.deepEqual(scaled.keys[1].pose.RightArm, [20, 10, 0]);
  const offset = trackAnimation(animation(), { op: 'offset', joint: 'LeftFoot', by: [-30, 0, 0] });
  assert.deepEqual(offset.animation.keys[0].pose.LeftFoot, [70, 0, 0]);
  const smooth = trackAnimation(animation(), { op: 'smooth', joint: 'RightArm' }).animation;
  // The middle key halfway toward its neighbours; round the loop the ends stay the same.
  assert.deepEqual(smooth.keys[1].pose.RightArm, [25, 10, 0]);
  assert.deepEqual(smooth.keys[0].pose.RightArm, smooth.keys[2].pose.RightArm);
  const copied = trackAnimation(animation(), { op: 'copy', joint: 'RightArm', to: 'Spine' });
  assert.deepEqual(copied.animation.keys[1].pose.Spine, [40, 20, 0]);
  assert.match(trackAnimation(animation(), { op: 'mirror', joint: 'Head' }).error, /Left or Right/);
});

test('the timing speeds up, trims and shifts', () => {
  assert.equal(retimeAnimation(animation(), { op: 'speed', factor: 2 }).length, 0.5);
  const trimmed = retimeAnimation(animation(), { op: 'trim', from: 0.5, to: 1 }).animation;
  const trimmedTimes = trimmed.keys.map((key) => key.time);
  assert.deepEqual(trimmedTimes, [0, 0.5]);
  const shifted = retimeAnimation(animation(), { op: 'shift', from: 0.25, by: 0.3 }).animation;
  const shiftedTimes = shifted.keys.map((key) => key.time);
  assert.deepEqual(shiftedTimes, [0, 0.8, 1.3]);
  assert.match(retimeAnimation(animation(), { op: 'shift', from: 0.25, by: -0.9 }).error, /order/);
});

test('a check finds jumps, seams, wrong lashes and the size as sent', () => {
  const clean = checkAnimation(animation());
  assert.deepEqual(clean.problems, []);
  assert.equal(clean.sent, 'in the player field');
  const bad = animation();
  // 350° in degrees is 10° of turn the long way round; 130° is a real fast turn.
  bad.keys[1].pose.RightArm = [360, 0, 0];
  bad.keys[1].pose.Spine = [130, 0, 0];
  bad.keys[2].pose.LeftFoot = [130, 0, 0];
  bad.keys[1].lash = [[1, 0, 0]];
  const report = checkAnimation(bad, { lashLength: 1.6 });
  const spin = /RightArm spins the long way .* change 350°, it turns 10°/;
  assert.ok(
    report.problems.some((one) => spin.test(one)),
    JSON.stringify(report.problems),
  );
  assert.ok(report.fastTurns.some((one) => /Spine turns 130°/.test(one)));
  assert.ok(report.problems.some((one) => /LeftFoot: the last key is 30°/.test(one)));
  assert.ok(report.problems.some((one) => /key 1: the lash is 1 m along/.test(one)));
});

test('a comparison and a lash shape', () => {
  const other = trackAnimation(animation(), { op: 'offset', joint: 'LeftFoot', by: [10, 0, 0] });
  const apart = compareAnimations(animation(), other.animation);
  assert.deepEqual(Object.keys(apart.joints)[0], 'LeftFoot');
  assert.equal(apart.joints.LeftFoot.most, 10);
  const straight = lashShape({ length: 1.2, count: 12 });
  assert.equal(straight.length, 12);
  assert.deepEqual(straight[11], [1.2, 0, 0]);
  // Bent and sagging, it stays as long.
  const bent = lashShape({ curl: 90, sag: 0.3, length: 1.2, count: 12 });
  let length = Math.hypot(...bent[0]);
  for (let index = 1; index < bent.length; index += 1) {
    length += Math.hypot(...bent[index].map((part, axis) => part - bent[index - 1][axis]));
  }
  assert.ok(Math.abs(length - 1.2) < 0.02, `length ${length}`);
});
