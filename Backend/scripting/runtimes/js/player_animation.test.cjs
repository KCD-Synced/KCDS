'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { rotation } = require('./math.cjs');

// Player.playAnimation, Player.stopAnimation and playerAnimationEnd on the server.

function hostWith(code = '') {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-animation-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'gamemode');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'gamemode', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    `kcds.core.Events.on('playerAnimationEnd', (player, fragment, reason) =>
       kcds.core.Chat.sendToAll('end ' + player.id + ' ' + fragment + ' ' + reason));
     ${code}`,
  );
  return { host, output };
}

function player(host, id, ready = true) {
  return host.updatePlayer({
    id,
    nickname: 'P' + id,
    ready,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

const orders = (output) => output.filter((e) => e.op === 'animation').map((e) => ({ ...e }));
const lastOrder = (output) => orders(output).at(-1);
const ends = (output) =>
  output
    .filter((e) => e.op === 'chat' && /^end /.test(e.text))
    .map((e) => e.text.slice('end '.length));
const report = (host, id, request, reason, exit = '') =>
  host.playerAnimations.report(host.players.get(id), { id, request, reason, exit });

test('playAnimation sends the order to its owner only and returns true', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  player(host, 2);
  assert.equal(owner.playAnimation('Wave'), true);
  assert.deepEqual(orders(output), [
    {
      op: 'animation',
      id: 1,
      kind: 'play',
      request: 1,
      restore: false,
      fragment: 'Wave',
      clip: '',
      tags: '',
      loop: false,
      lockMovement: false,
      upperBody: false,
      noGravity: false,
      lockRotation: false,
      lookYaw: 0,
      lookUp: 0,
      lookDown: 0,
      speed: 1,
      time: 0,
      props: [],
    },
  ]);
  owner.playAnimation('Bow', { loop: true, lockMovement: true, tags: 'sittingNoTable' });
  assert.deepEqual(
    (({ request, fragment, tags, loop, lockMovement }) => ({
      request,
      fragment,
      tags,
      loop,
      lockMovement,
    }))(lastOrder(output)),
    { request: 2, fragment: 'Bow', tags: 'sittingNoTable', loop: true, lockMovement: true },
  );
  assert.deepEqual([...new Set(orders(output).map((e) => e.id))], [1]);
});

test("a .caf the calling resource ships is ordered as its own clip; any other stays the game's", async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-animation-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'dance');
  fs.mkdirSync(path.join(dir, 'clips'), { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'dance', kcds: { serverScripts: ['index.js'], files: ['clips/**'] } }),
  );
  fs.writeFileSync(path.join(dir, 'clips', 'wave.caf'), 'caf');
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    `kcds.core.Events.on('playerCommand', (player, command, args) => {
       if (command === 'clip') player.playAnimation('', { clip: args[0] });
     });`,
  );
  await host.startAll();
  const owner = player(host, 1);
  host.receiveChat(1, '/clip clips/wave.caf');
  assert.equal(lastOrder(output).clip, '@dance/clips/wave.caf');
  // A file the resource does not ship, and a path of the game's own, stay as given.
  host.receiveChat(1, '/clip clips/other.caf');
  assert.equal(lastOrder(output).clip, 'clips/other.caf');
  host.receiveChat(1, '/clip animations/human/male/wave.caf');
  assert.equal(lastOrder(output).clip, 'animations/human/male/wave.caf');
  // Outside a resource's call there is no resource to ship it.
  owner.playAnimation('', { clip: 'clips/wave.caf' });
  assert.equal(lastOrder(output).clip, 'clips/wave.caf');
  // A script cannot name a resource's clip itself.
  assert.throws(() => owner.playAnimation('', { clip: '@dance/clips/wave.caf' }), RangeError);
});

test("upperBody plays the fragment's clip on the upper body only", async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  assert.equal(owner.playAnimation('CampWineDrink', { upperBody: true, loop: true }), true);
  const order = lastOrder(output);
  // The clip of the catalog's row plays on a layer of its own; the fragment is left out.
  assert.equal(order.upperBody, true);
  assert.equal(order.clip, 'camp_wine_drink_stand_drink');
  assert.equal(order.fragment, '');
  assert.equal(order.loop, true);
  // The tags pick the row they name.
  owner.playAnimation('BehaviorDrinkBarrelSmallSpigot', { upperBody: true, tags: 'stealth' });
  assert.equal(lastOrder(output).clip, 'behavior_drink_barrel_small_spigot_player');
  // A clip given plays as it is.
  owner.playAnimation('', { clip: 'camp_drink_spruit', upperBody: true });
  assert.equal(lastOrder(output).clip, 'camp_drink_spruit');
  // It keeps walking: no lockMovement; a fragment with no clip cannot play on the upper body.
  assert.throws(
    () => owner.playAnimation('CampWineDrink', { upperBody: true, lockMovement: true }),
    TypeError,
  );
  assert.throws(() => owner.playAnimation('Dead', { upperBody: true }), RangeError);
  assert.throws(() => owner.playAnimation('CampWineDrink', { upperBody: 'yes' }), TypeError);
});

test('gravity: false keeps the body where the script puts it', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  owner.playAnimation('DeadBody_Hanged', { tags: 'slaveHalter+hanged_01', gravity: false });
  assert.equal(lastOrder(output).noGravity, true);
  owner.playAnimation('DeadBody_Hanged', { tags: 'slaveHalter+hanged_01', gravity: true });
  assert.equal(lastOrder(output).noGravity, false);
  assert.throws(() => owner.playAnimation('Wave', { gravity: 0 }), TypeError);
});

test('playPairedAnimation puts the slave at the master and starts both', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  host.playerAnimations.pairDelayMs = 0;
  const master = player(host, 1);
  const slave = player(host, 2);
  assert.equal(
    master.playPairedAnimation(slave, 'Quest_StandHlbrdKill', { tags: '', slaveTags: '' }),
    true,
  );
  await new Promise((resolve) => setTimeout(resolve, 5));
  const plays = orders(output).filter((e) => e.kind === 'play');
  assert.deepEqual(
    plays.map((e) => [e.id, e.fragment, e.partner, e.pairedSlave === true, e.lockMovement]),
    [
      [1, 'Quest_StandHlbrdKill', 2, false, true],
      [2, 'Quest_StandHlbrdKillSlave', 1, true, true],
    ],
  );
  assert.equal(master.playPairedAnimation(master, 'Quest_StandHlbrdKill'), false);
  assert.throws(() => master.playPairedAnimation(slave, 'Quest_StandHlbrdKill', { loop: true }));
  // A fragment without a Slave in the catalog is refused.
  assert.equal(master.playPairedAnimation(slave, 'PrayStand'), false);
});

test('a lit prop burns with a flame and a light until the request goes', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  owner.playAnimation('PrayStand', {
    loop: true,
    props: { model: 'torch_player', hand: 'left', attach: 'game', lit: true },
  });
  // The order itself carries no lit (the bridge sends JSON): the server burns it.
  assert.equal(JSON.stringify(lastOrder(output).props[0]).includes('lit'), false);
  const flame = output.filter((e) => e.op === 'vfxSpawn').at(-1);
  assert.equal(flame.body, 1);
  assert.equal(flame.joint, 'LeftHandSlot');
  assert.equal(flame.effect, 'cinematics.fires.fire_torch_refr');
  assert.ok(Math.abs(flame.position.z - 0.45) < 1e-9);
  const light = output.filter((e) => e.op === 'lightSpawn').at(-1);
  assert.equal(light.body, 1);
  assert.equal(light.joint, 'LeftHandSlot');
  owner.stopAnimation();
  assert.deepEqual(
    output.filter((e) => e.op === 'vfxDestroy' || e.op === 'lightDestroy').map((e) => e.id),
    [flame.id, light.id],
  );
  assert.throws(
    () => owner.playAnimation('PrayStand', { props: { model: 'torch_player', lit: 'yes' } }),
    TypeError,
  );
});

test('lockRotation keeps the body turned while the head and camera look around', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  owner.playAnimation('PrayStand', { loop: true, lockRotation: true });
  assert.equal(lastOrder(output).lockRotation, true);
  assert.throws(() => owner.playAnimation('PrayStand', { lockRotation: 'yes' }), TypeError);
});

test('lockRotation limits how far the player looks around', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  owner.playAnimation('PrayStand', { loop: true, lockRotation: { yaw: 40, up: 10, down: 55 } });
  const order = lastOrder(output);
  assert.deepEqual(
    [order.lockRotation, order.lookYaw, order.lookUp, order.lookDown],
    [true, 40, 10, 55],
  );
  owner.playAnimation('PrayStand', { lockRotation: { yaw: 30 } });
  assert.deepEqual(
    [lastOrder(output).lookYaw, lastOrder(output).lookUp, lastOrder(output).lookDown],
    [30, 70, 70],
  );
  assert.throws(() => owner.playAnimation('PrayStand', { lockRotation: { yaw: 0 } }), RangeError);
  assert.throws(() => owner.playAnimation('PrayStand', { lockRotation: { up: 90 } }), RangeError);
  assert.throws(() => owner.playAnimation('PrayStand', { lockRotation: { left: 9 } }), TypeError);
});

test('a running animation changes speed, pauses and changes props', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const owner = player(host, 1);
  assert.equal(owner.animationSpeed, null);
  assert.equal(owner.pauseAnimation(), false);
  assert.equal(owner.setAnimationSpeed(2), false);
  assert.equal(owner.playAnimation('Wave', { speed: 0.5, time: 1.5, loop: true }), true);
  const request = lastOrder(output).request;
  assert.equal(lastOrder(output).speed, 0.5);
  assert.equal(lastOrder(output).time, 1.5);
  assert.equal(owner.animationSpeed, 0.5);

  assert.equal(owner.pauseAnimation(), true);
  assert.deepEqual(lastOrder(output), { op: 'animation', id: 1, kind: 'speed', request, speed: 0 });
  assert.equal(owner.animationSpeed, 0);
  // A new speed while paused waits for resumeAnimation.
  assert.equal(owner.setAnimationSpeed(2), true);
  assert.equal(lastOrder(output).speed, 0);
  assert.equal(owner.resumeAnimation(), true);
  assert.equal(lastOrder(output).speed, 2);
  assert.equal(owner.animationSpeed, 2);
  assert.throws(() => owner.setAnimationSpeed(11), RangeError);
  assert.throws(() => owner.setAnimationSpeed(), TypeError);
  assert.throws(() => owner.setAnimationSpeed('fast'), TypeError);

  // The props change without playing it again.
  assert.equal(owner.setAnimationProps([{ model: 'bucket_a_a', position: { x: 0.1 } }]), true);
  const props = lastOrder(output);
  assert.equal(props.kind, 'props');
  assert.equal(props.request, request);
  assert.equal(props.props.length, 1);
  assert.throws(() => owner.setAnimationProps([{ model: 'not_a_prop' }]), RangeError);

  // Sent again with the world: the speed and the props it has now.
  owner.pauseAnimation();
  host.playerAnimations.arrived(owner);
  const again = lastOrder(output);
  assert.equal(again.kind, 'play');
  assert.equal(again.request, request);
  assert.equal(again.speed, 0);
  assert.equal(again.props.length, 1);

  owner.stopAnimation();
  assert.equal(owner.animationSpeed, null);
  assert.equal(owner.resumeAnimation(), false);
  assert.equal(owner.setAnimationProps([]), false);
});

test('playAnimation and stopAnimation return false for a player without a body', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const loading = player(host, 1, false);
  assert.equal(loading.playAnimation('Wave'), false);
  assert.equal(loading.stopAnimation(), false);
  const gone = player(host, 2);
  host.disconnect(2);
  assert.equal(gone.playAnimation('Wave'), false);
  assert.equal(gone.stopAnimation(), false);
  assert.deepEqual(orders(output), []);
});

test('playAnimation and stopAnimation throw for arguments the game cannot use', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  assert.throws(() => p.playAnimation(), TypeError);
  assert.throws(() => p.playAnimation(7), TypeError);
  assert.throws(() => p.playAnimation('Wave Hello'), RangeError);
  assert.throws(() => p.playAnimation('x'.repeat(65)), RangeError);
  assert.throws(() => p.playAnimation(''), TypeError, 'nothing to play');
  assert.throws(() => p.playAnimation('Wave', null), TypeError);
  assert.throws(() => p.playAnimation('Wave', []), TypeError);
  assert.throws(() => p.playAnimation('Wave', { pace: 2 }), TypeError);
  assert.throws(() => p.playAnimation('Wave', { speed: '2' }), TypeError);
  assert.throws(() => p.playAnimation('Wave', { speed: 11 }), RangeError);
  assert.throws(() => p.playAnimation('Wave', { time: -1 }), RangeError);
  assert.throws(() => p.playAnimation('Wave', { time: Infinity }), TypeError);
  assert.throws(() => p.playAnimation('Wave', { loop: 'yes' }), TypeError);
  assert.throws(() => p.playAnimation('Wave', { lockMovement: 1 }), TypeError);
  assert.throws(() => p.playAnimation('Wave', { tags: 5 }), TypeError);
  assert.throws(() => p.playAnimation('Wave', { tags: 'a b' }), RangeError);
  assert.throws(() => p.playAnimation('Wave', { clip: 'a/wave.caf' }), TypeError, 'fragment');
  assert.throws(() => p.playAnimation('', { clip: '/etc/wave.caf' }), RangeError);
  assert.throws(() => p.playAnimation('', { clip: 'animations/../wave.caf' }), RangeError);
  assert.throws(() => p.playAnimation('', { clip: 'a'.repeat(201) }), RangeError);
  assert.throws(() => p.playAnimation('', { props: 'bucket_a_a' }), TypeError);
  assert.throws(() => p.playAnimation('', { props: [{}] }), TypeError, 'item or model');
  assert.throws(
    () => p.playAnimation('', { props: [{ item: 'alchemyOil', model: 'bucket_a_a' }] }),
    TypeError,
  );
  assert.throws(() => p.playAnimation('', { props: [{ model: 'not_a_prop' }] }), RangeError);
  assert.throws(() => p.playAnimation('', { props: [{ item: 'not_an_item' }] }), RangeError);
  assert.throws(() => p.playAnimation('', { props: [{ model: 'bucket_a_a', hand: 'top' }] }));
  assert.throws(
    () => p.playAnimation('', { props: [{ model: 'bucket_a_a', size: 2 }] }),
    TypeError,
  );
  assert.throws(
    () => p.playAnimation('', { props: [{ model: 'bucket_a_a', position: { x: NaN } }] }),
    TypeError,
  );
  assert.throws(
    () => p.playAnimation('', { props: [{ model: 'bucket_a_a', position: { x: 101 } }] }),
    RangeError,
  );
  assert.throws(
    () =>
      p.playAnimation('', {
        props: [{ model: 'bucket_a_a', rotation: { w: 0, x: 0, y: 0, z: 0 } }],
      }),
    TypeError,
  );
  const three = { model: 'bucket_a_a' };
  assert.throws(() => p.playAnimation('', { props: [three, three, three] }), RangeError);
  assert.throws(() => p.stopAnimation(null), TypeError);
  assert.throws(() => p.stopAnimation({ standUp: 1 }), TypeError);
  assert.throws(() => p.stopAnimation({ quickly: true }), TypeError);
  // None of them reached the client or used up a request id.
  assert.deepEqual(orders(output), []);
  assert.equal(p.playAnimation('Wave'), true);
  assert.equal(lastOrder(output).request, 1);
});

test('a prop item may name an animation prop, and its catalog path works as model', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  const scoop =
    'objects/manmade/task_specific_props/household/cooking_eating/eating_tools/wooden_scoop.cgf';
  assert.equal(p.playAnimation('GetMash', { props: { item: 'Scoop' } }), true);
  assert.equal(lastOrder(output).props[0].model, scoop);
  // The path Animations.props() gives, and the game's own form without objects/ and .cgf.
  const listed = host
    .makeAPI()
    .Animations.props()
    .find((prop) => prop.name === 'Scoop');
  assert.equal(listed.model, scoop);
  const gameForm = 'manmade/task_specific_props/household/cooking_eating/eating_tools/wooden_scoop';
  assert.equal(p.playAnimation('GetMash', { props: { model: gameForm } }), true);
  assert.equal(lastOrder(output).props[0].model, scoop);
});

test('props travel as an item class or a catalog mesh with the hand and offsets', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  assert.equal(
    p.playAnimation('', {
      tags: 'r_bucket',
      props: [
        { model: 'bucket_a_a' },
        {
          item: 'alchemyOil',
          hand: 'left',
          joint: 'hand_L_end',
          position: { x: 0.1, y: 0.2 },
          rotation: { x: 0, y: 90, z: 0 },
        },
      ],
    }),
    true,
  );
  const { props } = lastOrder(output);
  assert.deepEqual(props[0], {
    model: 'objects/manmade/common_furniture/bucket/bucket_a_a.cgf',
    hand: 'right',
    joint: '',
  });
  assert.equal(props[1].item, '34bd1d0b-1203-42a0-b9b4-d3585a4d9b48');
  assert.equal(props[1].hand, 'left');
  assert.equal(props[1].joint, 'hand_L_end');
  assert.deepEqual(props[1].position, { x: 0.1, y: 0.2, z: 0 });
  // Euler angles are degrees, as everywhere in the API.
  const { x, y, z, w } = props[1].rotation;
  const expected = rotation({ x: 0, y: 90, z: 0 });
  assert.ok(Math.abs(Math.hypot(x, y, z, w) - 1) < 1e-9);
  assert.ok(Math.abs(x - expected.x) + Math.abs(y - expected.y) + Math.abs(z - expected.z) < 1e-9);
  assert.ok(Math.abs(w - expected.w) < 1e-9);
  // The game's joints have spaces; '|' is the field's own.
  assert.equal(
    p.playAnimation('DeadBody_Hanged', {
      tags: 'slaveHalter+hanged_01',
      props: [{ item: 'Noose_mHangman', joint: 'Bip01 Neck' }],
    }),
    true,
  );
  assert.equal(lastOrder(output).props[0].joint, 'Bip01 Neck');
  assert.throws(
    () => p.playAnimation('', { props: [{ model: 'bucket_a_a', joint: 'Bip01|Neck' }] }),
    RangeError,
  );
  // A single prop and a quaternion are fine too.
  assert.equal(
    p.playAnimation('', { props: { model: 'bucket_a_a', rotation: { w: 2, x: 0, y: 0, z: 0 } } }),
    true,
  );
  assert.deepEqual(lastOrder(output).props[0].rotation, { x: 0, y: 0, z: 0, w: 1 });
});

test("attach: 'game' holds a prop in the game's grip, the hand's slot joint", async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  assert.equal(
    p.playAnimation('Guest_DrinkBeer', {
      props: [
        { item: 'Scoop', attach: 'game' },
        { model: 'bucket_a_a', hand: 'left', attach: 'game' },
      ],
    }),
    true,
  );
  const { props } = lastOrder(output);
  assert.equal(props[0].joint, 'RightHandSlot');
  assert.equal(props[1].joint, 'LeftHandSlot');
  // 'bone', the default, keeps the hand's own joint.
  assert.equal(
    p.playAnimation('Guest_DrinkBeer', { props: { item: 'Scoop', attach: 'bone' } }),
    true,
  );
  assert.equal(lastOrder(output).props[0].joint, '');
  assert.throws(
    () => p.playAnimation('Guest_DrinkBeer', { props: { item: 'Scoop', attach: 'glue' } }),
    /attach must be/,
  );
  assert.throws(
    () =>
      p.playAnimation('Guest_DrinkBeer', {
        props: { item: 'Scoop', attach: 'game', joint: 'RightHand' },
      }),
    /not both/,
  );
});

test('a prop rests as a Prop before and after, and its moments raise events', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  const events = [];
  const emit = host.emit.bind(host);
  host.emit = (name, ...args) => {
    if (name === 'propPickedUp' || name === 'propPlaced') events.push([name, ...args]);
    return emit(name, ...args);
  };
  const resting = { id: 31 };
  assert.equal(
    p.playAnimation('GetMash', {
      props: { item: 'Scoop', attach: 'game', fromAnimation: true, rest: resting },
    }),
    true,
  );
  const order = lastOrder(output);
  assert.equal(order.props[0].rest, 31);
  assert.throws(
    () => p.playAnimation('GetMash', { props: { item: 'Scoop', rest: resting } }),
    /only with fromAnimation/,
  );
  assert.throws(
    () => p.playAnimation('GetMash', { props: { item: 'Scoop', fromAnimation: true, rest: 'x' } }),
    /must be a Prop/,
  );
  host.playerAnimations.propCue(p, { request: order.request, prop: 0, taken: true });
  host.playerAnimations.propCue(p, { request: order.request, prop: 0, taken: false });
  // A stale request or a prop that does not follow the animation raises nothing.
  host.playerAnimations.propCue(p, { request: order.request + 99, prop: 0, taken: true });
  host.playerAnimations.propCue(p, { request: order.request, prop: 1, taken: true });
  assert.deepEqual(
    events.map(([name, who, prop]) => [name, who, prop.index, prop.fragment, prop.hand]),
    [
      ['propPickedUp', p, 0, 'GetMash', 'right'],
      ['propPlaced', p, 0, 'GetMash', 'right'],
    ],
  );
});

test('a prop can follow the animation: held only from its pickup to its put-down', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  assert.equal(p.playAnimation('GetMash', { props: { item: 'Scoop', fromAnimation: true } }), true);
  assert.equal(lastOrder(output).props[0].fromAnimation, true);
  assert.equal(
    p.playAnimation('GetMash', { props: { item: 'Scoop', fromAnimation: false } }),
    true,
  );
  assert.equal('fromAnimation' in lastOrder(output).props[0], false);
  // held from the start until the animation's detach, not with fromAnimation.
  assert.equal(p.playAnimation('GetMash', { props: { item: 'Scoop', untilPutDown: true } }), true);
  assert.equal(lastOrder(output).props[0].untilPutDown, true);
  assert.throws(
    () =>
      p.playAnimation('GetMash', {
        props: { item: 'Scoop', untilPutDown: true, fromAnimation: true },
      }),
    /not both/,
  );
  assert.throws(
    () => p.playAnimation('GetMash', { props: { item: 'Scoop', fromAnimation: 'yes' } }),
    TypeError,
  );
});

test('a one-shot ends once, with the original fragment, and chaining from the handler works', async () => {
  const { host, output } = hostWith(`
    kcds.core.Events.on('playerAnimationEnd', (player, fragment, reason) => {
      if (fragment === 'Wave' && reason === 'finished') player.playAnimation('Bow');
    });
  `);
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('Wave');
  report(host, 1, 1, 'finished');
  assert.deepEqual(ends(output), ['1 Wave finished']);
  // The chained animation is the player's state now, as request 2.
  assert.equal(lastOrder(output).fragment, 'Bow');
  assert.equal(lastOrder(output).request, 2);
  // A duplicate of the report does nothing, and the finished Wave is not "replaced" either.
  report(host, 1, 1, 'finished');
  report(host, 1, 1, 'interrupted');
  assert.deepEqual(ends(output), ['1 Wave finished']);
  report(host, 1, 2, 'interrupted');
  assert.deepEqual(ends(output), ['1 Wave finished', '1 Bow interrupted']);
  report(host, 1, 2, 'finished');
  assert.equal(ends(output).length, 2);
});

test('refused and interrupted are reported as the client says', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('NoSuchFragment');
  report(host, 1, 1, 'refused');
  p.playAnimation('Wave');
  report(host, 1, 2, 'interrupted');
  assert.deepEqual(ends(output), ['1 NoSuchFragment refused', '1 Wave interrupted']);
});

// tags none of which the body has ('monk' on a woman) are refused with the fragment '';
// a tags-only request raises nothing else.
test('tags only: a refusal is raised with the fragment empty, nothing else is', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('', { tags: 'monk' });
  report(host, 1, 1, 'refused');
  assert.deepEqual(ends(output), ['1  refused']);
  p.playAnimation('', { tags: 'drunk' });
  report(host, 1, 2, 'finished');
  assert.deepEqual(ends(output), ['1  refused']);
  p.playAnimation('Wave');
  assert.deepEqual(ends(output), ['1  refused'], 'replacing a tags-only request raises nothing');
});

test('replacing a running animation ends it replaced at once; its late report is stale', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('Wave');
  p.playAnimation('Wave');
  assert.deepEqual(ends(output), ['1 Wave replaced']);
  // The first request's outcome reaches the server late: it cannot end the second request, which
  // has the same fragment.
  report(host, 1, 1, 'finished');
  assert.deepEqual(ends(output), ['1 Wave replaced']);
  report(host, 1, 2, 'finished');
  assert.deepEqual(ends(output), ['1 Wave replaced', '1 Wave finished']);
  // A request that already ended is not ended again by its replacement.
  p.playAnimation('Bow');
  assert.deepEqual(ends(output), ['1 Wave replaced', '1 Wave finished']);
});

test('stopAnimation ends the running animation stopped, once', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('Wave');
  assert.equal(p.stopAnimation(), true);
  assert.deepEqual(lastOrder(output), {
    op: 'animation',
    id: 1,
    kind: 'stop',
    request: 0,
    standUp: false,
  });
  assert.deepEqual(ends(output), ['1 Wave stopped']);
  // The client's own reports for the stopped request, and a second stop, add nothing.
  report(host, 1, 1, 'interrupted');
  report(host, 1, 1, 'stopped');
  assert.equal(p.stopAnimation(), true);
  assert.deepEqual(ends(output), ['1 Wave stopped']);
  // Stopping an animation that already finished raises nothing more.
  p.playAnimation('Bow');
  report(host, 1, 2, 'finished');
  p.stopAnimation();
  assert.deepEqual(ends(output), ['1 Wave stopped', '1 Bow finished']);
});

test('a client cannot end a request stopped or replaced, or one it was not given', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('Wave');
  report(host, 1, 1, 'stopped');
  report(host, 1, 1, 'replaced');
  report(host, 1, 1, 'nonsense');
  report(host, 1, 99, 'finished');
  report(host, 1, 0, 'finished');
  report(host, 1, -1, 'finished');
  report(host, 1, 1.5, 'finished');
  report(host, 1, 1, undefined);
  host.playerAnimations.report(undefined, { id: 5, request: 1, reason: 'finished' });
  assert.deepEqual(ends(output), []);
  report(host, 1, 1, 'finished');
  assert.deepEqual(ends(output), ['1 Wave finished']);
});

test('a loop ends only stopped, replaced or refused', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('Idle', { loop: true });
  // Natural ends and interruptions restart a loop: nothing is raised.
  report(host, 1, 1, 'finished');
  report(host, 1, 1, 'interrupted');
  assert.deepEqual(ends(output), []);
  p.stopAnimation();
  assert.deepEqual(ends(output), ['1 Idle stopped']);
  p.playAnimation('Idle', { loop: true });
  p.playAnimation('Idle', { loop: true });
  assert.deepEqual(ends(output), ['1 Idle stopped', '1 Idle replaced']);
  p.playAnimation('Idle', { loop: true });
  assert.equal(ends(output).length, 3);
  report(host, 1, 4, 'refused');
  assert.deepEqual(ends(output).slice(-2), ['1 Idle replaced', '1 Idle refused']);
  assert.equal(ends(output).length, 4);
});

test('a request with no fragment raises nothing but a refusal and a clip end', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('', { tags: 'r_bucket', props: [{ model: 'bucket_a_a' }] });
  p.playAnimation('', { clip: 'animations/myresource/wave.caf' });
  p.playAnimation('', { tags: 'r_bucket' });
  report(host, 1, 3, 'finished');
  p.playAnimation('', { tags: 'r_bucket' });
  p.stopAnimation();
  p.playAnimation('', { tags: 'r_bucket' });
  report(host, 1, 5, 'refused');
  // The clip (request 2) was replaced by the next play: its end is raised, with its name.
  assert.deepEqual(ends(output), ['1  replaced', '1  refused']);
  // Five plays and a stop went out.
  assert.equal(orders(output).length, 6);
});

test('stopAnimation with standUp starts its own request, named by the client', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('Sit', { tags: 'sittingNoTable', loop: true });
  assert.equal(p.stopAnimation({ standUp: true }), true);
  assert.deepEqual(lastOrder(output), {
    op: 'animation',
    id: 1,
    kind: 'stop',
    request: 2,
    standUp: true,
  });
  assert.deepEqual(ends(output), ['1 Sit stopped']);
  // The player is on their feet: the stand-up the client chose ends, once.
  report(host, 1, 2, 'finished', 'StandUp');
  report(host, 1, 2, 'finished', 'StandUp');
  assert.deepEqual(ends(output), ['1 Sit stopped', '1 StandUp finished']);
  // A stance the game authors no way out of: nothing to report.
  p.playAnimation('Kneel', { tags: 'beggarKneel' });
  p.stopAnimation({ standUp: true });
  report(host, 1, 4, 'finished', '');
  assert.deepEqual(ends(output), ['1 Sit stopped', '1 StandUp finished', '1 Kneel stopped']);
  // Nothing to stand up from.
  p.stopAnimation({ standUp: true });
  assert.equal(lastOrder(output).request, 0);
  assert.equal(lastOrder(output).standUp, false);
});

test('a playAnimation during the stand-up replaces it, as the fragment the client names', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('Lie', { tags: 'lyingGround' });
  p.stopAnimation({ standUp: true });
  p.playAnimation('Wave');
  // The server cannot name the stand-up; nothing is raised until the client does.
  assert.deepEqual(ends(output), ['1 Lie stopped']);
  report(host, 1, 2, 'replaced', 'GetUp');
  report(host, 1, 2, 'replaced', 'GetUp');
  assert.deepEqual(ends(output), ['1 Lie stopped', '1 GetUp replaced']);
  // The client may have finished it just before: its own outcome stands.
  p.playAnimation('Lie', { tags: 'lyingGround' });
  p.stopAnimation({ standUp: true });
  p.stopAnimation();
  report(host, 1, 5, 'finished', 'StandUp');
  assert.deepEqual(ends(output).slice(-2), ['1 Lie stopped', '1 StandUp finished']);
  // A stand-up the client never named stays silent.
  p.playAnimation('Lie', { tags: 'lyingGround' });
  p.stopAnimation({ standUp: true });
  p.playAnimation('Bow');
  report(host, 1, 7, 'replaced', '');
  const before = ends(output).length;
  report(host, 1, 7, 'replaced', 'StandUp');
  assert.equal(ends(output).length, before);
});

test('the animation is sent again, with its id, when the world gets ready again', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('Sit', { loop: true, tags: 'sittingNoTable', lockMovement: true });
  const first = orders(output).length;
  player(host, 1, false);
  assert.equal(orders(output).length, first);
  assert.equal(p.playAnimation('Wave'), false);
  player(host, 1, true);
  const again = orders(output).slice(first);
  assert.equal(again.length, 1);
  assert.equal(again[0].request, 1);
  assert.equal(again[0].fragment, 'Sit');
  assert.equal(again[0].lockMovement, true);
  assert.equal(again[0].restore, false);
  // A request that ended is only restored, not played again, and ends nothing a second time.
  p.playAnimation('Wave');
  report(host, 1, 2, 'finished');
  player(host, 1, false);
  player(host, 1, true);
  assert.equal(lastOrder(output).request, 2);
  assert.equal(lastOrder(output).restore, true);
  assert.deepEqual(ends(output), ['1 Sit replaced', '1 Wave finished']);
  // The new body reports the re-sent request: its end is the one the server waits for.
  p.playAnimation('Bow');
  player(host, 1, false);
  player(host, 1, true);
  assert.equal(lastOrder(output).restore, false);
  report(host, 1, 3, 'finished');
  assert.deepEqual(ends(output).at(-1), '1 Bow finished');
  // Nothing is sent for a stopped animation, nor for a stand-up of the body that is gone, and the
  // gone body's report of it raises nothing.
  p.stopAnimation({ standUp: true });
  const sent = orders(output).length;
  player(host, 1, false);
  player(host, 1, true);
  assert.equal(orders(output).length, sent);
  report(host, 1, 4, 'finished', 'StandUp');
  assert.deepEqual(ends(output), ['1 Sit replaced', '1 Wave finished', '1 Bow finished']);
});

test('a player leaving takes the animation with them and raises nothing', async () => {
  const { host, output } = hostWith(`
    kcds.core.Events.on('playerDisconnect', (player) => {
      kcds.core.Chat.sendToAll('left ' + player.playAnimation('Wave') + ' ' + player.stopAnimation());
    });
  `);
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('Sit', { loop: true });
  const sent = orders(output).length;
  host.disconnect(1);
  assert.deepEqual(
    output.filter((e) => e.op === 'chat' && /^left /.test(e.text)).map((e) => e.text),
    ['left false false'],
  );
  assert.deepEqual(ends(output), []);
  assert.equal(orders(output).length, sent);
  // A late report for the player, or a handle kept past the disconnect, does nothing.
  report(host, 1, 1, 'finished');
  assert.equal(p.playAnimation('Wave'), false);
  assert.deepEqual(ends(output), []);
  // The next player starts from nothing, with a request id nobody had before.
  const next = player(host, 2);
  next.playAnimation('Wave');
  assert.equal(lastOrder(output).request, 2);
  report(host, 2, 1, 'finished');
  assert.deepEqual(ends(output), []);
});

test('each player has requests of their own', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const a = player(host, 1);
  const b = player(host, 2);
  a.playAnimation('Wave');
  b.playAnimation('Wave');
  // Request 2 is player 2's: player 1 cannot end it.
  report(host, 1, 2, 'finished');
  assert.deepEqual(ends(output), []);
  report(host, 2, 2, 'finished');
  report(host, 1, 1, 'finished');
  assert.deepEqual(ends(output), ['2 Wave finished', '1 Wave finished']);
});

// the weapon a player holds, and props: 'held' that keeps it in the hand.
test("heldWeapon names the weapon in the right hand and props: 'held' holds what the hands hold", async () => {
  const { host, output } = hostWith();
  await host.startAll();
  const p = player(host, 1);
  const sword = '00cca9e3-8ef2-46db-8cbf-86ec51930919'; // longSwordDuel
  const swordHex = '46db8ef200cca9e319099351ec86bf8c';
  const shieldHex = '4e06f904000a72ec73ecb68aac2e578c'; // shieldCuman_06
  assert.equal(p.heldWeapon, '');
  host.updatePlayer({ id: 1, ready: true, hands: [swordHex, shieldHex] });
  assert.equal(p.heldWeapon, sword);
  assert.equal(p.playAnimation('Wave', { props: 'held' }), true);
  assert.deepEqual(lastOrder(output).props, [
    { item: sword, hand: 'right', joint: 'RightHandSlot' },
    { item: '000a72ec-f904-4e06-8c57-2eac8ab6ec73', hand: 'left', joint: 'LeftHandSlot' },
  ]);
  // An apple in the hand is no weapon; empty hands hold nothing.
  const appleHex = '4c0f590e2264f217f6b932650cf5c6a4';
  host.updatePlayer({ id: 1, ready: true, hands: [appleHex, ''] });
  assert.equal(p.heldWeapon, '');
  host.updatePlayer({ id: 1, ready: true, hands: ['', ''] });
  assert.throws(() => p.playAnimation('', { props: 'held' }), TypeError);
  assert.equal(p.playAnimation('Wave', { props: 'held' }), true);
  assert.deepEqual(lastOrder(output).props, []);
});

test('a clip played without a fragment ends with its name (finished, interrupted, refused)', async () => {
  const { host, output } = hostWith(
    `kcds.core.Events.on('playerAnimationEnd', (player, fragment, reason, clip) =>
       kcds.core.Chat.sendToAll('clip ' + clip + ' ' + reason));`,
  );
  await host.startAll();
  const p = player(host, 1);
  p.playAnimation('', { clip: 'wedding_dance_a' });
  report(host, 1, 1, 'finished');
  p.playAnimation('', { clip: 'wedding_dance_b' });
  report(host, 1, 2, 'interrupted');
  const clips = output
    .filter((e) => e.op === 'chat' && /^clip /.test(e.text))
    .map((e) => e.text.slice('clip '.length));
  assert.deepEqual(clips, ['wedding_dance_a finished', 'wedding_dance_b interrupted']);
});

// an animation at a horse puts the body beside it, turned to it, then plays.
test('playAnimation with a target places the body beside it and then plays', async () => {
  const { host, output } = hostWith();
  await host.startAll();
  host.playerAnimations.targetDelayMs = 0;
  const groom = player(host, 1);
  const horse = host.api.Horse.spawn({ x: 10, y: 20, z: 30 });
  assert.equal(groom.playAnimation('PrayStand', { target: horse, side: 'right' }), true);
  // On the horse's right (it faces +y): 1 m to +x, turned to face -x.
  const placed = output.filter((e) => e.op === 'entityPosition' && e.id === 1).at(-1);
  assert.ok(placed, 'the body is placed first');
  assert.ok(Math.abs(placed.position.x - 11) < 1e-6 && Math.abs(placed.position.y - 20) < 1e-6);
  await new Promise((resolve) => setTimeout(resolve, 5));
  assert.equal(lastOrder(output).fragment, 'PrayStand');
  assert.throws(() => groom.playAnimation('PrayStand', { side: 'left' }), TypeError);
  assert.throws(() => groom.playAnimation('PrayStand', { target: horse, side: 'top' }), RangeError);
  assert.throws(() => groom.playAnimation('PrayStand', { target: horse, distance: 9 }), RangeError);
  assert.equal(groom.playAnimation('PrayStand', { target: groom }), false);
});
