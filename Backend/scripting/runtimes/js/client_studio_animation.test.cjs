'use strict';

// The Asset/Animation Studio's animation and timeline (Client/resources/studio/animation.cjs,
// timeline.cjs): keys, blends, the turns at any time, the timeline's presses and the
// whole animation an assistant sets by joint names.
const test = require('node:test');
const assert = require('node:assert/strict');
const { Animation, quaternion } = require('../../../../Client/resources/studio/animation.cjs');
const timeline = require('../../../../Client/resources/studio/timeline.cjs');
const { StudioEditor } = require('../../../../Client/resources/studio/editor.cjs');

function near(actual, expected, tolerance = 1e-6) {
  assert.ok(Math.abs(actual - expected) <= tolerance, `${actual} is not ${expected}`);
}

function swing() {
  const animation = new Animation();
  animation.restore({
    keys: [
      { time: 0, pose: { 5: [0, 0, 0] } },
      { time: 0.5, pose: { 5: [90, 0, 0] } },
      { time: 1, pose: { 5: [0, 0, 0] } },
    ],
    loop: false,
  });
  return animation;
}

test('a turn is a quaternion about the parent’s axes, x then y then z', () => {
  const turn = quaternion([90, 0, 0]);
  near(turn[0], Math.SQRT1_2);
  near(turn[3], Math.SQRT1_2);
  const both = quaternion([0, 0, 180]);
  near(both[2], 1);
  near(both[3], 0);
});

test('the spline goes through every key and flows through them', () => {
  const animation = swing();
  near(animation.turnsAt(0.5)[5][0], 90);
  near(animation.turnsAt(0)[5][0], 0);
  // A peak is still at its top; the two sides mirror each other.
  near(animation.turnsAt(0.25)[5][0], animation.turnsAt(0.75)[5][0]);
  animation.blend = 'linear';
  near(animation.turnsAt(0.25)[5][0], 45);

  // A ramp 0, 45, 90: the spline keeps moving through the middle key, the smooth blend stops.
  const ramp = new Animation();
  ramp.restore({
    keys: [
      { time: 0, pose: { 5: [0, 0, 0] } },
      { time: 0.5, pose: { 5: [45, 0, 0] } },
      { time: 1, pose: { 5: [90, 0, 0] } },
    ],
    loop: false,
  });
  const through = (blend) => {
    ramp.blend = blend;
    return ramp.turnsAt(0.55)[5][0] - ramp.turnsAt(0.45)[5][0];
  };
  assert.ok(through('spline') > through('smooth') * 2, `${through('spline')}`);
  near(through('linear'), 9);
});

test('a key added at a time takes the turns there; a key moves between its neighbours', () => {
  const animation = swing();
  animation.blend = 'linear';
  assert.equal(animation.addKeyAt(0.25), true);
  assert.deepEqual(
    animation.keys.map((one) => one.time),
    [0, 0.25, 0.5, 1],
  );
  assert.deepEqual(animation.keys[1].pose, { 5: [45, 0, 0] });
  assert.equal(animation.key, 1);
  assert.equal(animation.addKeyAt(0.25), false, 'a key is there already');
  assert.equal(animation.moveKeyTo(1, 0.9), true);
  assert.equal(animation.keys[1].time, 0.45, 'stops before the next key');
  assert.equal(animation.moveKeyTo(0, 0.2), false, 'the first key stays at 0');
});

test('the timeline: transport presses, a key’s diamond, the ruler and a track', () => {
  const view = {
    time: 0,
    span: timeline.spanOf(1),
    length: 1,
    playing: false,
    loop: true,
    blend: 'spline',
    key: 0,
    keys: [0, 0.5, 1],
    tracks: [{ label: 'RightArm', joint: 7, chosen: true, keys: [1] }],
    drag: null,
  };
  assert.equal(view.span, 2);
  assert.deepEqual(timeline.hit(view, 100, 90), { kind: 'play' });
  assert.deepEqual(timeline.hit(view, 30, 90), { kind: 'start' });
  assert.deepEqual(timeline.hit(view, 30, 130), { kind: 'addKey' });
  assert.deepEqual(timeline.hit(view, 150, 130), { kind: 'removeKey' });
  assert.deepEqual(timeline.hit(view, 100, 170), { kind: 'loop' });
  assert.deepEqual(timeline.hit(view, 100, 210), { kind: 'blend', direction: -1 });
  const half = 432 + 0.5 * ((1896 - 432) / 2);
  assert.deepEqual(timeline.hit(view, half + 3, 70 + 24 + 12), { kind: 'key', key: 1, joint: 7 });
  assert.deepEqual(timeline.hit(view, half + 3, 50), { kind: 'scrub', time: 0.5, joint: null });
  // On the RightArm track there is no key at 1 s: a press there moves the playhead.
  const end = 432 + 1 * ((1896 - 432) / 2);
  assert.deepEqual(timeline.hit(view, end, 70 + 24 + 12), { kind: 'scrub', time: 1, joint: 7 });
  assert.deepEqual(timeline.hit(view, 300, 70 + 24 + 12), { kind: 'joint', joint: 7 });
  assert.equal(timeline.timeAt(view, 432), 0);
});

test('the assistant sets a whole animation by joint names; unknown names are reported', async () => {
  const sent = [];
  const editor = new StudioEditor({
    NativeUI: {},
    NoClip: {},
    Key: { isDown: () => false },
    send: (message) => sent.push(message),
    log: () => {},
    now: () => 0,
  });
  editor.jointsKept(['root', 'Hips', 'RightArm', 'RightForeArm'], [-1, 0, 1, 2]);
  const animation = {
    keys: [
      { time: 0, pose: { RightArm: [0, 0, 0], rightforearm: [0, 0, 0] } },
      { time: 0.4, pose: { RightArm: [0, -60, 30], RightForeArm: [0, 0, 70], Tail: [1, 2, 3] } },
      { time: 0.8, pose: { RightArm: [0, 0, 0] } },
    ],
    blend: 'spline',
  };
  // The assistant's step (Dev/tools/architect-mcp studio_set_animation), answered as text.
  const answer = await editor.script(`animation ${JSON.stringify(animation)}`);
  assert.match(
    answer,
    /-> \{"keys":3,"length":0.8,"blend":"spline","loop":true,"playing":true,"unknownJoints":\["Tail"\],"events":0,"eventsLeftOut":0\}/,
  );
  assert.deepEqual(editor.animation.keys[1].pose, { 2: [0, -60, 30], 3: [0, 0, 70] });
  assert.deepEqual(editor.animationByName().keys[1].pose, {
    RightArm: [0, -60, 30],
    RightForeArm: [0, 0, 70],
  });
  assert.match(
    await editor.script('find forearm'),
    /-> \[\{"id":3,"name":"RightForeArm","parent":"RightArm"\}\]/,
  );
  const bad = editor.setAnimation(
    '{"keys":[{"time":0,"pose":{}},{"time":0.5,"pose":{}},{"time":0.5,"pose":{}}]}',
  );
  assert.match(bad.error, /each key later/);
  editor.undo();
  assert.equal(editor.animation.keys.length, 1, 'one undo takes the whole animation back');
});

test('the editor hides all of itself for a clean view and comes back as it was', async () => {
  const sent = [];
  const editor = new StudioEditor({
    NativeUI: {},
    NoClip: {},
    Key: { isDown: () => false },
    send: (message) => sent.push(message),
    log: () => {},
    now: () => 0,
  });
  editor.mode = 'bones';
  assert.match(await editor.script('ui hide'), /"hidden":true/);
  assert.deepEqual(
    sent.findLast((one) => one.op === 'studioGrid'),
    { op: 'studioGrid', on: false },
  );
  assert.equal(sent.findLast((one) => one.op === 'studioBones').on, false);
  // Nothing turns the bones on while it is hidden.
  editor.sendBones(true);
  assert.equal(sent.findLast((one) => one.op === 'studioBones').on, false);
  assert.match(await editor.script('ui show'), /"hidden":false/);
  assert.deepEqual(
    sent.findLast((one) => one.op === 'studioGrid'),
    { op: 'studioGrid', on: true },
  );
  assert.equal(sent.findLast((one) => one.op === 'studioBones').on, true);
});

test('the assistant changes some keys, a track and the timing; long steps come in parts', async () => {
  const sent = [];
  const editor = new StudioEditor({
    NativeUI: {},
    NoClip: {},
    Key: { isDown: () => false },
    send: (message) => sent.push(message),
    log: () => {},
    now: () => 0,
  });
  editor.jointsKept(['root', 'Hips', 'LeftFoot', 'RightFoot'], [-1, 0, 1, 1]);
  const animation = {
    keys: [
      { time: 0, pose: { LeftFoot: [100, 0, 0], RightFoot: [-150, 5, 5] }, lift: -0.05 },
      { time: 0.5, pose: { LeftFoot: [120, 0, 0], RightFoot: [-140, 5, 5] } },
      { time: 1, pose: { LeftFoot: [100, 0, 0], RightFoot: [-150, 5, 5] } },
    ],
  };
  // A step longer than the bridge takes comes in parts; the last runs it whole.
  const whole = `animation ${JSON.stringify(animation)}`;
  const half = Math.ceil(whole.length / 2);
  assert.match(await editor.script(`part 0 2 ${whole.slice(0, half)}`), /"part":0,"of":2/);
  assert.match(await editor.script(`part 1 2 ${whole.slice(half)}`), /"keys":3/);
  // One joint in one key: the rest stays, lift too.
  const patched = await editor.script('patch {"times":[0.5],"add":{"LeftFoot":[-20,0,0]}}');
  assert.match(patched, /"patched":1/);
  assert.deepEqual(editor.animationByName().keys[1].pose.LeftFoot, [100, 0, 0]);
  assert.equal(editor.animationByName().keys[0].lift, -0.05);
  // The left foot as the right one's mirror; the check finds no problem.
  await editor.script('track {"op":"mirror","joint":"LeftFoot"}');
  assert.deepEqual(editor.animationByName().keys[1].pose.LeftFoot, [-140, -5, -5]);
  assert.match(await editor.script('check'), /"problems":\[\]/);
  await editor.script('retime {"op":"speed","factor":2}');
  assert.equal(editor.animation.length(), 0.5);
  // History says which changes the assistant made.
  assert.match(editor.history[0], /^\d+\. {2}MCP · Retimed \(speed\)$/);
  assert.match(editor.history[1], /MCP · Track mirror/);
  assert.match(await editor.script('history'), /"undo":\["Retimed \(speed\)","Track mirror"/);
  // Each change one step to undo.
  editor.undo();
  assert.equal(editor.animation.length(), 1);
  // A change by hand is not marked.
  editor.changeAnimation('Key added', () => true);
  assert.doesNotMatch(editor.history[0], /MCP/);
});
