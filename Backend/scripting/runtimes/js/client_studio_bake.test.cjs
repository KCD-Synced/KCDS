'use strict';

// A game clip baked into a studio animation (Client/resources/studio/clip_bake.cjs):
// the body's joints from the game client's recording, as keys on top of the idle.
const test = require('node:test');
const assert = require('node:assert/strict');
const { bakeClip } = require('../../../../Client/resources/studio/clip_bake.cjs');

function about(axis, degrees) {
  const half = (degrees * Math.PI) / 360;
  const turn = [0, 0, 0, Math.cos(half)];
  turn[axis] = Math.sin(half);
  return turn;
}

function multiply(a, b) {
  const [ax, ay, az, aw] = a;
  const [bx, by, bz, bw] = b;
  return [
    aw * bx + ax * bw + ay * bz - az * by,
    aw * by - ax * bz + ay * bw + az * bx,
    aw * bz + ax * by - ay * bx + az * bw,
    aw * bw - ax * bx - ay * by - az * bz,
  ];
}

// A recording: the root walks round a quarter turn over the clip, the hips turn slowly and
// twist, the arm swings every 2 s, the left leg bends with it; the hips a little low.
const NAMES = ['Human_Male', 'Hips', 'Spine', 'RightArm', 'LeftUpLeg', 'RightUpLeg'];
const PARENTS = [-1, 0, 1, 2, 1, 1];

function recording(seconds = 6) {
  const frames = [];
  for (let frame = 0; frame <= seconds * 30; frame += 1) {
    const time = frame / 30;
    const swing = Math.sin((2 * Math.PI * time) / 2);
    const root = about(2, 15 * time);
    // In the body's space: each joint's turn is its parent's times its own.
    const hips = multiply(root, about(2, 20 * time + 10 * swing));
    const spine = multiply(hips, [0, 0, 0, 1]);
    const arm = multiply(spine, about(0, 60 * swing));
    const leftLeg = multiply(hips, about(0, 25 * swing));
    const rightLeg = multiply(hips, [0, 0, 0, 1]);
    frames.push(
      time,
      0.1 * time,
      0,
      0.9,
      ...root,
      ...hips,
      ...spine,
      ...arm,
      ...leftLeg,
      ...rightLeg,
    );
  }
  return {
    seconds,
    names: NAMES,
    parents: PARENTS,
    idle: NAMES.flatMap(() => [0, 0, 0, 1]),
    idleHips: [0, 0, 0.95],
    frames,
  };
}

test('a bake keeps one loop of the clip, legs included, the root and the slow turning out', () => {
  const baked = bakeClip(recording());
  assert.equal(baked.error, undefined);
  assert.equal(baked.loop, true);
  assert.equal(baked.blend, 'spline');
  // One swing round: 2 s (or 4), and at most 32 keys.
  const length = baked.keys.at(-1).time;
  assert.ok(Math.abs(length - 2) < 0.15 || Math.abs(length - 4) < 0.15, `loop ${length}`);
  assert.ok(baked.keys.length <= 32);
  const joints = new Set(baked.keys.flatMap((key) => Object.keys(key.pose)));
  // The root walks the body: not baked. The leg that bends is kept, the one that does not left out.
  assert.equal(joints.has('Human_Male'), false);
  assert.equal(joints.has('LeftUpLeg'), true);
  assert.equal(joints.has('RightUpLeg'), false);
  assert.equal(joints.has('RightArm'), true);
  // The hips' slow turning is gone: they face forward, the twist stays small.
  for (const key of baked.keys) {
    assert.ok(Math.abs(key.pose.Hips?.[2] ?? 0) < 15, `hips heading ${key.pose.Hips?.[2]}`);
  }
  // It goes round without a seam: the last key is the first one.
  const first = baked.keys[0];
  const last = baked.keys.at(-1);
  for (const name of joints) {
    first.pose[name].forEach((part, axis) => {
      assert.ok(Math.abs(part - last.pose[name][axis]) < 0.5, `${name} seam`);
    });
  }
  // The hips stay over the body's place; their height goes into each key's lift.
  assert.ok(baked.keys.every((key) => Math.abs(key.lift + 0.05) < 0.002));
});

test('a bake too short or without joints says why', () => {
  assert.match(bakeClip({ names: [], parents: [] }).error, /no joints/);
  const short = recording(0.5);
  assert.match(bakeClip(short).error, /too short/);
});
