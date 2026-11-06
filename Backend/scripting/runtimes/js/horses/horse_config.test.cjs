'use strict';

// a horse's carry weight limit, from the server's horse config and per horse.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { ResourceHost } = require('../host.cjs');

function hostWith(config) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-horse-config-'));
  if (config !== undefined) {
    fs.writeFileSync(path.join(root, 'horse-config.json'), JSON.stringify(config));
  }
  const output = [];
  const logs = [];
  const host = new ResourceHost(root, {
    send: (e) => output.push(e),
    log: (who, text) => logs.push(text),
  });
  return { host, output, logs };
}

test('a spawned horse carries the config maxCarryWeight; setting it tells every game', () => {
  const { host, output } = hostWith({ maxCarryWeight: 150 });
  const { Horse } = host.makeAPI();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  assert.equal(horse.maxCarryWeight, 150);
  assert.equal(output.find((e) => e.op === 'horseSpawn').maxCarryWeight, 150);
  horse.maxCarryWeight = 80.5;
  assert.deepEqual(output.at(-1), { op: 'horseMaxCarryWeight', id: horse.id, weight: 80.5 });
  horse.maxCarryWeight = null;
  assert.deepEqual(output.at(-1), { op: 'horseMaxCarryWeight', id: horse.id, weight: -1 });
  assert.equal(horse.maxCarryWeight, null);
  assert.throws(() => {
    horse.maxCarryWeight = -5;
  }, TypeError);
});

test("without a config a horse carries the game's own; a wrong config is named", () => {
  const plain = hostWith();
  const horse = plain.host.makeAPI().Horse.spawn({ x: 0, y: 0, z: 0 });
  assert.equal(horse.maxCarryWeight, null);
  assert.equal(plain.output.find((e) => e.op === 'horseSpawn').maxCarryWeight, -1);
  const wrong = hostWith({ maxCarryWeight: 'heavy' });
  assert.ok(wrong.logs.some((line) => line.includes('maxCarryWeight is kg')));
});

// breeds in the server's horse config.
test('a breed gives a spawned horse its look, carry limit and knockOut; the call goes over it', () => {
  const { HorseLooks } = require('./looks.cjs');
  const head = HorseLooks.options('head')[0];
  const body = HorseLooks.options('body')[0];
  const { host, output, logs } = hostWith({
    maxCarryWeight: 100,
    breeds: {
      warhorse: { look: { head, body }, maxCarryWeight: 200, knockOut: true },
      pony: { look: { head: 'no_such_head' } },
    },
  });
  assert.ok(logs.some((line) => line.includes('breed pony: a look part is not in HorseLooks')));
  const { Horse } = host.makeAPI();
  const war = Horse.spawn({ x: 0, y: 0, z: 0 }, {}, '', '', undefined, { breed: 'warhorse' });
  assert.equal(war.breed, 'warhorse');
  assert.equal(war.maxCarryWeight, 200);
  assert.equal(war.knockOut, true);
  assert.equal(war.look.head, head);
  assert.equal(war.look.body, body);
  const sent = output.find((e) => e.op === 'horseSpawn' && e.id === war.id);
  assert.equal(sent.maxCarryWeight, 200);
  assert.ok(sent.look.startsWith(`${body}|${head}|`));
  const light = Horse.spawn({ x: 0, y: 0, z: 0 }, {}, '', '', undefined, {
    breed: 'warhorse',
    maxCarryWeight: 50,
    knockOut: false,
  });
  assert.equal(light.maxCarryWeight, 50);
  assert.equal(light.knockOut, false);
  const plain = Horse.spawn({ x: 0, y: 0, z: 0 });
  assert.equal(plain.breed, null);
  assert.equal(plain.maxCarryWeight, 100);
  assert.throws(
    () => Horse.spawn({ x: 0, y: 0, z: 0 }, {}, '', '', undefined, { breed: 'pony' }),
    /Unknown horse breed/,
  );
});

test('a breed gives stamina, speed and hit points; the call and the setters go over them', () => {
  const { host, output, logs } = hostWith({
    breeds: {
      courser: { stamina: 300, speed: 50, health: 200 },
      broken: { speed: 20 },
    },
  });
  assert.ok(logs.some((line) => line.includes('broken')));
  const { Horse } = host.makeAPI();
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 }, {}, '', '', undefined, { breed: 'courser' });
  assert.equal(horse.maxStamina, 300);
  assert.equal(horse.speed, 50);
  assert.equal(horse.hitPoints, 200);
  const spawned = output.find((e) => e.op === 'horseSpawn');
  assert.equal(spawned.maxStamina, 300);
  assert.equal(spawned.speed, 50);
  const quick = Horse.spawn({}, {}, '', '', undefined, { breed: 'courser', speed: 45 });
  assert.equal(quick.speed, 45);
  const plain = Horse.spawn({});
  assert.equal(plain.maxStamina, null);
  assert.equal(output.filter((e) => e.op === 'horseSpawn').at(-1).maxStamina, -1);
  horse.maxStamina = 250;
  assert.deepEqual(output.at(-1), { op: 'horseStats', id: horse.id, maxStamina: 250, speed: 50 });
  horse.speed = null;
  assert.deepEqual(output.at(-1), { op: 'horseStats', id: horse.id, maxStamina: 250, speed: -1 });
  assert.throws(() => {
    horse.speed = 0;
  }, TypeError);
  assert.throws(() => {
    horse.maxStamina = 'a lot';
  }, TypeError);
});

test('a horse of 200 hit points loses half the health the game took', () => {
  const { host, output } = hostWith({ breeds: { courser: { health: 200 } } });
  const { Horse } = host.makeAPI();
  const horse = Horse.spawn({}, {}, '', '', undefined, { breed: 'courser' });
  host.horseHealth.reported({ id: horse.id, health: 80, state: 0 });
  assert.equal(horse.health, 90);
  assert.deepEqual(output.at(-1), { op: 'horseHealth', id: horse.id, health: 90, state: 0 });
  const plain = Horse.spawn({});
  host.horseHealth.reported({ id: plain.id, health: 80, state: 0 });
  assert.equal(plain.health, 80);
});
