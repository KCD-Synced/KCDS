'use strict';

// Prop: the props the server puts in the world.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-props-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'decor');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'decor', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  const chat = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  return { host, chat, output };
}

const BARREL = 'objects/manmade/common_furniture/barrels/barrel_a.cgf';
const DOBESOVICKO = 'structures/living/houses/unique/dobesovicko/house_1s_4r_angled_a_dobesovicko';

test('addMark puts a mark on a joint of the player until removeMark', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const player = host.updatePlayer({
    id: 5,
    ready: true,
    position: { x: 0, y: 0, z: 10 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const brand = player.addMark({
    model: 'paper_a',
    joint: 'RightArm',
    position: { x: 0.02, y: 0.05, z: 0 },
    scale: 0.2,
  });
  assert.equal(typeof brand, 'number');
  const spawn = output.filter((e) => e.op === 'propSpawn').at(-1);
  assert.equal(spawn.id, brand);
  assert.equal(spawn.body, 5);
  assert.equal(spawn.joint, 'RightArm');
  assert.deepEqual(player.marks, [brand]);
  // A prop hung with attachProp is no mark.
  const sack = player.attachProp({ model: 'sack_a', joint: 'Head' });
  assert.deepEqual(player.marks, [brand]);
  assert.equal(player.removeMark(sack.id), false);
  assert.throws(() => player.addMark({ model: 'paper_a' }), TypeError);
  assert.throws(() => player.addMark(null), TypeError);
  assert.equal(player.removeMark(brand), true);
  assert.equal(player.removeMark(brand), false);
  assert.deepEqual(player.marks, []);
});

test('attachProp hangs a mesh on a joint of the player, detachProp and leaving take it off', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const player = host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const sack = player.attachProp({
    model: 'barrel_a',
    joint: 'Head',
    position: { x: 0, y: 0.05, z: 0.1 },
  });
  assert.equal(sack.attachedTo, player);
  assert.equal(sack.joint, 'Head');
  assert.equal(sack.physics, 'none');
  const spawn = output.filter((e) => e.op === 'propSpawn').at(-1);
  assert.equal(spawn.body, 7);
  assert.equal(spawn.joint, 'Head');
  assert.equal(spawn.physics, 'none');
  assert.deepEqual(spawn.position, { x: 0, y: 0.05, z: 0.1 });
  // It has no physics; it stays within reach of the joint.
  assert.throws(() => {
    sack.physics = 'rigid';
  }, TypeError);
  assert.throws(() => sack.setPose({ x: 9, y: 0, z: 0 }), RangeError);
  assert.throws(
    () => player.attachProp({ model: 'barrel_a', position: { x: 6, y: 0, z: 0 } }),
    RangeError,
  );
  assert.throws(() => player.attachProp({ model: 'barrel_a', joint: 7 }), TypeError);
  // A prop in the world is not the player's to take off.
  const { Prop } = host.makeAPI();
  const loose = Prop.spawn('barrel_a', { x: 1, y: 2, z: 3 });
  assert.equal(loose.attachedTo, null);
  assert.equal(player.detachProp(loose), false);
  assert.equal(player.detachProp(sack.id), true);
  assert.equal(Prop.getById(sack.id), null);
  // The props on a player who leaves go with them.
  const rope = player.attachProp({ model: 'barrel_a', joint: 'LeftHand' });
  host.disconnect(7);
  assert.equal(Prop.getById(rope.id), null);
  assert.equal(Prop.getById(loose.id), loose);
});

test('attachProp hand hangs a prop in the game hand slot, as the game holds its rope cuffs', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const player = host.updatePlayer({
    id: 7,
    nickname: 'P7',
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const cuffs = player.attachProp({ model: 'barrel_a', hand: 'left' });
  assert.equal(cuffs.joint, 'LeftHandSlot');
  assert.equal(output.filter((e) => e.op === 'propSpawn').at(-1).joint, 'LeftHandSlot');
  assert.equal(player.attachProp({ model: 'barrel_a', hand: 'right' }).joint, 'RightHandSlot');
  assert.throws(() => player.attachProp({ model: 'barrel_a', hand: 'both' }), /'left' or 'right'/);
  assert.throws(
    () => player.attachProp({ model: 'barrel_a', hand: 'left', joint: 'Head' }),
    /a joint or a hand/,
  );
});

test('Prop.spawn replicates a mesh, and destroy takes it away', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('propSpawn', (p) => kcds.core.Chat.sendToAll('spawn ' + p.meshName));
    kcds.core.Events.on('propDestroy', (p) => kcds.core.Chat.sendToAll('destroy ' + p.model));
    kcds.world.Prop.spawn('barrel_a', { x: 1, y: 2, z: 3 }, { x: 0, y: 0, z: 90 }, 2, 'rigid', 7);
  `);
  await host.startAll();
  const { Prop } = host.makeAPI();
  const [prop] = Prop.all();
  assert.equal(prop.model, BARREL);
  assert.equal(prop.meshName, 'barrel_a');
  assert.equal(prop.meshGroup, 'manmade/common_furniture/barrels');
  assert.equal(prop.physics, 'rigid');
  assert.equal(prop.scale, 2);
  assert.equal(prop.dimension, 7);
  assert.equal(prop.position.x, 1);
  assert.equal(Prop.getById(prop.id), prop);
  assert.equal(new Prop(prop.id), prop);
  const spawn = output.find((e) => e.op === 'propSpawn');
  assert.deepEqual(
    { ...spawn, position: { ...spawn.position }, rotation: undefined },
    {
      op: 'propSpawn',
      id: prop.id,
      model: BARREL,
      position: { x: 1, y: 2, z: 3 },
      rotation: undefined,
      scale: { x: 2, y: 2, z: 2 },
      physics: 'rigid',
      dimension: 7,
      mass: 0,
    },
  );
  prop.destroy();
  prop.destroy();
  assert.deepEqual(chat(), ['spawn barrel_a', `destroy ${BARREL}`]);
  assert.equal(output.filter((e) => e.op === 'propDestroy').length, 1);
  assert.equal(Prop.getById(prop.id), null);
});

test('physics and scale changes are sent once, and only when they change', async () => {
  const { host, output } = hostWith(`kcds.world.Prop.spawn('barrel_a');`);
  await host.startAll();
  const [prop] = host.makeAPI().Prop.all();
  assert.equal(prop.physics, 'static');
  assert.equal(prop.dimension, 0xffffffff);
  prop.physics = 'static';
  prop.scale = 1;
  assert.equal(output.filter((e) => e.op === 'propUpdate').length, 0);
  prop.physics = 'none';
  prop.scale = 1000;
  prop.scale = { x: 1, y: 2, z: 3 };
  const updates = output.filter((e) => e.op === 'propUpdate');
  assert.equal(updates.length, 3);
  assert.deepEqual(updates[0], {
    op: 'propUpdate',
    id: prop.id,
    physics: 'none',
    scale: { x: 1, y: 1, z: 1 },
    mass: 0,
  });
  assert.deepEqual(updates[1].scale, { x: 100, y: 100, z: 100 });
  assert.deepEqual(updates[2].scale, { x: 1, y: 2, z: 3 });
  assert.equal(prop.scale.z, 3);
  assert.throws(() => (prop.physics = 'soft'), RangeError);
});

test('a hidden static prop still collides and is drawn by no one', async () => {
  const { host, output } = hostWith(`kcds.world.Prop.spawn('barrel_a');`);
  await host.startAll();
  const [prop] = host.makeAPI().Prop.all();
  assert.equal(prop.visible, true);
  prop.visible = false;
  prop.visible = false;
  let updates = output.filter((e) => e.op === 'propUpdate');
  assert.equal(updates.length, 1);
  assert.equal(updates[0].physics, 'wall');
  assert.equal(prop.visible, false);
  assert.equal(prop.physics, 'static');
  // A player who comes later is sent it hidden too.
  assert.equal(host.props.spawnMessage(prop).physics, 'wall');
  assert.throws(() => (prop.physics = 'rigid'), TypeError);
  assert.throws(() => (prop.visible = 'no'), TypeError);
  prop.visible = true;
  updates = output.filter((e) => e.op === 'propUpdate');
  assert.equal(updates.at(-1).physics, 'static');
  prop.physics = 'rigid';
  assert.throws(() => (prop.visible = false), TypeError);
});

test('Prop.spawn refuses unknown meshes and accepts streamed ones', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const { Prop } = host.makeAPI();
  assert.throws(() => Prop.spawn('no_such_mesh_anywhere'), RangeError);
  assert.throws(() => Prop.spawn(BARREL, {}, {}, 1, 'static', -1), TypeError);
  assert.throws(() => Prop.spawn(BARREL, { x: 1e9 }), RangeError);
  const chair = Prop.spawn('objects/kcds/decor/chair.cgf');
  assert.equal(chair.meshGroup, 'kcds/decor');
  assert.equal(chair.meshName, 'chair');
});

test('Prop.spawn builds a game prefab by its path or name, static and at its own size', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  const { Prop } = host.makeAPI();
  const house = Prop.spawn('prefab:house_1s_4r_angled_a_dobesovicko', { x: 10, y: 20, z: 30 });
  assert.equal(house.model, `prefab:${DOBESOVICKO}`);
  assert.equal(house.meshName, 'house_1s_4r_angled_a_dobesovicko');
  assert.equal(house.meshGroup, 'prefabs/structures/living/houses/unique/dobesovicko');
  assert.equal(house.physics, 'static');
  // The client is sent the template's GUID.
  const spawn = output.filter((e) => e.op === 'propSpawn').at(-1);
  assert.equal(spawn.model, 'prefab:551ac3b8-8105-42b8-93c0-6541cf948ddf');
  assert.equal(spawn.physics, 'static');
  // Its full path, with or without prefabs/ and .xml, names it as well.
  const same = Prop.spawn(`prefab:Prefabs/${DOBESOVICKO}.xml`);
  assert.equal(same.model, house.model);
  // Static, at its own size, in the world.
  assert.throws(() => Prop.spawn('prefab:house_1s_4r_angled_a_dobesovicko', {}, {}, 2), TypeError);
  assert.throws(
    () => Prop.spawn('prefab:house_1s_4r_angled_a_dobesovicko', {}, {}, 1, 'rigid'),
    TypeError,
  );
  assert.throws(() => (house.scale = 2), TypeError);
  assert.throws(() => (house.physics = 'none'), TypeError);
  assert.throws(() => (house.visible = false), TypeError);
  // A house whose doors are prefabs of their own is built with them; one the catalog does not
  // carry and one that holds a prefab the game does not have are refused.
  assert.equal(Prop.spawn('prefab:house_1s_2r_a').meshName, 'house_1s_2r_a');
  assert.throws(() => Prop.spawn('prefab:no_such_prefab'), /does not carry/);
  assert.throws(() => Prop.spawn('prefab:wild_beehive_01'), /does not have/);
  // It moves and goes away as any prop.
  house.position = { x: 11, y: 20, z: 30 };
  assert.equal(output.filter((e) => e.op === 'propMove').at(-1).id, house.id);
  house.destroy();
  assert.equal(output.filter((e) => e.op === 'propDestroy').at(-1).id, house.id);
});

test('Prop.collisionOf tells how a catalog mesh collides', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const { Prop } = host.makeAPI();
  // The stake collides only with a capsule and a rough mesh: nothing stands on its top.
  assert.equal(Prop.collisionOf('witch_stake'), 'proxy');
  assert.equal(Prop.collisionOf('barrel_a'), 'proxy');
  assert.equal(Prop.collisionOf('carpet_a'), 'none');
  assert.equal(Prop.collisionOf('objects/characters/animals/crayfish/crayfish.cgf'), 'mesh');
  assert.equal(Prop.collisionOf('not_a_mesh'), null);
  assert.equal(Prop.collisionOf(7), null);
});

test('Prop.destroyAll clears one dimension or all of them', async () => {
  const { host, output } = hostWith(`
    kcds.world.Prop.spawn('barrel_a', {}, {}, 1, 'static', 1);
    kcds.world.Prop.spawn('barrel_a', {}, {}, 1, 'static', 1);
    kcds.world.Prop.spawn('barrel_a', {}, {}, 1, 'static', 2);
  `);
  await host.startAll();
  const { Prop } = host.makeAPI();
  assert.equal(Prop.destroyAll(1), 2);
  assert.equal(Prop.all().length, 1);
  assert.equal(Prop.destroyAll(), 1);
  assert.equal(output.filter((e) => e.op === 'propDestroy').length, 3);
});

test('a prop moves as it is and can be shown to some players only', async () => {
  const { host, output } = hostWith(`kcds.world.Prop.spawn('barrel_a', { x: 1, y: 2, z: 3 });`);
  await host.startAll();
  const report = (id) =>
    host.updatePlayer({
      id,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  const alice = report(5);
  const bob = report(6);
  const { Prop } = host.makeAPI();
  const [prop] = Prop.all();
  const ops = (op) => output.filter((e) => e.op === op);

  prop.position = { x: 4, y: 5, z: 6 };
  assert.deepEqual(prop.position.x, 4);
  assert.deepEqual(ops('propMove').at(-1), {
    op: 'propMove',
    id: prop.id,
    position: { x: 4, y: 5, z: 6 },
    rotation: { ...prop.facing },
  });
  prop.setPose({ x: 4, y: 5, z: 6 });
  assert.equal(ops('propMove').length, 1, 'no move where it already is');
  prop.setPose({ x: 7, y: 5, z: 6 }, { x: 0, y: 0, z: 90 });
  assert.equal(ops('propMove').length, 2);
  assert.throws(() => (prop.position = { x: Infinity, y: 0, z: 0 }), TypeError);
  assert.throws(() => (prop.position = { x: 1e7, y: 0, z: 0 }), RangeError);

  // Only Alice: Bob takes it down; later ops go to Alice alone.
  prop.showOnlyTo(alice);
  assert.deepEqual(prop.visibleTo, [alice]);
  assert.deepEqual(ops('propDestroy').at(-1), { op: 'propDestroy', id: prop.id, to: bob.id });
  prop.position = { x: 8, y: 5, z: 6 };
  assert.equal(ops('propMove').at(-1).to, alice.id);
  // A player whose world becomes ready gets it only when they see it.
  const before = ops('propSpawn').length;
  host.props.arrived(bob);
  assert.equal(ops('propSpawn').length, before);
  host.props.arrived(alice);
  assert.equal(ops('propSpawn').at(-1).to, alice.id);
  // Everyone again: Bob builds it.
  prop.showOnlyTo(null);
  assert.equal(prop.visibleTo, null);
  assert.equal(ops('propSpawn').at(-1).to, bob.id);
  assert.throws(() => prop.showOnlyTo([{}]), TypeError);

  // Hidden from Alice: everyone else sees it, Alice takes it down and is sent nothing of it.
  prop.hideFrom(alice);
  assert.deepEqual(prop.hiddenFrom, [alice]);
  assert.deepEqual(ops('propDestroy').at(-1), { op: 'propDestroy', id: prop.id, to: alice.id });
  prop.position = { x: 9, y: 5, z: 6 };
  assert.deepEqual(
    ops('propMove')
      .slice(-1)
      .map((e) => e.to),
    [bob.id],
    'a move goes to each player who sees it, not to Alice',
  );
  const spawns = ops('propSpawn').length;
  host.props.arrived(alice);
  assert.equal(ops('propSpawn').length, spawns, 'Alice is not told of it when her world is ready');
  // Hidden from nobody again: Alice builds it.
  prop.hideFrom(null);
  assert.deepEqual(prop.hiddenFrom, []);
  assert.equal(ops('propSpawn').at(-1).to, alice.id);
  assert.throws(() => prop.hideFrom([{}]), TypeError);
});

test('a rigid prop is simulated by the nearest player, pushed there and reported', async () => {
  const { host, chat, output } = hostWith(`
    kcds.core.Events.on('propRest', (prop, at, rotation) =>
      kcds.core.Chat.sendToAll('rest ' + prop.id + ' ' + at.x + ' ' + at.z + ' ' + rotation.w));
    kcds.core.Events.on('propHit', (prop, speed, at) =>
      kcds.core.Chat.sendToAll('hit ' + prop.id + ' ' + speed + ' ' + at.x));
    kcds.world.Prop.spawn('barrel_a', { x: 0, y: 0, z: 10 }, {}, 1, 'rigid');
    kcds.world.Prop.spawn('barrel_a', { x: 0, y: 0, z: 10 }, {}, 1, 'static');
  `);
  await host.startAll();
  const at = (id, x) =>
    host.updatePlayer({
      id,
      ready: true,
      position: { x, y: 0, z: 10 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  const near = at(5, 10);
  const far = at(6, 30);
  const [prop, fixed] = host.makeAPI().Prop.all();
  const ops = (op) => output.filter((e) => e.op === op);

  // The nearest player within 60 m simulates it; the election runs every half second.
  host.props.tick(1000);
  assert.equal(prop.simulator, near.id);
  assert.deepEqual(ops('propSimulate'), [
    { op: 'propSimulate', id: prop.id, simulate: true, to: near.id },
  ]);
  host.props.tick(1200);
  assert.equal(ops('propSimulate').length, 1);
  at(5, 200);
  host.props.tick(1500);
  assert.equal(prop.simulator, far.id);
  assert.deepEqual(ops('propSimulate').slice(1), [
    { op: 'propSimulate', id: prop.id, simulate: false, to: near.id },
    { op: 'propSimulate', id: prop.id, simulate: true, to: far.id },
  ]);

  // Pushes go to that player only.
  assert.equal(prop.applyImpulse({ x: 0, y: 0, z: 150 }, { x: 0, y: 0, z: 11 }), true);
  assert.deepEqual(ops('propPush').at(-1), {
    op: 'propPush',
    id: prop.id,
    push: 'impulse',
    linear: { x: 0, y: 0, z: 150 },
    angular: { x: 0, y: 0, z: 0 },
    point: { x: 0, y: 0, z: 11 },
    hasPoint: true,
    to: far.id,
  });
  assert.equal(prop.setVelocity({ x: 3, y: 0, z: 0 }, { x: 0, y: 0, z: 1 }), true);
  assert.equal(ops('propPush').at(-1).push, 'velocity');
  assert.equal(prop.wake(), true);
  assert.equal(prop.sleep(), true);
  assert.deepEqual(
    ops('propPush')
      .slice(-2)
      .map((e) => e.push),
    ['wake', 'sleep'],
  );
  const pushes = ops('propPush').length;
  assert.equal(prop.setVelocity({ x: 201, y: 0, z: 0 }), false);
  assert.equal(prop.setVelocity({ x: 0, y: 0, z: 0 }, { x: 101, y: 0, z: 0 }), false);
  assert.equal(prop.applyImpulse({ x: NaN, y: 0, z: 0 }), false);
  assert.equal(prop.applyImpulse({ x: 1, y: 0, z: 0 }, { x: 1e7, y: 0, z: 0 }), false);
  assert.equal(prop.applyImpulse(null), false);
  assert.equal(fixed.applyImpulse({ x: 1, y: 0, z: 0 }), false);
  assert.equal(fixed.wake(), false);
  assert.equal(fixed.resetPhysics(), false);
  assert.equal(ops('propPush').length, pushes);

  // Its reports: only the simulating player's count; the others are shown the pose.
  const pose = (resting, x = 2, z = 9) => ({
    prop: prop.id,
    position: { x, y: 0, z },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    resting,
  });
  assert.equal(host.props.report(near, pose(false)), false);
  assert.equal(host.props.report(far, pose(false)), true);
  assert.deepEqual(ops('propMove').at(-1), {
    op: 'propMove',
    id: prop.id,
    position: { x: 2, y: 0, z: 9 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
    to: near.id,
  });
  assert.equal(prop.position.x, 2);
  assert.deepEqual(chat(), []);
  assert.equal(host.props.report(far, pose(true, 3, 8)), true);
  assert.deepEqual(chat(), [`rest ${prop.id} 3 8 1`]);

  // Farther than 100 m from where it last rested: put back there, nothing raised.
  assert.equal(host.props.report(far, pose(true, 300, 8)), false);
  assert.deepEqual(ops('propMove').at(-1), {
    op: 'propMove',
    id: prop.id,
    position: { x: 3, y: 0, z: 8 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
  });
  assert.equal(chat().length, 1);

  // resetPhysics: back where it was spawned for everyone, no propRest.
  assert.equal(prop.resetPhysics(), true);
  assert.deepEqual(ops('propMove').at(-1).position, { x: 0, y: 0, z: 10 });
  assert.equal(prop.position.z, 10);
  assert.equal(chat().length, 1);

  // The mass rebuilds it on every client and the election starts again.
  assert.equal(prop.mass, 0);
  prop.mass = 20;
  prop.mass = -1;
  prop.mass = 100001;
  prop.mass = Infinity;
  assert.equal(prop.mass, 20);
  assert.deepEqual(ops('propUpdate').at(-1), {
    op: 'propUpdate',
    id: prop.id,
    physics: 'rigid',
    scale: { x: 1, y: 1, z: 1 },
    mass: 20,
  });
  assert.equal(prop.simulator, 0);
  host.props.tick(2500);
  assert.equal(prop.simulator, far.id);

  // However far, the nearest player simulates it.
  at(5, 400);
  at(6, 500);
  host.props.tick(3000);
  assert.equal(prop.simulator, near.id);

  // A hit raises propHit with its speed; a report without one raises none.
  const hit = (hitSpeed) => ({ ...pose(false, 3, 8), hitSpeed });
  assert.equal(host.props.report(near, hit(7.5)), true);
  assert.deepEqual(chat().at(-1), `hit ${prop.id} 7.5 3`);
  const said = chat().length;
  assert.equal(host.props.report(near, hit(0)), true);
  assert.equal(host.props.report(near, pose(false, 3, 8)), true);
  assert.equal(chat().length, said);

  // With nobody whose game shows it, it stays where it last rested.
  prop.showOnlyTo([]);
  host.props.tick(3500);
  assert.equal(prop.simulator, 0);
  assert.equal(prop.applyImpulse({ x: 1, y: 0, z: 0 }), false);
});
