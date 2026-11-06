'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// The health, death and knock-out of a horse a resource spawned: the ledger the
// resource sets, the native server's horseHealth reports of the simulator, and the events.

async function startedHost() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-horse-health-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 500, count: 1025 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  const sent = () => output.filter((message) => message.op === 'horseHealth');
  const events = [];
  for (const name of ['horseDamage', 'horseDeath', 'horseKnockedOut', 'horseWake', 'horseRevive']) {
    host.on(name, (horse, ...rest) => events.push([name, horse.id, ...rest]));
  }
  const spawn = (options) =>
    host.api.Horse.spawn({ x: 1, y: 2, z: 3 }, {}, '', '', undefined, options);
  return { host, Horse: host.api.Horse, sent, events, spawn, output };
}

test('a horse starts alive at full health, writing nothing', async () => {
  const { spawn, sent, events } = await startedHost();
  const horse = spawn();
  assert.equal(horse.health, 100);
  assert.equal(horse.maxHealth, 100);
  assert.equal(horse.alive, true);
  assert.equal(horse.knockedOut, false);
  assert.equal(sent().length, 0);
  assert.deepEqual(events, []);
});

test('the health setter clamps, validates and sends the ledger', async () => {
  const { spawn, sent, events } = await startedHost();
  const horse = spawn();
  horse.health = 40;
  assert.equal(horse.health, 40);
  assert.deepEqual(sent().at(-1), { op: 'horseHealth', id: horse.id, health: 40, state: 0 });
  // A scripted loss of health is a horseDamage without an attacker.
  assert.deepEqual(events, [['horseDamage', horse.id, null, 60, 'scripted']]);
  horse.health = 250;
  assert.equal(horse.health, 100);
  assert.equal(sent().at(-1).health, 100);
  assert.equal(events.length, 1);
  for (const value of [-1, Number.NaN, Number.POSITIVE_INFINITY]) {
    assert.throws(() => (horse.health = value), RangeError);
  }
  for (const value of ['5', null, undefined, {}]) {
    assert.throws(() => (horse.health = value), RangeError);
  }
  assert.equal(horse.health, 100);
});

test('health 0 and kill() kill the horse; a dead horse ignores the writes', async () => {
  const { spawn, sent, events } = await startedHost();
  const horse = spawn();
  horse.health = 0;
  assert.equal(horse.alive, false);
  assert.equal(horse.health, 0);
  assert.deepEqual(sent().at(-1), { op: 'horseHealth', id: horse.id, health: 0, state: 2 });
  assert.deepEqual(events, [['horseDeath', horse.id, null, 'scripted']]);
  const sentBefore = sent().length;
  horse.health = 50;
  assert.equal(horse.health, 0);
  assert.equal(horse.kill(), false);
  assert.equal(horse.knockDown(), false);
  assert.equal(horse.wake(), false);
  assert.equal(sent().length, sentBefore);

  const other = spawn();
  assert.equal(other.kill(), true);
  assert.equal(other.alive, false);
  assert.equal(events.at(-1)[0], 'horseDeath');
});

test('knockDown() and wake() move between alive and knocked out', async () => {
  const { spawn, sent, events } = await startedHost();
  const horse = spawn();
  assert.equal(horse.wake(), false);
  assert.equal(horse.knockDown(), true);
  assert.equal(horse.knockedOut, true);
  assert.equal(horse.alive, true);
  assert.equal(horse.health, 0);
  assert.deepEqual(sent().at(-1), { op: 'horseHealth', id: horse.id, health: 0, state: 1 });
  assert.deepEqual(events, [['horseKnockedOut', horse.id, null, 'scripted']]);
  assert.equal(horse.knockDown(), false);
  // Getting up with no health given: a lethal blow left none, so it is full.
  assert.equal(horse.wake(), true);
  assert.equal(horse.knockedOut, false);
  assert.equal(horse.health, 100);
  assert.deepEqual(sent().at(-1), { op: 'horseHealth', id: horse.id, health: 100, state: 0 });
  assert.deepEqual(events.at(-1), ['horseWake', horse.id]);
  // With a health, and a health that cannot be.
  horse.knockDown();
  assert.throws(() => horse.wake(0), RangeError);
  assert.throws(() => horse.wake(-5), RangeError);
  assert.throws(() => horse.wake('7'), RangeError);
  assert.equal(horse.knockedOut, true);
  assert.equal(horse.wake(30), true);
  assert.equal(horse.health, 30);
  // A knocked-out horse can be killed, and then it is dead.
  horse.knockDown();
  assert.equal(horse.kill(), true);
  assert.equal(horse.alive, false);
  assert.equal(horse.knockedOut, false);
});

test('revive() brings a dead horse back and refuses a living one', async () => {
  const { spawn, sent, events } = await startedHost();
  const horse = spawn();
  assert.equal(horse.revive(), false);
  horse.kill();
  assert.equal(horse.revive(), true);
  assert.equal(horse.alive, true);
  assert.equal(horse.health, 100);
  assert.deepEqual(sent().at(-1), { op: 'horseHealth', id: horse.id, health: 100, state: 0 });
  assert.deepEqual(events.at(-1), ['horseRevive', horse.id]);
  assert.equal(horse.revive(), false);
  horse.kill();
  assert.throws(() => horse.revive(0), RangeError);
  assert.throws(() => horse.revive(-5), RangeError);
  assert.throws(() => horse.revive(Number.NaN), RangeError);
  assert.equal(horse.revive(25), true);
  assert.equal(horse.health, 25);
  // A knocked-out horse is no dead horse.
  horse.knockDown();
  assert.equal(horse.revive(), false);
});

test('a destroyed horse takes none of it', async () => {
  const { spawn, sent } = await startedHost();
  const horse = spawn();
  horse.destroy();
  const before = sent().length;
  assert.equal(horse.kill(), false);
  assert.equal(horse.knockDown(), false);
  assert.equal(horse.wake(), false);
  assert.equal(horse.revive(), false);
  horse.health = 10;
  assert.equal(sent().length, before);
});

test('the simulator reports health the game took: horseDamage, then death or knock-out', async () => {
  const { host, spawn, sent, events } = await startedHost();
  const horse = spawn();
  host.horseHealthReported({ id: horse.id, health: 70, state: 0 });
  assert.equal(horse.health, 70);
  assert.deepEqual(events, [['horseDamage', horse.id, null, 30, 'unknown']]);
  // A report is no resource decision: nothing goes back to the native server.
  assert.equal(sent().length, 0);
  // Health coming back is no damage.
  host.horseHealthReported({ id: horse.id, health: 90, state: 0 });
  assert.equal(horse.health, 90);
  assert.equal(events.length, 1);
  // The lethal blow: damage first, then the death.
  host.horseHealthReported({ id: horse.id, health: 0, state: 2 });
  assert.equal(horse.alive, false);
  assert.deepEqual(events.slice(1), [
    ['horseDamage', horse.id, null, 90, 'unknown'],
    ['horseDeath', horse.id, null, 'unknown'],
  ]);

  // The server turned a death into a knock-out (the horse's knockOut option) before it got here.
  const knocked = spawn({ knockOut: true });
  host.horseHealthReported({ id: knocked.id, health: 0, state: 1 });
  assert.equal(knocked.alive, true);
  assert.equal(knocked.knockedOut, true);
  assert.deepEqual(events.at(-1), ['horseKnockedOut', knocked.id, null, 'unknown']);
});

test('a report never moves a horse that lies back up, and a stale one is dropped', async () => {
  const { host, spawn, events } = await startedHost();
  const dead = spawn();
  dead.kill();
  const count = events.length;
  host.horseHealthReported({ id: dead.id, health: 80, state: 0 });
  host.horseHealthReported({ id: dead.id, health: 80, state: 1 });
  assert.equal(dead.alive, false);
  assert.equal(dead.health, 0);

  const down = spawn();
  down.knockDown();
  host.horseHealthReported({ id: down.id, health: 80, state: 0 });
  host.horseHealthReported({ id: down.id, health: 0, state: 1 });
  assert.equal(down.knockedOut, true);
  assert.equal(down.health, 0);
  // The one thing a report still does to a knocked-out horse is kill it.
  host.horseHealthReported({ id: down.id, health: 0, state: 2 });
  assert.equal(down.alive, false);
  assert.deepEqual(
    events.slice(count).map((event) => event[0]),
    ['horseKnockedOut', 'horseDeath'],
  );

  // Reports of horses that are gone or of values that make no sense do nothing.
  const gone = spawn();
  gone.destroy();
  host.horseHealthReported({ id: gone.id, health: 0, state: 2 });
  host.horseHealthReported({ id: 999999, health: 0, state: 2 });
  const fine = spawn();
  host.horseHealthReported({ id: fine.id, health: Number.NaN, state: 0 });
  host.horseHealthReported({ id: fine.id, health: 10, state: 7 });
  assert.equal(fine.health, 100);
});

test('a dead or knocked-out horse drops the order it walked, and takes none', async () => {
  const { spawn, sent, output } = await startedHost();
  const horse = spawn();
  assert.equal(horse.moveTo({ x: 10, y: 2, z: 3 }), true);
  assert.equal(horse.travelStatus, 'running');
  horse.knockDown();
  assert.equal(horse.travelStatus, 'idle');
  assert.equal(output.filter((message) => message.op === 'horseIntent').at(-1).intent, '');
  assert.equal(horse.moveTo({ x: 10, y: 2, z: 3 }), false);
  assert.equal(horse.patrol([{ x: 1, y: 2, z: 3 }]), false);
  horse.wake();
  assert.equal(horse.moveTo({ x: 10, y: 2, z: 3 }), true);
  horse.kill();
  assert.equal(horse.moveTo({ x: 10, y: 2, z: 3 }), false);
  assert.equal(sent().length, 3);
});
