'use strict';

// Felling the level's trees (Trees): blows with an axe, refusals, the server's fall,
// cutting the trunk into items and regrowth.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { lookupItemClass, classBytesHex } = require('./inventory/catalog.cjs');
const { isTreeMesh } = require('./trees/trees.cjs');

const BIRCH = 'objects/natural/vegetation/trees/normal_trees/betula_pendula/betula_pendula_a.cgf';
const AT = { x: 10, y: 20, z: 30 };
const AXE = classBytesHex(lookupItemClass('axeWork01'));
const SWORD = classBytesHex(lookupItemClass('longSwordDuel'));

async function started(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const api = f.host.makeAPI();
  let now = 1000000;
  f.host.trees.now = () => now;
  const ops = (...names) => f.sent.filter((message) => names.includes(message.op));
  const treeEvents = () =>
    f.sent
      .filter((message) => message.op === 'event' && message.name === 'kcds:tree')
      .map((message) => JSON.parse(message.payload).event);
  return { ...f, api, ops, treeEvents, advance: (ms) => (now += ms) };
}

// A blow of player `id` on the birch, as the player's game reports it.
function treeHit(distance = 1.5) {
  return {
    position: { x: AT.x - 0.3, y: AT.y, z: AT.z + 1.2 },
    normal: { x: -1, y: 0, z: 0 },
    distance,
    terrain: false,
    kind: 'brush',
    model: BIRCH,
    surface: 'mat_wood',
    lo: { kind: 'brush', name: BIRCH, at: AT, rot: [0, 0, 0, 1], scale: { x: 1, y: 1, z: 1 } },
  };
}

function swing(f, player, hit, ms = 500) {
  f.advance(ms);
  f.host.worldTraces.weaponHit(player, hit);
}

test('tree meshes: the vegetation trees, not fallen branches or stumps', () => {
  assert.equal(isTreeMesh(BIRCH), true);
  assert.equal(isTreeMesh('Objects\\Natural\\Vegetation\\Trees\\normal_trees\\x.cgf'), true);
  assert.equal(
    isTreeMesh('objects/natural/vegetation/trees/fallen_trees/tree_branch_a.cgf'),
    false,
  );
  assert.equal(isTreeMesh('objects/natural/vegetation/trees/stumps/stump_a.cgf'), false);
  assert.equal(isTreeMesh('objects/manmade/structures/barn.cgf'), false);
});

test('blows count with an axe, within reach and one per swing; playerTreeChop may refuse one', async (t) => {
  const f = await started(t);
  f.report(7, { hands: [AXE, ''], position: { x: 8.5, y: 20, z: 30 } });
  const player = f.host.players.get(7);
  const { Trees, Events } = f.api;
  Trees.configure({ hits: 3 });
  const blows = [];
  let refuse = false;
  Events.on('playerTreeChop', (who, tree, blow) => {
    blows.push([who.id, tree.id, blow.number, blow.of]);
    return !refuse;
  });
  swing(f, player, treeHit());
  f.host.worldTraces.weaponHit(player, treeHit()); // the same swing again: not counted
  swing(f, player, treeHit(9)); // out of reach
  f.report(7, { hands: [SWORD, ''], position: { x: 8.5, y: 20, z: 30 } });
  swing(f, player, treeHit()); // no axe
  f.report(7, { hands: [AXE, ''], position: { x: 8.5, y: 20, z: 30 } });
  refuse = true;
  swing(f, player, treeHit()); // refused by the handler
  refuse = false;
  swing(f, player, treeHit());
  const id = `${BIRCH}@10,20,30`;
  assert.deepEqual(blows, [
    [7, id, 1, 3],
    [7, id, 2, 3],
    [7, id, 2, 3],
  ]);
  const tree = Trees.get(id);
  assert.equal(tree.hits, 2);
  assert.equal(tree.health, 1);
  assert.equal(tree.state, 'standing');
  assert.deepEqual(f.treeEvents(), ['treeChop', 'treeChop']);
});

test('the last blow fells the tree: hidden, the same mesh falls away from the chopper and lies', async (t) => {
  const f = await started(t);
  t.mock.timers.enable({ apis: ['setTimeout', 'setInterval'] });
  f.report(7, { hands: [AXE, ''], position: { x: 8.5, y: 20, z: 30 } });
  const player = f.host.players.get(7);
  const { Trees, Events, LevelEdit, Prop } = f.api;
  Trees.configure({ hits: 2, height: 10 });
  const fallen = [];
  Events.on('playerTreeFell', (who, tree, fall) => {
    fallen.push([tree.id, Math.round(fall.direction.x), Math.round(fall.direction.y)]);
  });
  Events.on('treeFelled', (tree) => fallen.push(['felled', tree.state]));
  swing(f, player, treeHit());
  swing(f, player, treeHit());
  const id = `${BIRCH}@10,20,30`;
  assert.deepEqual(fallen[0], [id, 1, 0], 'away from the chopper, who stands west of it');
  assert.equal(Trees.get(id).state, 'falling');
  const [edit] = LevelEdit.all();
  assert.equal(edit.hidden, true);
  assert.equal(edit.name, BIRCH);
  const trunk = Prop.all().find((prop) => prop.model === BIRCH);
  assert.ok(trunk, 'the same mesh stands in as a prop');
  // The fall runs to its end with the bounce; the trunk lies about 90 degrees over, eastwards.
  for (let step = 0; step < 200; step += 1) {
    f.advance(33);
    t.mock.timers.tick(33);
  }
  assert.deepEqual(fallen[1], ['felled', 'felled']);
  const top = trunk.rotation.rotateVector({ x: 0, y: 0, z: 1 });
  assert.ok(top.x > 0.95 && Math.abs(top.z) < 0.1, `lying eastwards, top ${JSON.stringify(top)}`);
  assert.equal(trunk.physics, 'static');
  assert.deepEqual(f.treeEvents().slice(-2), ['treeFalling', 'treeFelled']);
});

test('the trunk is cut into items; playerTrunkCut may change them; then the tree regrows', async (t) => {
  const f = await started(t);
  t.mock.timers.enable({ apis: ['setTimeout', 'setInterval'] });
  f.report(7, { hands: [AXE, ''], position: { x: 8.5, y: 20, z: 30 } });
  const player = f.host.players.get(7);
  const { Trees, Events, LevelEdit, Prop } = f.api;
  Trees.configure({ hits: 1, trunkHits: 2, height: 10, regrowMinutes: 1, regrowClearance: 5 });
  const tree = Trees.of({ name: BIRCH, at: AT });
  assert.equal(tree.fell({ x: 1, y: 0, z: 0 }, player), true);
  for (let step = 0; step < 200; step += 1) {
    f.advance(33);
    t.mock.timers.tick(33);
  }
  assert.equal(tree.state, 'felled');
  Events.on('playerTrunkCut', (who, felled, items) => {
    items.push({ item: 'apple', amount: 1 });
  });
  const given = [];
  f.host.playerInventories.giveItem = (who, item, amount) => {
    given.push([who.id, item, amount]);
    return true;
  };
  // A blow on the lying trunk, 4 m along it eastwards.
  // The game names the hidden tree for a blow on its trunk (the trunk is the tree's own mesh), and
  // the trunk rests on its branches, a metre above the line from its foot.
  const onTrunk = { ...treeHit(), position: { x: 14, y: 20, z: 31.2 } };
  f.report(7, { hands: [AXE, ''], position: { x: 14, y: 21.5, z: 30 } });
  swing(f, player, onTrunk);
  swing(f, player, onTrunk);
  assert.equal(tree.state, 'cut');
  assert.deepEqual(given, [
    [7, 'bsmt_woodExotic', 2],
    [7, 'apple', 1],
  ]);
  assert.equal(
    Prop.all().some((prop) => prop.model === BIRCH),
    false,
    'the trunk is gone',
  );
  // Regrowth waits for the time and for nobody standing close.
  f.advance(61000);
  t.mock.timers.tick(1000);
  assert.equal(tree.state, 'cut', 'the chopper stands within the clearance');
  f.report(7, { hands: [AXE, ''], position: { x: 40, y: 20, z: 30 } });
  f.advance(1000);
  t.mock.timers.tick(1000);
  assert.equal(Trees.get(tree.id), null);
  assert.equal(LevelEdit.all().length, 0, 'the standing tree is back');
  assert.equal(f.treeEvents().at(-1), 'treeRegrown');
});

test('playerTreeFell may keep the tree standing; settings and species are checked', async (t) => {
  const f = await started(t);
  f.report(7, { hands: [AXE, ''], position: { x: 8.5, y: 20, z: 30 } });
  const player = f.host.players.get(7);
  const { Trees, Events } = f.api;
  Trees.configure({ hits: 1 });
  Events.on('playerTreeFell', () => false);
  swing(f, player, treeHit());
  const tree = Trees.get(`${BIRCH}@10,20,30`);
  assert.equal(tree.state, 'standing');
  assert.throws(() => Trees.configure({ hits: 0 }), /hits/);
  assert.throws(() => Trees.configure({ leaves: 3 }), /unknown setting/);
  assert.throws(
    () => Trees.configure({ drops: [{ item: 'nosuchthing', amount: 1 }] }),
    /item class/,
  );
  assert.throws(() => Trees.configure({ axes: ['nosuchaxe'] }), /item class/);
  assert.equal(
    Trees.setSpecies('objects/natural/vegetation/trees/normal_trees/betula', { hits: 2 }),
    true,
  );
  assert.equal(Trees.isTree(BIRCH), true);
  assert.equal(Trees.isTree('objects/kcds/my/oak.cgf'), false);
  Trees.setSpecies('objects/kcds/my/', { height: 12 });
  assert.equal(Trees.isTree('objects/kcds/my/oak.cgf'), true);
  assert.equal(Trees.of({ name: 'objects/manmade/barn.cgf', at: AT }), null);
});

test('Trees.state and Trees.restore keep felled and cut trees across a restart', async (t) => {
  const first = await started(t);
  t.mock.timers.enable({ apis: ['setTimeout', 'setInterval'] });
  first.report(7, { hands: [AXE, ''], position: { x: 8.5, y: 20, z: 30 } });
  const { Trees } = first.api;
  Trees.configure({ hits: 1, height: 10 });
  const felled = Trees.of({ name: BIRCH, at: AT });
  felled.fell({ x: 0, y: 1, z: 0 }, first.host.players.get(7));
  const other = Trees.of({ name: BIRCH, at: { x: 50, y: 20, z: 30 } });
  other.setHits(0);
  for (let step = 0; step < 200; step += 1) {
    first.advance(33);
    t.mock.timers.tick(33);
  }
  const kept = JSON.parse(JSON.stringify(Trees.state()));
  assert.equal(kept.length, 2);
  assert.deepEqual(kept.map((entry) => entry.state).sort(), ['felled', 'standing']);

  const second = await started(t);
  assert.equal(second.api.Trees.restore(kept), 2);
  const back = second.api.Trees.get(felled.id);
  assert.equal(back.state, 'felled');
  assert.equal(back.regrowAt, felled.regrowAt);
  assert.equal(second.api.LevelEdit.all().length, 1, 'the standing tree is hidden again');
  const trunk = second.api.Prop.all().find((prop) => prop.model === BIRCH);
  const top = trunk.rotation.rotateVector({ x: 0, y: 0, z: 1 });
  assert.ok(top.y > 0.95, 'the trunk lies northwards, as it lay');
  assert.equal(second.api.Trees.restore(kept), 0, 'a tree already touched is not restored twice');
  assert.throws(() => second.api.Trees.restore('nope'), TypeError);
});
