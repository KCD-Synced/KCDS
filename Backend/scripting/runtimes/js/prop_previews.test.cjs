'use strict';

// PropGhost and PropPlacer: previews shown to one player.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const BARREL = 'objects/manmade/common_furniture/barrels/barrel_a.cgf';
const TURN = { x: 0, y: 0, z: 0, w: 1 };
const ORIGIN = { x: 0, y: 0, z: 0 };

async function setup(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const api = f.host.makeAPI();
  const player = f.report(7);
  const other = f.report(8);
  const ops = (name) => f.sent.filter((m) => m.op === name);
  return { f, api, player, other, ops, previews: f.host.propPreviews };
}

function pose(x, extra = {}) {
  return {
    position: { x, y: 0, z: 0 },
    rotation: TURN,
    normal: { x: 0, y: 0, z: 1 },
    surface: 'mat_dirt',
    distance: 3,
    onSurface: true,
    ...extra,
  };
}

// a ghost scaled per axis, as Prop.spawn is.
test('a ghost takes its scale per axis as a Vector3', async (t) => {
  const { api, player, ops } = await setup(t);
  const ghost = api.PropGhost.create(player, {
    model: 'barrel_a',
    position: { x: 1, y: 2, z: 3 },
    scale: new api.Vector3(1.9, 1.5, 0.7),
  });
  const [created] = ops('ghostCreate');
  assert.equal(created.scale, 1);
  assert.deepEqual(created.scaleAxes, { x: 1.9, y: 1.5, z: 0.7 });
  assert.ok(ghost.scale instanceof api.Vector3);
  assert.equal(ghost.scale.y, 1.5);
  ghost.setPose(undefined, undefined, new api.Vector3(1.9, 1.5, 0.7));
  assert.equal(ops('ghostPose').length, 0);
  ghost.setPose(undefined, undefined, { x: 2, y: 500, z: 1 });
  assert.deepEqual(ops('ghostPose').at(-1).scaleAxes, { x: 2, y: 100, z: 1 });
  ghost.setPose(undefined, undefined, 3);
  assert.equal(ops('ghostPose').at(-1).scale, 3);
  assert.equal(ops('ghostPose').at(-1).scaleAxes, undefined);
  assert.throws(() => ghost.setPose(undefined, undefined, { x: 1, y: 'a', z: 1 }), TypeError);
});

test('a ghost is created for one player and changes are sent once', async (t) => {
  const { api, player, ops } = await setup(t);
  const ghost = api.PropGhost.create(player, {
    model: 'barrel_a',
    position: { x: 1, y: 2, z: 3 },
    scale: 500,
  });
  const [created] = ops('ghostCreate');
  assert.equal(created.id, 7);
  assert.deepEqual(created.models, [BARREL]);
  assert.equal(created.scale, 100);
  assert.equal(created.tint, null);
  assert.equal(ghost.valid, true);
  assert.equal(api.PropGhost.all(player).length, 1);

  assert.equal(ghost.setPose({ x: 1, y: 2, z: 3 }), true);
  assert.equal(ops('ghostPose').length, 0);
  ghost.setPose({ x: 4, y: 2, z: 3 }, { x: 0, y: 0, z: 1, w: 0 });
  assert.equal(ops('ghostPose').length, 1);
  assert.equal(ghost.position.x, 4);

  ghost.setValid(true);
  ghost.setValid(true);
  ghost.setValid(false);
  assert.deepEqual(
    ops('ghostTint').map((m) => m.tint.g),
    [1, 0],
  );
  ghost.setTint(null);
  assert.equal(ops('ghostTint').at(-1).tint, null);

  ghost.setHidden(true);
  ghost.setHidden(true);
  assert.equal(ops('ghostHidden').length, 1);

  assert.equal(ghost.destroy(), true);
  assert.equal(ghost.destroy(), false);
  assert.equal(ghost.setHidden(false), false);
  assert.equal(ops('ghostDestroy').length, 1);
});

test('a ghost refuses a mesh the catalog lacks and a player without a body', async (t) => {
  const { f, api, player } = await setup(t);
  assert.throws(() => api.PropGhost.create(player, { model: 'no_such_mesh_xyz' }), RangeError);
  assert.throws(
    () => api.PropGhost.create(player, { model: 'barrel_a', position: { x: NaN, y: 0, z: 0 } }),
    RangeError,
  );
  const gone = f.report(9, { ready: false });
  assert.throws(() => api.PropGhost.create(gone, { model: 'barrel_a', position: ORIGIN }));
});

test('a player leaving drops their ghosts and session without ops', async (t) => {
  const { f, api, player, other, ops, previews } = await setup(t);
  api.PropGhost.create(player, { model: 'barrel_a', position: ORIGIN });
  api.PropGhost.create(other, { model: 'barrel_a', position: ORIGIN });
  api.PropPlacer.begin(player, { model: 'barrel_a' });
  f.sent.length = 0;
  previews.left(player);
  assert.equal(api.PropGhost.all(player).length, 0);
  assert.equal(api.PropGhost.all(other).length, 1);
  assert.equal(api.PropPlacer.get(player), null);
  assert.equal(f.sent.length, 0);
  assert.equal(ops('ghostDestroy').length, 0);
});

test('placer begin sends only what was asked, one session per player', async (t) => {
  const { api, player, ops } = await setup(t);
  const session = api.PropPlacer.begin(player, {
    model: 'barrel_a',
    range: 9999,
    keepOpen: false,
    rules: { maxSlope: 20, surfaces: ['mat_dirt'] },
  });
  const [begin] = ops('placerBegin');
  assert.equal(begin.session, session.id);
  assert.deepEqual(begin.models, [BARREL]);
  assert.equal(begin.range, 512);
  assert.equal(begin.keepOpen, false);
  assert.equal('yaw' in begin, false);
  assert.deepEqual(begin.rules, { maxSlope: 20, surfaces: ['mat_dirt'] });
  assert.equal(api.PropPlacer.begin(player, { model: 'barrel_a' }), null);
  assert.equal(api.PropPlacer.get(player), session);

  assert.equal(session.veto(), true);
  assert.equal(ops('placerVeto')[0].vetoed, true);
  assert.equal(session.end(), true);
  assert.equal(session.end(), false);
  assert.equal(ops('placerEnd').length, 1);
  assert.equal(session.active, false);
  assert.throws(
    () => api.PropPlacer.begin(player, { model: 'barrel_a', rules: { maxDistance: -1 } }),
    RangeError,
  );
});

test('a confirm reaches the handlers with the final pose and builds a prop', async (t) => {
  const { f, api, player, previews } = await setup(t);
  const seen = [];
  f.host.handlers.set('propPlaceConfirm', [
    {
      active: true,
      owner: undefined,
      resource: 'test',
      fn: (who, session, placement) => seen.push([who.id, session.id, placement]),
    },
  ]);
  const session = api.PropPlacer.begin(player, {
    model: 'barrel_a',
    scale: 2,
    spawn: { physics: 'rigid' },
  });
  previews.report(player, { session: session.id, event: 'confirm', pose: pose(5) });
  assert.equal(seen.length, 1);
  assert.equal(seen[0][2].position.x, 5);
  assert.equal(seen[0][2].surface, 'mat_dirt');
  const [prop] = api.Prop.all();
  assert.equal(prop.model, BARREL);
  assert.equal(prop.physics, 'rigid');
  assert.equal(prop.scale, 2);
  assert.equal(prop.position.x, 5);
  // The session keeps running until it ends.
  assert.equal(session.active, true);
});

test('a confirm is refused by a handler, by the distance rule or by a bad pose', async (t) => {
  const { f, api, player, previews } = await setup(t);
  const refused = [];
  let allow = false;
  f.host.handlers.set('propPlaceConfirm', [{ active: true, resource: 'test', fn: () => allow }]);
  f.host.handlers.set('propPlaceRejected', [
    { active: true, resource: 'test', fn: (who, s, placement) => refused.push(placement) },
  ]);
  const session = api.PropPlacer.begin(player, {
    model: 'barrel_a',
    spawn: {},
    rules: { maxDistance: 10 },
  });
  const confirm = (p) =>
    previews.report(player, { session: session.id, event: 'confirm', pose: p });
  confirm(pose(5));
  confirm(pose(50));
  confirm({ ...pose(1), position: { x: Infinity, y: 0, z: 0 } });
  assert.equal(refused.length, 3);
  assert.equal(refused[2], null);
  assert.equal(api.Prop.all().length, 0);
  allow = true;
  confirm(pose(5));
  assert.equal(api.Prop.all().length, 1);
});

test('cancel, move and end reports follow the session and stale ones are ignored', async (t) => {
  const { f, api, player, other, ops, previews } = await setup(t);
  const log = [];
  for (const name of ['propPlaceCancel', 'propPlaceMove', 'propPlaceEnd']) {
    f.host.handlers.set(name, [{ active: true, resource: 'test', fn: () => log.push(name) }]);
  }
  const quiet = api.PropPlacer.begin(player, { model: 'barrel_a' });
  previews.report(player, { session: quiet.id, event: 'move', pose: pose(1), valid: true });
  previews.report(player, { session: quiet.id + 99, event: 'cancel' });
  previews.report(other, { session: quiet.id, event: 'cancel' });
  assert.deepEqual(log, []);
  previews.report(player, { session: quiet.id, event: 'cancel' });
  previews.report(player, { session: quiet.id, event: 'end' });
  assert.deepEqual(log, ['propPlaceCancel', 'propPlaceEnd']);
  assert.equal(quiet.active, false);
  // The game ended it itself, so there is no placerEnd to send.
  assert.equal(ops('placerEnd').length, 0);

  const live = api.PropPlacer.begin(player, { model: 'barrel_a', reportMoves: true });
  previews.report(player, { session: live.id, event: 'move', pose: pose(1), valid: true });
  assert.equal(log.at(-1), 'propPlaceMove');
});

test('a stopped resource takes its ghosts and session with it', async (t) => {
  const { f, api, player, ops, previews } = await setup(t);
  const owner = { name: 'builder' };
  f.host.context.run(owner, () => {
    api.PropGhost.create(player, { model: 'barrel_a', position: ORIGIN });
    api.PropPlacer.begin(player, { model: 'barrel_a' });
  });
  api.PropGhost.create(player, { model: 'barrel_a', position: ORIGIN });
  previews.resourceStopped('builder');
  assert.equal(api.PropGhost.all(player).length, 1);
  assert.equal(api.PropPlacer.get(player), null);
  assert.equal(ops('ghostDestroy').length, 1);
  assert.equal(ops('placerEnd').length, 1);
});
