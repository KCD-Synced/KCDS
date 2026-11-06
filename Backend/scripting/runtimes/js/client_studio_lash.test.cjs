'use strict';

// A whip's lash as frames (Client/resources/studio/lash_keys.cjs): the lash the
// physics moved, recorded with the handle's frame, kept as each key's shape.
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  lashShapeOf,
  lashKeys,
  LASH_POINTS,
} = require('../../../../Client/resources/studio/lash_keys.cjs');
const { Animation } = require('../../../../Client/resources/studio/animation.cjs');
const { StudioEditor } = require('../../../../Client/resources/studio/editor.cjs');
const timeline = require('../../../../Client/resources/studio/timeline.cjs');

// The handle along +y with its up +z (its right +x), its tip at (1, 2, 1.5).
const HANDLE = [0, 1, 0, 0, 0, 1];

// A lash of 21 points from the tip: straight out along the handle for its first half, then
// hanging straight down.
function lash() {
  const points = [];
  for (let index = 0; index <= 20; index += 1) {
    const out = Math.min(index, 10) * 0.08;
    const down = Math.max(0, index - 10) * 0.08;
    points.push(1, 2 + out, 1.5 - down);
  }
  return points;
}

test('a lash is kept as 12 points along it, in the handle’s frame', () => {
  const shape = lashShapeOf(lash(), HANDLE);
  assert.equal(shape.length, LASH_POINTS);
  // Evenly along its 1.6 m: the sixth point at 0.8 m is where it bends; the last at its end.
  assert.deepEqual(shape[5], [0.8, 0, 0]);
  assert.deepEqual(shape[11], [0.8, 0, -0.8]);
  // A turned handle turns the frame: the same lash seen from a handle along +x is to its left.
  const turned = lashShapeOf(lash(), [1, 0, 0, 0, 0, 1]);
  assert.deepEqual(turned[5], [0, -0.8, 0]);
  assert.equal(lashShapeOf(lash(), [0, 0, 1, 0, 0, 1]), null);
});

test('each key keeps the lash of its time; an animation blends them and keeps them', () => {
  const frames = [lash(), lash(), lash()];
  const shapes = lashKeys({ rate: 10, frames, handles: frames.map(() => HANDLE) }, [0, 0.1, 0.6]);
  assert.equal(shapes.length, 3);
  assert.deepEqual(shapes[2], shapes[0]);
  assert.deepEqual(lashKeys({ rate: 10, frames }, [0]), [null]);
  const animation = new Animation();
  assert.equal(
    animation.restore({
      keys: [
        { time: 0, pose: {}, lash: [[0.1, 0, 0]] },
        { time: 1, pose: {}, lash: [[0.3, 0, -0.2]] },
      ],
    }),
    true,
  );
  assert.equal(animation.hasLash(), true);
  // Halfway the piece's way is halfway round (blended, not its end point) and its length halfway.
  const half = animation.lashAt(0.5)[0];
  const length = Math.hypot(...half);
  const wanted = (0.1 + Math.hypot(0.3, 0.2)) / 2;
  assert.ok(Math.abs(length - wanted) < 1e-3, `${length}`);
  assert.ok(half[2] < 0 && half[0] > 0);
  assert.deepEqual(animation.toJSON().keys[1].lash, [[0.3, 0, -0.2]]);
  // A bad lash is left out.
  animation.restore({ keys: [{ time: 0, pose: {}, lash: [[9, 0, 0]] }] });
  assert.equal(animation.hasLash(), false);
});

// A lash bent one way, then the other: its two pieces each 0.8 m.
function bent(side) {
  return [
    [0.8, 0, 0],
    [0.8, 0.8 * side, 0],
  ];
}

test('between two keys a bent lash keeps its length (blending the points shrank it)', () => {
  const animation = new Animation();
  animation.restore({
    keys: [
      { time: 0, pose: {}, lash: bent(1) },
      { time: 1, pose: {}, lash: bent(-1) },
    ],
  });
  const [first, second] = animation.lashAt(0.5);
  const piece = second.map((part, axis) => part - first[axis]);
  const along = Math.hypot(...first) + Math.hypot(...piece);
  assert.ok(Math.abs(along - 1.6) < 1e-3, `${along}`);
});

function whipEditor(isDown = () => false) {
  const sent = [];
  const editor = new StudioEditor({
    NativeUI: {},
    NoClip: {},
    Key: { isDown },
    send: (message) => sent.push(message),
    log: () => {},
    now: () => 0,
  });
  editor.rope = { kind: 'whip', length: 1.6, physics: {}, hold: { joint: 'RightHand' }, grip: {} };
  editor.animation.restore({
    keys: [
      { time: 0, pose: {} },
      { time: 1.6, pose: {} },
    ],
    loop: true,
  });
  return { editor, sent };
}

test('the lash posed by hand: keys at their own times, shaped, played exactly so', async () => {
  const { editor, sent } = whipEditor();
  await editor.script('lashpose {"time":0.4,"add":true}');
  await editor.script('lashpose {"time":0.4,"shape":{"aimUp":30}}');
  const answer = JSON.parse(
    (await editor.script('lashpose {"time":1,"shape":{"aimSide":40,"curl":-20}}'))
      .split(' -> ')[1]
      .split(' {"open"')[0],
  );
  assert.equal(answer.count, 2);
  assert.deepEqual(answer.keys[1].first[0], 40);
  // The lash track is baked of the keys, in the body's frame: a key's shape at its time.
  const track = editor.animation.lashTrack;
  assert.equal(track.space, 'body');
  assert.equal(track.frames.length, 64);
  const atKey = editor.animation.lashAt(1)[0];
  assert.ok(Math.abs(Math.atan2(atKey[1], atKey[0]) * (180 / Math.PI) - 40) < 2);
  // The studio shows it in the body's frame; it is kept, and undo takes a shaping back.
  assert.equal(sent.findLast((one) => one.op === 'studioLashShape').space, 'body');
  assert.equal(editor.animationByName().lashKeys.keys.length, 2);
  editor.undo();
  assert.ok(Math.abs(editor.animation.lashKeys.keys[1].ways[0][0]) < 1);
  // The Details rows and the timeline's Lash row.
  const labels = editor.lashKeyRows().map((row) => row.label);
  assert.ok(labels.includes('Curl') && labels.includes('Lash key'));
  assert.deepEqual(editor.timelineView().lashKeys, [0.4, 1]);
  const lashRow = 70 + 24 + 12;
  const x = 432 + 1 * ((1896 - 432) / editor.timelineView().span);
  const pressed = timeline.hit(editor.timelineView(), x, lashRow);
  assert.deepEqual(pressed, { kind: 'lashKey', index: 1, time: 1 });
  await editor.script('lashpose {"clear":true}');
  assert.equal(editor.animation.lashKeys, null);
});

test('a held key follows until the button is up; a release while held does not drop it', () => {
  let down = true;
  const { editor } = whipEditor(() => down);
  editor.animation.restore({
    keys: [
      { time: 0, pose: {} },
      { time: 0.5, pose: {} },
      { time: 1, pose: {} },
    ],
  });
  editor.drag = { key: 1, time: 0.7, startedAt: -1000 };
  editor.releaseTimeline(800);
  assert.ok(editor.drag, 'the game’s release while the button is held is not the end');
  down = false;
  editor.endKeyDrag(null);
  assert.equal(editor.drag, null);
  assert.equal(editor.animation.keys[1].time, 0.7);
});

test('a lash track plays at its own times, its whole length, round the loop', () => {
  // Two segments: straight out, then the second turned 90° to the right.
  const lashTrack = {
    rate: 10,
    length: 1,
    frames: [
      [0, 0, 0, 0],
      [0, 0, 90, 0],
    ],
  };
  const animation = new Animation();
  const keys = [
    { time: 0, pose: {}, lash: [[0.5, 0, 0]] },
    { time: 0.2, pose: {}, lash: [[0.5, 0, 0]] },
  ];
  assert.equal(animation.restore({ keys, lashTrack }), true);
  assert.deepEqual(animation.lashAt(0), [
    [0.5, 0, 0],
    [1, 0, 0],
  ]);
  assert.deepEqual(animation.lashAt(0.1), [
    [0.5, 0, 0],
    [0.5, 0.5, 0],
  ]);
  // Halfway between the frames the second piece is half turned, still 0.5 m.
  const [, end] = animation.lashAt(0.05);
  assert.ok(Math.abs(Math.hypot(end[0] - 0.5, end[1]) - 0.5) < 1e-3);
  // Looping, 0.2 s is the first frame again; the track is kept with the animation.
  assert.deepEqual(animation.lashAt(0.2), animation.lashAt(0));
  assert.deepEqual(animation.toJSON().lashTrack, lashTrack);
  // A bad track is left out.
  const bad = {
    rate: 10,
    length: 1,
    frames: [
      [0, 95, 0, 0],
      [0, 0, 0, 0],
    ],
  };
  animation.restore({ keys, lashTrack: bad });
  assert.equal(animation.lashTrack, null);
});
