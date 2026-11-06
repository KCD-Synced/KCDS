'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// What a lethal blow does to a horse a resource spawned: kill it (the game's default)
// or only knock it out. A KCDS addition.

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-horse-knockout-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 500, count: 1025 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const sent = (op) => output.filter((message) => message.op === op);
  return { Horse: host.api.Horse, sent };
}

test('a horse spawned without options is killed by a lethal blow', async () => {
  const { Horse, sent } = await startedHost();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  assert.equal(horse.knockOut, false);
  assert.equal(sent('horseSpawn')[0].knockOut, false);
  const other = Horse.spawn({ x: 1, y: 2, z: 3 }, {}, '', '', undefined, {});
  assert.equal(other.knockOut, false);
  assert.equal(sent('horseSpawn')[1].knockOut, false);
});

test('Horse.spawn with knockOut true sends it with the spawn', async () => {
  const { Horse, sent } = await startedHost();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 }, {}, 'soul', 'Bay', undefined, { knockOut: true });
  assert.equal(horse.knockOut, true);
  assert.equal(horse.name, 'Bay');
  assert.equal(sent('horseSpawn')[0].knockOut, true);
  const plain = Horse.spawn({ x: 0, y: 0, z: 0 }, {}, '', '', undefined, { knockOut: false });
  assert.equal(plain.knockOut, false);
  assert.equal(sent('horseSpawn')[1].knockOut, false);
});

test('the knockOut setter changes the choice and sends horseKnockOut', async () => {
  const { Horse, sent } = await startedHost();
  const horse = Horse.spawn({ x: 0, y: 0, z: 0 });
  horse.knockOut = true;
  assert.equal(horse.knockOut, true);
  assert.deepEqual(
    { id: sent('horseKnockOut')[0].id, knockOut: sent('horseKnockOut')[0].knockOut },
    { id: horse.id, knockOut: true },
  );
  horse.knockOut = false;
  assert.equal(horse.knockOut, false);
  assert.equal(sent('horseKnockOut').at(-1).knockOut, false);
  assert.equal(sent('horseKnockOut').length, 2);
  // A horse that is gone sends nothing more.
  horse.destroy();
  horse.knockOut = true;
  assert.equal(sent('horseKnockOut').length, 2);
});

test('knockOut refuses values that are not booleans', async () => {
  const { Horse, sent } = await startedHost();
  const spawn = (options) => () =>
    Horse.spawn({ x: 0, y: 0, z: 0 }, {}, '', '', undefined, options);
  assert.throws(spawn({ knockOut: 1 }), TypeError);
  assert.throws(spawn({ knockOut: 'true' }), TypeError);
  assert.throws(spawn({ knockOut: null }), TypeError);
  assert.throws(spawn(null), TypeError);
  assert.throws(spawn(true), TypeError);
  assert.equal(sent('horseSpawn').length, 0);
  const horse = Horse.spawn({ x: 0, y: 0, z: 0 });
  for (const value of [1, 0, 'true', null, undefined, {}]) {
    assert.throws(() => (horse.knockOut = value), TypeError);
  }
  assert.equal(horse.knockOut, false);
  assert.equal(sent('horseKnockOut').length, 0);
});
