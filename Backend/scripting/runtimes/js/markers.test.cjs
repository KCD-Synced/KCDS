'use strict';

// Marker and Blip: the handles, who they are shown to, the ops sent to the clients,
// and the enter and exit events of trigger markers.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const GLOBAL_WORLD = 0xffffffff;

async function started(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const events = [];
  f.host.on('markerPlace', (marker) => events.push(['place', marker.id]));
  f.host.on('markerRemove', (marker) => events.push(['remove', marker.id]));
  f.host.on('markerEnter', (marker, player) => events.push(['enter', marker.id, player.id]));
  f.host.on('markerExit', (marker, player) => events.push(['exit', marker.id, player.id]));
  const ops = (...names) => f.sent.filter((message) => names.includes(message.op));
  // The next tick: the settling pass of changed markers runs in a microtask.
  const tick = () => new Promise((resolve) => setImmediate(resolve));
  const move = (id, x, y, z = 0, fields = {}) => f.report(id, { position: { x, y, z }, ...fields });
  return { ...f, events, ops, tick, move, Marker: f.host.api.Marker, Blip: f.host.api.Blip };
}

test('Marker.place makes a handle with the defaults and clamped fields', async (t) => {
  const { Marker, host, events } = await started(t);
  const marker = Marker.place(
    'materials/decals/burglar/chalk_cross',
    { x: 1, y: 2, z: 3 },
    {
      shape: 'cylinder',
      size: 500,
      height: 0,
      spin: -1000,
      bob: 9,
      color: 0xff0000ff,
    },
  );
  assert.equal(marker.exists, true);
  assert.equal(marker instanceof host.api.Entity, true);
  assert.equal(marker.material, 'materials/decals/burglar/chalk_cross');
  assert.equal(marker.group, 'burglar');
  assert.equal(marker.shape, 'cylinder');
  assert.equal(marker.size, 64);
  assert.equal(marker.height, 0.25);
  assert.equal(marker.spin, -720);
  assert.equal(marker.bob, 4);
  assert.equal(marker.color, 0xff0000ff);
  assert.equal(marker.trigger, false);
  assert.equal(marker.dimension, GLOBAL_WORLD);
  assert.deepEqual({ ...marker.position }, { x: 1, y: 2, z: 3 });
  assert.deepEqual(events, [['place', marker.id]]);
  assert.equal(Marker.place('materials/decals/x').shape, 'decal');
  // A small decal, such as a thrown thing's stain on a wall, down to 5 cm.
  assert.equal(Marker.place('materials/decals/x', {}, { size: 0.05 }).size, 0.05);
  assert.equal(Marker.place('materials/decals/x', {}, { size: 0.01 }).size, 0.05);
});

test('Marker.place rejects what it cannot draw', async (t) => {
  const { Marker } = await started(t);
  assert.throws(() => Marker.place(''), TypeError);
  assert.throws(() => Marker.place('m', { x: NaN }), TypeError);
  assert.throws(() => Marker.place('m', {}, { shape: 'torus' }), RangeError);
  assert.throws(() => Marker.place('m', {}, { trigger: 1 }), TypeError);
  assert.throws(() => Marker.place('m', {}, {}, -1), TypeError);
});

test('Marker lookups: getById, all by world, removeAll, and Entity', async (t) => {
  const { Marker, host, events } = await started(t);
  const a = Marker.place('m', {}, {}, 1);
  const b = Marker.place('m', {}, {}, 2);
  const c = Marker.place('m');
  assert.equal(Marker.getById(a.id), a);
  assert.equal(new Marker(b.id), b);
  assert.equal(new host.api.Entity(c.id), c);
  assert.equal(Marker.getById(1), null);
  assert.deepEqual(Marker.all(), [a, b, c]);
  assert.deepEqual(Marker.all(2), [b]);
  assert.equal(Marker.removeAll(1), 1);
  assert.equal(a.exists, false);
  assert.equal(Marker.removeAll(), 2);
  assert.deepEqual(Marker.all(), []);
  assert.deepEqual(
    events.filter((event) => event[0] === 'remove'),
    [
      ['remove', a.id],
      ['remove', b.id],
      ['remove', c.id],
    ],
  );
  assert.throws(() => {
    a.size = 3;
  }, /destroyed/);
  assert.equal(a.destroy(), undefined);
  assert.equal(a.remove(), undefined);
});

test('Marker.remove raises markerRemove once; Marker.list names the game decals', async (t) => {
  const { Marker, events } = await started(t);
  const marker = Marker.place('materials/decals/arrow');
  marker.remove();
  marker.remove();
  assert.deepEqual(events, [
    ['place', marker.id],
    ['remove', marker.id],
  ]);
  const all = Marker.list();
  assert.ok(all.length > 200);
  assert.ok(all.includes('materials/decals/arrow'));
  assert.ok(
    all.every((name) => name === name.toLowerCase() && name.startsWith('materials/decals/')),
  );
  const burglar = Marker.list('Materials/Decals/burglar');
  assert.ok(burglar.length > 0);
  assert.ok(burglar.every((name) => name.startsWith('materials/decals/burglar')));
  assert.deepEqual(Marker.list('nothing/'), []);
  assert.throws(() => Marker.list(3), TypeError);
});

test('A marker is sent to a ready player once, restated on change, and hidden when removed', async (t) => {
  const { Marker, report, ops, sent } = await started(t);
  report(7);
  report(9, { ready: false });
  const marker = Marker.place('m', { x: 5, y: 6, z: 0 }, { size: 4, color: 0x11223344 });
  assert.deepEqual(ops('markerShow'), [
    {
      op: 'markerShow',
      id: 7,
      marker: marker.id,
      material: 'm',
      shape: 'decal',
      position: { x: 5, y: 6, z: 0 },
      rotation: { x: 0, y: 0, z: 0, w: 1 },
      size: 4,
      height: 2,
      spin: 0,
      bob: 0,
      color: 0x11223344,
    },
  ]);
  const before = sent.length;
  marker.size = 4;
  marker.color = 0x11223344;
  assert.equal(sent.length, before);
  marker.spin = 90;
  assert.equal(ops('markerShow').length, 2);
  assert.equal(ops('markerShow')[1].spin, 90);
  marker.destroy();
  assert.deepEqual(ops('markerHide'), [{ op: 'markerHide', id: 7, marker: marker.id }]);
});

test('A marker follows the dimensions and the players it is limited to', async (t) => {
  const { Marker, host, report, ops } = await started(t);
  report(7);
  report(8);
  const marker = Marker.place('m', {}, {}, 5);
  assert.deepEqual(ops('markerShow'), []);
  report(7, { dimension: 5 });
  assert.deepEqual(
    ops('markerShow').map((message) => message.id),
    [7],
  );
  marker.setDimension(GLOBAL_WORLD);
  assert.deepEqual(
    ops('markerShow').map((message) => message.id),
    [7, 8],
  );
  marker.showOnlyTo(host.players.get(8));
  assert.deepEqual(
    ops('markerHide').map((message) => message.id),
    [7],
  );
  marker.showOnlyTo(null);
  assert.equal(ops('markerShow').length, 3);
  assert.throws(() => marker.showOnlyTo({ id: 99 }), TypeError);
  // Moving to the other world takes it off the screen of the one who stayed behind.
  report(8, { dimension: 5 });
  report(8, { dimension: 6 });
  marker.setDimension(5);
  assert.deepEqual(
    ops('markerHide').map((message) => message.id),
    [7, 8],
  );
});

test('A player who arrives gets every marker again; one who left is forgotten', async (t) => {
  const { Marker, host, report, ops } = await started(t);
  Marker.place('a');
  Marker.place('b');
  report(7, { ready: false });
  assert.deepEqual(ops('markerShow'), []);
  report(7);
  assert.equal(ops('markerShow').length, 2);
  // Their world got ready again: their game lost everything, so it all goes again.
  report(7, { ready: false });
  report(7);
  assert.equal(ops('markerShow').length, 4);
  host.disconnect(7);
  Marker.place('c');
  assert.equal(ops('markerShow').length, 4);
});

test('A trigger marker raises enter and exit when a player walks in and out', async (t) => {
  const { Marker, report, move, events, tick } = await started(t);
  report(7);
  const marker = Marker.place('m', { x: 100, y: 100, z: 0 }, { size: 10, trigger: true });
  await tick();
  assert.deepEqual(events, [['place', marker.id]]);
  move(7, 90, 100);
  move(7, 96, 100);
  move(7, 97, 100);
  move(7, 100, 100);
  move(7, 104, 100);
  move(7, 120, 100);
  move(7, 100, 100, 50);
  assert.deepEqual(events.slice(1), [
    ['enter', marker.id, 7],
    ['exit', marker.id, 7],
  ]);
});

test('A marker placed on a player, moved onto them or switched to trigger raises enter after the call', async (t) => {
  const { Marker, move, events, tick } = await started(t);
  move(7, 0, 0);
  const marker = Marker.place('m', { x: 0, y: 0, z: 0 }, { size: 4, trigger: true });
  assert.deepEqual(events, [['place', marker.id]]);
  await tick();
  assert.deepEqual(events.slice(1), [['enter', marker.id, 7]]);
  marker.position = { x: 50, y: 0, z: 0 };
  await tick();
  assert.deepEqual(events.slice(2), [['exit', marker.id, 7]]);
  marker.position = { x: 0, y: 0, z: 0 };
  marker.trigger = false;
  await tick();
  assert.equal(events.length, 3);
  marker.trigger = true;
  await tick();
  assert.deepEqual(events.slice(3), [['enter', marker.id, 7]]);
});

test('Turning the trigger off, removing the marker and leaving raise no exit', async (t) => {
  const { Marker, host, move, events, tick } = await started(t);
  move(7, 0, 0);
  move(8, 0, 0);
  const off = Marker.place('m', {}, { size: 4, trigger: true });
  const removed = Marker.place('m', {}, { size: 4, trigger: true });
  await tick();
  assert.equal(events.filter((event) => event[0] === 'enter').length, 4);
  off.trigger = false;
  removed.destroy();
  host.disconnect(7);
  await tick();
  assert.equal(events.filter((event) => event[0] === 'exit').length, 0);
});

test('A trigger marker counts only players it is meant for, in their world', async (t) => {
  const { Marker, host, move, events, tick } = await started(t);
  move(7, 0, 0);
  move(8, 0, 0, 0, { dimension: 4 });
  const marker = Marker.place('m', {}, { size: 4, trigger: true }, 4);
  await tick();
  assert.deepEqual(events.slice(1), [['enter', marker.id, 8]]);
  marker.showOnlyTo(host.players.get(7));
  await tick();
  assert.deepEqual(events.slice(2), [['exit', marker.id, 8]]);
  marker.setDimension(GLOBAL_WORLD);
  await tick();
  assert.deepEqual(events.slice(3), [['enter', marker.id, 7]]);
  // A player who changes world is checked at once.
  move(7, 0, 0, 0, { dimension: 9 });
  assert.deepEqual(events.slice(4), []);
  move(8, 0, 0, 0, { dimension: 9 });
  marker.showOnlyTo(null);
  await tick();
  assert.deepEqual(events.slice(4), [['enter', marker.id, 8]]);
});

test('Trigger volumes: a ball for a sphere, a cylinder with its height for the rest', async (t) => {
  const { Marker, move, events, tick } = await started(t);
  const ball = Marker.place(
    'm',
    { x: 0, y: 0, z: 10 },
    { shape: 'sphere', size: 6, trigger: true },
  );
  const tall = Marker.place(
    'm',
    { x: 100, y: 0, z: 0 },
    { shape: 'cube', size: 6, height: 10, trigger: true },
  );
  await tick();
  move(7, 0, 0, 14);
  move(7, 0, 0, 12);
  move(7, 100, 0, 9);
  move(7, 100, 0, 11);
  assert.deepEqual(events.slice(2), [
    ['enter', ball.id, 7],
    ['exit', ball.id, 7],
    ['enter', tall.id, 7],
    ['exit', tall.id, 7],
  ]);
});

test('With no trigger marker a moving player costs the markers nothing', async (t) => {
  const { Marker, host, move } = await started(t);
  Marker.place('m', {}, { size: 4 });
  move(7, 0, 0);
  move(7, 1, 0);
  assert.equal(host.markers.bodies.get(7).markers.size, 0);
  assert.equal(host.markers.triggers.size, 0);
  assert.equal(host.markers.grid.cells.size, 0);
});

test('Blip.create makes a blip with the defaults, and Blip.types lists the icons', async (t) => {
  const { Blip, host, report, ops } = await started(t);
  report(7);
  const blip = Blip.create({ position: { x: 1, y: 2, z: 3 } });
  assert.equal(blip.exists, true);
  assert.equal(blip instanceof host.api.Entity, true);
  assert.equal(blip.type, 'GeneralPoi');
  assert.equal(blip.markState, 0);
  assert.equal(blip.label, '');
  assert.equal(blip.compass, true);
  assert.equal(blip.map, true);
  assert.equal(blip.dimension, GLOBAL_WORLD);
  assert.deepEqual(ops('blipShow'), [
    {
      op: 'blipShow',
      id: 7,
      blip: blip.id,
      type: 'GeneralPoi',
      markState: 0,
      label: '',
      compass: true,
      map: true,
      position: { x: 1, y: 2, z: 3 },
    },
  ]);
  const types = Blip.types();
  assert.equal(types.includes('Checkpoint'), true);
  assert.equal(types.includes('MineEnrtrance'), true);
  assert.throws(() => Blip.create({ position: { x: 0, y: 0, z: 0 }, type: 'Nope' }), RangeError);
  assert.throws(() => Blip.create({}), TypeError);
  assert.throws(() => Blip.create({ position: { x: 0, y: 0, z: 0 }, compass: 'yes' }), TypeError);
});

test('Blip changes restate it once, and an unknown type leaves it as it was', async (t) => {
  const { Blip, report, ops, sent } = await started(t);
  report(7);
  const blip = Blip.create({ position: { x: 0, y: 0, z: 0 }, label: 'a'.repeat(100) });
  assert.equal(blip.label.length, 64);
  const shown = () => ops('blipShow').length;
  const before = shown();
  blip.type = 'Camp';
  blip.type = 'Camp';
  blip.type = 'Nope';
  assert.equal(blip.type, 'Camp');
  assert.equal(shown(), before + 1);
  blip.label = 'Camp';
  blip.markState = 3;
  blip.map = false;
  blip.position = { x: 9, y: 9, z: 9 };
  assert.equal(shown(), before + 5);
  const last = ops('blipShow').pop();
  assert.equal(last.label, 'Camp');
  assert.equal(last.markState, 3);
  assert.equal(last.map, false);
  assert.deepEqual(last.position, { x: 9, y: 9, z: 9 });
  assert.equal(sent.filter((message) => message.op === 'blipHide').length, 0);
});

test('A blip can be private to a player or to chosen players, and follows worlds', async (t) => {
  const { Blip, host, report, ops } = await started(t);
  report(7);
  report(8);
  report(9);
  const blip = Blip.create({
    position: { x: 0, y: 0, z: 0 },
    player: host.players.get(7),
    dimension: 3,
  });
  assert.deepEqual(ops('blipShow'), []);
  report(7, { dimension: 3 });
  report(8, { dimension: 3 });
  assert.deepEqual(
    ops('blipShow').map((message) => message.id),
    [7],
  );
  blip.showOnlyTo([host.players.get(7), host.players.get(8)]);
  assert.deepEqual(
    ops('blipShow').map((message) => message.id),
    [7, 8],
  );
  blip.showOnlyTo(null);
  report(7, { dimension: 4 });
  assert.deepEqual(
    ops('blipHide').map((message) => message.id),
    [7],
  );
  assert.throws(() => Blip.create({ position: {}, player: { id: 99 } }), TypeError);
});

test('Blip lookups: getById, all by world, removeAll, destroy hides it', async (t) => {
  const { Blip, host, report, ops } = await started(t);
  report(7);
  const a = Blip.create({ position: { x: 0, y: 0, z: 0 }, dimension: 1 });
  const b = Blip.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(Blip.getById(b.id), b);
  assert.equal(new host.api.Entity(a.id), a);
  assert.deepEqual(Blip.all(1), [a]);
  assert.equal(Blip.removeAll(1), 1);
  b.remove();
  b.remove();
  b.destroy();
  assert.deepEqual(
    ops('blipHide').map((message) => message.blip),
    [b.id],
  );
  assert.equal(Blip.getById(b.id), null);
  assert.equal(Blip.removeAll(), 0);
  assert.throws(() => new Blip(b.id), /Unknown blip/);
});

test('Blip image (KCDS): only a .dds a resource ships to clients; none sends no image', async (t) => {
  const { Blip, report, ops } = await started(t);
  report(7);
  assert.throws(
    () => Blip.create({ position: { x: 0, y: 0, z: 0 }, image: 'images/logo.dds' }),
    /Blip image/,
  );
  const blip = Blip.create({ position: { x: 0, y: 0, z: 0 } });
  assert.equal(blip.image, '');
  assert.equal('image' in ops('blipShow').at(-1), false);
  blip.image = '';
  assert.throws(() => (blip.image = 5), /Blip image/);
});
