'use strict';

// Player.setPoseAnimation and Player.poseAnimation (Asset/Animation Studio): the
// studio's keys of joint turns, carried to every client by the NetworkPlayer field poseAnimation
// (kcd2s/integration/pose_animation.hpp).
const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const lastAnimation = (sent) =>
  sent.filter((message) => message.op === 'playerPoseAnimation').pop();

const STRIKE = Object.freeze({
  keys: [
    { time: 0, pose: { RightArm: [40, 0, 0], RightForeArm: [0, 0, 0] } },
    { time: 0.4, pose: { RightForeArm: [0, 0, 70], RightArm: [130, 0, -40] } },
    { time: 1.2, pose: { RightArm: [40, 0, 0] } },
  ],
});

test('a studio animation goes to every client as the field text, joints in name order', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  assert.equal(player.poseAnimation, null);
  player.setPoseAnimation(STRIKE);
  assert.deepEqual(lastAnimation(sent), {
    op: 'playerPoseAnimation',
    id: 3,
    animation:
      'A1LS|0:RightArm=40,0,0;RightForeArm=0,0,0|0.4:RightArm=130,0,-40;RightForeArm=0,0,70' +
      '|1.2:RightArm=40,0,0',
  });
  assert.equal(player.poseAnimation.blend, 'spline');
  assert.equal(player.poseAnimation.loop, true);
  // The value returned is a copy.
  player.poseAnimation.keys[0].time = 5;
  assert.equal(player.poseAnimation.keys[0].time, 0);
  player.setPoseAnimation({ ...STRIKE, blend: 'linear', loop: false });
  assert.match(lastAnimation(sent).animation, /^A1NI\|/);
  player.setPoseAnimation(null);
  assert.equal(player.poseAnimation, null);
  assert.deepEqual(lastAnimation(sent), { op: 'playerPoseAnimation', id: 3, animation: '' });
});

test('keys that set the physical prop go after their joints', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  player.setPoseAnimation({
    keys: [
      { time: 0, pose: { RightArm: [40, 0, 0] }, prop: { pitch: 90, collide: 1 } },
      { time: 0.4, pose: { RightArm: [130, 0, -40] } },
      { time: 1.2, pose: {}, prop: { pitch: 20, hand: 'LeftHand', collide: 2, gravity: 4.5 } },
    ],
  });
  assert.equal(
    lastAnimation(sent).animation,
    'A1LS|0:RightArm=40,0,0@c1;p90|0.4:RightArm=130,0,-40|1.2:@c2;g4.5;h1;p20',
  );
  assert.deepEqual(player.poseAnimation.keys[2].prop, {
    pitch: 20,
    hand: 'LeftHand',
    collide: 2,
    gravity: 4.5,
  });
  for (const [prop, error] of [
    [{ wobble: 1 }, TypeError],
    [{ hand: 'Foot' }, TypeError],
    [{ pitch: 200 }, RangeError],
    [{ collide: 3 }, RangeError],
  ]) {
    assert.throws(() => player.setPoseAnimation({ keys: [{ time: 0, pose: {}, prop }] }), error);
  }
});

test('what does not fit throws and changes nothing', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  player.setPoseAnimation(STRIKE);
  const before = sent.length;
  for (const wrong of [
    undefined,
    'strike',
    {},
    { keys: [{ time: 0, pose: { 'Right Arm': [0, 0, 0] } }] },
    { keys: [{ time: 0, pose: { RightArm: [0, 0] } }] },
    { keys: [{ time: 0, pose: {} }], blend: 'bounce' },
  ]) {
    assert.throws(() => player.setPoseAnimation(wrong), TypeError);
  }
  for (const wrong of [
    { keys: [] },
    { keys: [{ time: 0.5, pose: {} }] },
    {
      keys: [
        { time: 0, pose: {} },
        { time: 0, pose: {} },
      ],
    },
    { keys: Array.from({ length: 33 }, (_, index) => ({ time: index / 10, pose: {} })) },
  ]) {
    assert.throws(() => player.setPoseAnimation(wrong), RangeError);
  }
  assert.equal(sent.length, before);
  assert.equal(player.poseAnimation.keys.length, 3);
});

test('a named studio animation plays its effect events on the body at their times, each loop', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const { Animations } = host.makeAPI();
  const bursts = () => sent.filter((message) => message.op === 'vfxBurstOn');
  Animations.define('whip_crack', {
    ...STRIKE,
    events: [
      { time: 0.4, kind: 'effect', name: 'WH_Particels.dust.sweep', joint: 'RightHand', scale: 2 },
      { time: 0.4, kind: 'sound', name: 'v_horse_whistle' },
    ],
  });
  assert.throws(
    () =>
      Animations.define('bad', { ...STRIKE, events: [{ time: 0, kind: 'effect', name: 'nope' }] }),
    TypeError,
  );
  const player = report(3);
  player.playAnimation('whip_crack');
  t.mock.timers.tick(300);
  assert.equal(bursts().length, 0);
  t.mock.timers.tick(200);
  assert.equal(bursts().length, 1);
  assert.equal(bursts()[0].effect, 'WH_Particels.dust.sweep');
  assert.equal(bursts()[0].entity, 3);
  assert.equal(bursts()[0].joint, 'RightHand');
  // The loop (1.2 s long) starts again at 1.2 s and plays it at 1.6 s.
  t.mock.timers.tick(800);
  t.mock.timers.tick(400);
  assert.equal(bursts().length, 2);
  // Stopped: no more.
  player.stopAnimation();
  t.mock.timers.tick(5000);
  assert.equal(bursts().length, 2);
});

test('a studio animation defined by name plays with playAnimation and stops like the game’s', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const { Animations } = host.makeAPI();
  const ends = [];
  const emit = host.emit.bind(host);
  host.emit = (name, ...args) => {
    if (name === 'playerAnimationEnd') {
      ends.push(`${args[0].id} ${args[1]} ${args[2]}`);
    }
    return emit(name, ...args);
  };
  Animations.define('whip_strike', STRIKE);
  assert.deepEqual(Animations.defined(), ['whip_strike']);
  assert.ok(Animations.list().length > 0, 'the game’s animations are still listed');
  const player = report(3);
  assert.equal(player.playAnimation('whip_strike', { loop: false }), true);
  assert.match(lastAnimation(sent).animation, /^A1NS\|0:RightArm=40/);
  assert.equal(player.poseAnimation.loop, false);
  // Again: the one before is replaced.
  assert.equal(player.playAnimation('whip_strike'), true);
  assert.match(lastAnimation(sent).animation, /^A1LS\|/);
  assert.equal(player.stopAnimation(), true);
  assert.deepEqual(lastAnimation(sent), { op: 'playerPoseAnimation', id: 3, animation: '' });
  assert.deepEqual(ends, ['3 whip_strike replaced', '3 whip_strike stopped']);
  // A pose animation set directly is not stopped by stopAnimation as a named one.
  player.setPoseAnimation(STRIKE);
  player.stopAnimation();
  assert.notEqual(player.poseAnimation, null);
  assert.throws(() => Animations.define('bad name', STRIKE), TypeError);
  assert.throws(() => Animations.define('empty', { keys: [] }), RangeError);
  assert.equal(Animations.undefine('whip_strike'), true);
  assert.deepEqual(Animations.defined(), []);
});

test('a key lifts the hips after its joints (a baked game clip)', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  player.setPoseAnimation({
    keys: [
      { time: 0, pose: { LeftLeg: [30, 0, 0] }, lift: -0.06 },
      { time: 0.4, pose: { LeftLeg: [10, 0, 0] }, lift: 0 },
    ],
  });
  assert.equal(lastAnimation(sent).animation, 'A1LS|0:LeftLeg=30,0,0#-0.06|0.4:LeftLeg=10,0,0#0');
  assert.equal(player.poseAnimation.keys[0].lift, -0.06);
  assert.throws(
    () => player.setPoseAnimation({ keys: [{ time: 0, pose: {}, lift: -2 }] }),
    RangeError,
  );
  // A key's lash shape (Lash to keys): after the lift, before the prop.
  const first = [
    [0.1, 0, 0],
    [0.2, 0, -0.05],
  ];
  const second = [
    [0.1, 0, 0.02],
    [0.2, 0, 0.05],
  ];
  player.setPoseAnimation({
    keys: [
      { time: 0, pose: {}, lift: -0.05, lash: first, prop: { pitch: 90 } },
      { time: 0.4, pose: {}, lash: second },
    ],
  });
  assert.equal(
    lastAnimation(sent).animation,
    'A1LS|0:#-0.05~0.1,0,0;0.2,0,-0.05@p90|0.4:~0.1,0,0.02;0.2,0,0.05',
  );
  assert.throws(
    () => player.setPoseAnimation({ keys: [{ time: 0, pose: {}, lash: [[9, 0, 0]] }] }),
    RangeError,
  );
  assert.throws(
    () => player.setPoseAnimation({ keys: [{ time: 0, pose: {}, lash: [] }] }),
    TypeError,
  );
  // A lash track (Lash from motion): after the keys, its frames joined by '/'.
  const lashTrack = {
    rate: 40,
    length: 1.6,
    frames: [
      [0, -5, 3, -8],
      [2, -6, 5, -9],
    ],
  };
  player.setPoseAnimation({
    keys: [
      { time: 0, pose: {} },
      { time: 0.4, pose: {} },
    ],
    lashTrack,
  });
  assert.equal(lastAnimation(sent).animation, 'A1LS|0:|0.4:^40,1.6:0,-5,3,-8/2,-6,5,-9');
  assert.deepEqual(player.poseAnimation.lashTrack, lashTrack);
  // Kept in the body's frame: ",b" after the length.
  player.setPoseAnimation({
    keys: [
      { time: 0, pose: {} },
      { time: 0.4, pose: {} },
    ],
    lashTrack: { ...lashTrack, space: 'body' },
  });
  assert.equal(lastAnimation(sent).animation, 'A1LS|0:|0.4:^40,1.6,b:0,-5,3,-8/2,-6,5,-9');
  const bad = {
    ...lashTrack,
    frames: [
      [0, 95, 0, 0],
      [0, 0, 0, 0],
    ],
  };
  assert.throws(
    () => player.setPoseAnimation({ keys: [{ time: 0, pose: {} }], lashTrack: bad }),
    RangeError,
  );
});

// An animation longer than the field takes: 32 keys of 24 joints, a baked game clip's size.
function longAnimation() {
  const joints = Array.from({ length: 24 }, (unused, index) => `Joint${index}`);
  return {
    keys: Array.from({ length: 32 }, (unused, key) => ({
      time: key / 10,
      pose: Object.fromEntries(joints.map((name, index) => [name, [key + 0.5, index, -12.5]])),
      lift: -0.05,
    })),
  };
}

test('a defined animation longer than the field goes to every client by its name', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const { Animations } = host.makeAPI();
  const events = () =>
    sent.filter((message) => message.op === 'event' && message.name === 'kcds:poseAnimation');
  host.clientResourcesStarted(3);
  Animations.define('whipping', longAnimation());
  // Every client whose resources run gets it once, as the field's text.
  const given = events().at(-1);
  assert.equal(given.player, 3);
  const { name, field } = JSON.parse(given.payload);
  assert.equal(name, 'whipping');
  assert.match(field, /^A1LS\|0:Joint0=0\.5,0,-12\.5;/);
  assert.ok(field.length > 2000);
  // It plays by its name; lockMovement holds the player where they stand while it plays.
  const player = report(3);
  assert.equal(player.playAnimation('whipping', { lockMovement: true }), true);
  assert.deepEqual(lastAnimation(sent), {
    op: 'playerPoseAnimation',
    id: 3,
    animation: 'R1Lwhipping',
  });
  assert.equal(player.frozen, true);
  assert.equal(player.playAnimation('whipping', { loop: false }), true);
  assert.equal(lastAnimation(sent).animation, 'R1Nwhipping');
  assert.equal(player.frozen, false);
  assert.equal(player.stopAnimation(), true);
  assert.deepEqual(lastAnimation(sent), { op: 'playerPoseAnimation', id: 3, animation: '' });
  // A client whose resources start later gets it then; undefined, every client forgets it.
  host.clientResourcesStarted(5);
  assert.equal(events().at(-1).player, 5);
  Animations.undefine('whipping');
  assert.deepEqual(JSON.parse(events().at(-1).payload), { name: 'whipping', field: '' });
  assert.throws(
    () =>
      Animations.define('too_long', { keys: [...longAnimation().keys, ...longAnimation().keys] }),
    RangeError,
  );
});
