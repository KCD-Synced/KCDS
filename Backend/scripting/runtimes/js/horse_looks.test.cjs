'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { HorseLooks, randomLook, lookOf, lookField } = require('./horses/looks.cjs');

// A horse's look: the server chooses it, so every client builds the same horse.

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-horse-looks-'));
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

const PARTS = ['body', 'head', 'hair', 'tail'];

test('a spawned horse gets a common look and sends it with the spawn', async () => {
  const { Horse, sent } = await startedHost();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  const look = horse.look;
  for (const part of PARTS) {
    assert.ok(HorseLooks.common(part).includes(look[part]), `${part} ${look[part]}`);
  }
  assert.equal(sent('horseSpawn')[0].look, lookField(look));
  assert.equal(lookField(look).split('|').length, 4);
});

test('setLook changes the parts asked for and refuses names the catalog does not have', async () => {
  const { Horse, sent } = await startedHost();
  const horse = Horse.spawn({ x: 0, y: 0, z: 0 });
  const before = horse.look;
  assert.equal(horse.setLook({ body: 'Horse_Solid_Black_m02', head: 'War_horse' }), true);
  assert.deepEqual(horse.look, { ...before, body: 'Horse_Solid_Black_m02', head: 'War_horse' });
  assert.equal(sent('horseLook').at(-1).look, lookField(horse.look));
  assert.equal(horse.setLook({ body: 'Unicorn' }), false);
  assert.throws(() => horse.setLook({ coat: 'x' }), TypeError);
  assert.throws(() => horse.setLook({ body: 3 }), TypeError);
  assert.throws(() => horse.setLook(null), TypeError);
  assert.equal(sent('horseLook').length, 1);
  horse.destroy();
  assert.equal(horse.setLook({ head: 'Draft_horse' }), false);
});

test('the catalog lists every part, and the common looks are all in it', () => {
  for (const part of PARTS) {
    const options = HorseLooks.options(part);
    assert.ok(options.length > 0);
    for (const name of HorseLooks.common(part)) {
      assert.ok(options.includes(name));
    }
  }
  assert.ok(HorseLooks.options('head').includes('Female_horse'));
  assert.throws(() => HorseLooks.options('mane'), TypeError);
  // A look picked with a fixed random source, and checked again by lookOf.
  const look = randomLook(() => 0);
  assert.deepEqual(lookOf(look, {}), look);
});

// Horse.name is writable: every client's copy of the horse takes the new name.
test('setting a horse name renames it on every client, and a spread still carries it', async () => {
  const { Horse, sent } = await startedHost();
  const horse = Horse.spawn({ x: 0, y: 0, z: 0 }, {}, '', 'Pebbles');
  assert.equal(horse.name, 'Pebbles');
  horse.name = 'Hrom';
  assert.equal(horse.name, 'Hrom');
  assert.deepEqual(sent('horseName'), [{ op: 'horseName', id: horse.id, name: 'Hrom' }]);
  assert.equal({ ...horse }.name, 'Hrom');
  assert.equal(JSON.parse(JSON.stringify(horse)).name, 'Hrom');
  assert.throws(() => {
    horse.name = 3;
  }, TypeError);
  horse.destroy();
  horse.name = 'Gone';
  assert.equal(sent('horseName').length, 1);
});

test("a horse spawned by a game soul's GUID keeps that soul's own look", () => {
  const { isGameHorseSoul, lookField } = require('./horses/looks.cjs');
  assert.equal(isGameHorseSoul('e39e9f8c-6604-4e34-90fd-4be29da50ad2'), true);
  assert.equal(isGameHorseSoul('FC0E6251-B314-4398-B83F-3A66A52961EE'), false);
  assert.equal(isGameHorseSoul('horse'), false);
  assert.equal(isGameHorseSoul(''), false);
  assert.equal(lookField(null), '');
});

test('Horse.spawn by a game soul sends no look, so its soul shows its own; a look goes over it', async () => {
  const { Horse, sent } = await startedHost();
  const named = Horse.spawn({ x: 1, y: 2, z: 3 }, {}, 'e39e9f8c-6604-4e34-90fd-4be29da50ad2');
  assert.equal(named.look, null);
  assert.equal(sent('horseSpawn')[0].look, '');
  assert.equal(sent('horseSpawn')[0].soul, 'e39e9f8c-6604-4e34-90fd-4be29da50ad2');
  const dressed = Horse.spawn(
    { x: 1, y: 2, z: 3 },
    {},
    'e39e9f8c-6604-4e34-90fd-4be29da50ad2',
    '',
    undefined,
    {
      look: { body: 'Horse_Solid_Black_m02' },
    },
  );
  assert.equal(dressed.look.body, 'Horse_Solid_Black_m02');
  assert.equal(named.setLook({ head: 'War_horse' }), true);
  assert.equal(named.look.head, 'War_horse');
});
