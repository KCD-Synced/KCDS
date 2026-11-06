'use strict';

// LevelEdit and WorldResource.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

const BRUSH = 'objects/buildings/wall_a.cgf';
const GUID = '0123456789abcdef';

function hostWith(worldResources = []) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-level-'));
  const output = [];
  const host = new ResourceHost(root, {
    send: (e) => output.push(e),
    log: () => {},
    worldResources,
  });
  const ops = (name) => output.filter((e) => e.op === name);
  return { host, root, output, ops };
}

test('LevelEdit.apply hides and moves, and restore gives the object back', () => {
  const { host, ops } = hostWith();
  const { LevelEdit } = host.api;
  const edit = LevelEdit.apply({ name: BRUSH, at: [1, 2, 3], hidden: true });
  assert.equal(edit.kind, 'brush');
  assert.equal(edit.hidden, true);
  assert.equal(edit.moved, false);
  assert.equal(edit.exists, true);
  assert.equal(ops('levelEditApply').length, 1);
  assert.equal(ops('levelEditApply')[0].dimension, 0xffffffff);

  assert.equal(edit.move({ x: 5, y: 5, z: 5 }, [0, 0, 0, 1], 2), true);
  assert.equal(edit.moved, true);
  assert.deepEqual(edit.toJSON(), {
    kind: 'brush',
    name: BRUSH,
    at: [1, 2, 3],
    hidden: true,
    position: [5, 5, 5],
    rotation: [0, 0, 0, 1],
    scale: [2, 2, 2],
  });
  // Nothing is sent while nothing changes.
  const sent = ops('levelEditApply').length;
  edit.hidden = true;
  assert.equal(ops('levelEditApply').length, sent);
  assert.equal(edit.move({ x: NaN, y: 0, z: 0 }), false);

  edit.clearMove();
  assert.equal(edit.moved, false);
  assert.equal(edit.hidden, true);

  edit.restore();
  assert.equal(edit.exists, false);
  assert.equal(edit.toJSON(), null);
  assert.equal(ops('levelEditRemove')[0].id, edit.id);
  assert.equal(LevelEdit.getById(edit.id), null);
});

test('applying the same object twice restates the edit, per world', () => {
  const { host } = hostWith();
  const { LevelEdit } = host.api;
  const first = LevelEdit.apply({ name: BRUSH, at: [1, 2, 3] });
  // Within 5 cm of the same spot it is the same brush.
  const again = LevelEdit.apply({ name: BRUSH, at: [1.01, 2, 3], hidden: true });
  assert.equal(again, first);
  assert.equal(first.hidden, true);
  const other = LevelEdit.apply({ name: BRUSH, at: [1, 2, 3] }, 4);
  assert.notEqual(other, first);
  assert.equal(LevelEdit.all().length, 2);
  assert.equal(LevelEdit.all(4).length, 1);
  assert.equal(LevelEdit.restoreAll(4), 1);
  assert.equal(LevelEdit.restoreAll(), 1);
  assert.equal(LevelEdit.all().length, 0);
});

test('LevelEdit.apply refuses a definition no client could find the object by', () => {
  const { host } = hostWith();
  const { LevelEdit } = host.api;
  assert.throws(() => LevelEdit.apply({ at: [0, 0, 0] }), TypeError);
  assert.throws(() => LevelEdit.apply({ kind: 'entity', guid: 'xyz', at: [0, 0, 0] }), TypeError);
  assert.throws(() => LevelEdit.apply({ name: BRUSH, at: [0, 0] }), TypeError);
  const entity = LevelEdit.apply({ kind: 'entity', guid: GUID.toUpperCase(), at: [0, 0, 0] });
  assert.equal(entity.guid, GUID);
  assert.equal(entity.kind, 'entity');
});

test('a player arriving is told of the edits again', () => {
  const { host, ops } = hostWith();
  host.api.LevelEdit.apply({ name: BRUSH, at: [0, 0, 0], hidden: true });
  host.levelEdits.arrived({ id: 9 });
  assert.equal(ops('levelEditApply').at(-1).to, 9);
});

test('a world export loads as one unit and unloads at shutdown', async () => {
  const { host, root, output, ops } = hostWith();
  const file = path.join(root, 'village.world.json');
  fs.writeFileSync(
    file,
    JSON.stringify({
      level: 'rataje',
      props: [{ model: 'barrel_a', position: { x: 1, y: 2, z: 3 }, physics: 'rigid' }],
      effects: [{ effect: 'Fire.small.flame', position: { x: 0, y: 0, z: 0 } }],
      world: [{ name: BRUSH, at: [1, 2, 3], hidden: true }],
      items: [
        { item: 'arrow_crude', position: { x: 2, y: 2, z: 3 }, amount: 3, name: 'harvest' },
        { item: 'bread', position: { x: 3, y: 2, z: 3 } },
      ],
      doors: [
        { model: 'lvl1_door_a_left', position: { x: 6, y: 2, z: 3 }, name: 'barn', locked: true },
        { model: 'lvl1_door_a_left', position: [9, 2, 3], dimension: 5 },
      ],
      smartObjects: [
        { blueprint: 'Bench/bench_2places_low', position: { x: 4, y: 4, z: 3 }, name: 'bench' },
        { blueprint: 'Table/table_dice', position: { x: 8, y: 4, z: 3 }, dimension: 5 },
      ],
      areas: [{ id: 'square', type: 'sphere', position: { x: 0, y: 0, z: 0 }, radius: 5 }],
      patrolRoutes: [
        {
          id: 'watch',
          points: [{ position: { x: 0, y: 0, z: 0 } }, { position: { x: 5, y: 0, z: 0 } }],
        },
      ],
    }),
  );
  host.bridge.worldResources = [file];
  host.api.Events.on('worldResourcesReady', () => output.push({ op: 'test-ready' }));
  assert.equal(host.api.WorldResource.ready, false);
  await host.startAll();
  const { WorldResource, Prop } = host.api;
  assert.equal(WorldResource.ready, true);
  const village = WorldResource.find('village');
  assert.equal(WorldResource.all().length, 1);
  assert.equal(village.loaded, true);
  assert.equal(village.level, 'rataje');
  assert.equal(village.path, file);
  assert.equal(village.props.length, 1);
  assert.equal(village.effects.length, 1);
  assert.equal(village.levelEdits.length, 1);
  assert.equal(village.areas.length, 1);
  assert.equal(village.patrolRoutes.length, 1);
  // The stacks the export lays, named as it names them.
  assert.deepEqual(
    village.items.map((stack) => [stack.amount, stack.name]),
    [
      [3, 'harvest'],
      [1, ''],
    ],
  );
  village.items[1].destroy();
  assert.equal(village.items.length, 1);
  // The doors it hangs, as Door.spawn hangs them; the server owns them.
  assert.deepEqual(
    village.doors.map((door) => [door.name, door.locked, door.dimension, door.spawned]),
    [
      ['barn', true, 0xffffffff, true],
      ['spawned_door_2', false, 5, true],
    ],
  );
  assert.equal(ops('doorSpawn').length, 2);
  village.doors[1].destroy();
  assert.equal(village.doors.length, 1);
  // The seats and tables it places, as SmartObject.spawn places them.
  assert.deepEqual(
    village.smartObjects.map((object) => [object.kind, object.name, object.dimension]),
    [
      ['seat', 'bench', 0xffffffff],
      ['dice', '', 5],
    ],
  );
  village.smartObjects[0].destroy();
  assert.equal(village.smartObjects.length, 1);
  assert.equal(ops('propSpawn').length, 1);
  assert.equal(ops('levelEditApply').length, 1);
  assert.equal(WorldResource.find('missing'), null);

  // What a script destroys drops out of the lists.
  village.props[0].destroy();
  assert.equal(village.props.length, 0);
  assert.equal(Prop.all().length, 0);

  await host.shutdown();
  assert.equal(village.loaded, false);
  assert.equal(village.name, '');
  assert.equal(host.api.LevelEdit.all().length, 0);
  assert.equal(host.api.Door.all().length, 0);
  assert.equal(ops('doorDespawn').length, 2);
  assert.equal(host.api.SmartObject.all().length, 0);
  assert.equal(WorldResource.all().length, 0);
});

test('a bad export adds nothing and keeps the server from being ready', async () => {
  const { host, root, ops } = hostWith();
  const file = path.join(root, 'broken.world.json');
  fs.writeFileSync(
    file,
    JSON.stringify({
      props: [{ model: 'barrel_a', position: { x: 0, y: 0, z: 0 } }],
      world: [{ at: [0, 0, 0] }],
    }),
  );
  host.bridge.worldResources = [file, path.join(root, 'absent.world.json')];
  await host.startAll();
  assert.equal(host.api.WorldResource.ready, false);
  assert.equal(host.api.WorldResource.all().length, 0);
  assert.equal(host.api.Prop.all().length, 0);
  assert.equal(ops('propDestroy').length, 1);
});

// LevelEdit model: another mesh stands in the object's place.
test('a model swaps the object for a static stand-in, which follows a move and goes with the edit', () => {
  const { host, ops } = hostWith();
  const { LevelEdit, Prop } = host.api;
  const edit = LevelEdit.apply({ name: BRUSH, at: [1, 2, 3], model: 'stump_fagus_sylvatica_a' });
  const stump =
    'objects/manmade/task_specific_props/wood_industry/loggingcamp/stump_fagus_sylvatica_a.cgf';
  assert.equal(edit.model, stump);
  // Every client takes the object out; the stump stands where it stood.
  assert.equal(ops('levelEditApply').at(-1).hidden, true);
  assert.equal(edit.hidden, false, 'the edit itself does not hide it');
  const [standIn] = Prop.all();
  assert.equal(standIn.model, stump);
  assert.deepEqual(standIn.position.toJSON(), { x: 1, y: 2, z: 3 });
  assert.equal(standIn.physics, 'static');
  assert.equal(edit.toJSON().model, stump);
  assert.match(String(edit), /swapped/);
  // A move takes the stand-in along.
  edit.move({ x: 4, y: 5, z: 6 });
  assert.equal(Prop.all().length, 1);
  assert.deepEqual(Prop.all()[0].position.toJSON(), { x: 4, y: 5, z: 6 });
  // Clearing the model gives the object back.
  edit.model = '';
  assert.equal(Prop.all().length, 0);
  assert.equal(ops('levelEditApply').at(-1).hidden, false);
  assert.throws(() => {
    edit.model = 'objects/not/a/mesh.cgf';
  }, TypeError);
  edit.model = stump;
  assert.equal(Prop.all().length, 1);
  edit.restore();
  assert.equal(Prop.all().length, 0);
  assert.throws(() => LevelEdit.apply({ name: BRUSH, at: [0, 0, 0], model: 'nope' }), TypeError);
});
