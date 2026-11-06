'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { HorseOwners, OWNER_ABSENCE_MS, testAbsenceMs } = require('./horses/horse_owners.cjs');

// A player's horse after its owner left: it waits OWNER_ABSENCE_MS (15 minutes) and
// is then removed unless it was stolen; one timer per departed owner, none over all horses.

// Timers the test runs by hand, and the clock they read.
function fakeTime() {
  const time = { now: 0, timers: new Map(), next: 1 };
  time.clock = () => time.now;
  time.setTimer = (fn, ms) => {
    const id = time.next;
    time.next += 1;
    time.timers.set(id, { fn, at: time.now + ms });
    return id;
  };
  time.clearTimer = (id) => time.timers.delete(id);
  // Moves the clock on and runs the timers due by then, as Node would.
  time.advance = (ms) => {
    time.now += ms;
    for (const [id, timer] of [...time.timers]) {
      if (timer.at <= time.now && time.timers.has(id)) {
        time.timers.delete(id);
        timer.fn();
      }
    }
  };
  return time;
}

function owners(time, connected = new Set()) {
  const removed = [];
  const logs = [];
  const horseOwners = new HorseOwners({
    clock: time.clock,
    setTimer: time.setTimer,
    clearTimer: time.clearTimer,
    connected: (id) => connected.has(id),
    remove: (horse) => {
      removed.push(horse.id);
      horseOwners.destroyed(horse);
    },
    log: (text) => logs.push(text),
  });
  return { horseOwners, removed, logs };
}

function give(horseOwners, horse, ownerId) {
  const previous = horse.ownerId;
  horse.ownerId = ownerId;
  horseOwners.owned(horse, previous);
}

test('the wait is 15 minutes; only a test server shortens it', () => {
  assert.equal(OWNER_ABSENCE_MS, 15 * 60 * 1000);
  assert.equal(testAbsenceMs({}), undefined);
  assert.equal(testAbsenceMs({ KCDS_HORSE_OWNER_ABSENCE_SECONDS: '60' }), undefined);
  const testServer = (seconds) =>
    testAbsenceMs({ KCDS_TEST_HOOKS: '1', KCDS_HORSE_OWNER_ABSENCE_SECONDS: seconds });
  assert.equal(testServer('60'), 60000);
  for (const bad of ['', '0', '-5', 'soon']) {
    assert.equal(testServer(bad), undefined);
  }
});

test("an owner's horses wait 15 minutes, then go; one timer per owner", () => {
  const time = fakeTime();
  const { horseOwners, removed } = owners(time);
  const mine = [{ id: 500 }, { id: 501 }];
  const other = { id: 502 };
  for (const horse of mine) give(horseOwners, horse, 1);
  give(horseOwners, other, 2);
  horseOwners.ownerLeft(1);
  assert.equal(time.timers.size, 1);
  assert.ok(horseOwners.waiting(mine[0]) && !horseOwners.waiting(other));
  time.advance(OWNER_ABSENCE_MS - 1);
  assert.deepEqual(removed, []);
  time.advance(1);
  assert.deepEqual(removed, [500, 501]);
  // Nothing of the departed owner is left: no timer, no entry.
  assert.equal(time.timers.size, 0);
  assert.equal(horseOwners.absences.size, 0);
  assert.ok(!horseOwners.byOwner.has(1));
  assert.ok(horseOwners.byOwner.get(2).has(other));
});

test('a timer that runs early is set again: no horse goes before 15 minutes', () => {
  const time = fakeTime();
  const { horseOwners, removed } = owners(time);
  const horse = { id: 500 };
  give(horseOwners, horse, 1);
  horseOwners.ownerLeft(1);
  const [[first]] = [...time.timers];
  time.now = OWNER_ABSENCE_MS - 5;
  // Node fired it 5 ms early.
  horseOwners.expire(1);
  time.clearTimer(first);
  assert.deepEqual(removed, []);
  assert.equal(time.timers.size, 1);
  time.advance(4);
  assert.deepEqual(removed, []);
  time.advance(1);
  assert.deepEqual(removed, [500]);
});

test('a stolen horse does not go with its owner; its timer is cleared', () => {
  const time = fakeTime();
  const { horseOwners, removed, logs } = owners(time);
  const before = { id: 500 };
  const during = { id: 501 };
  give(horseOwners, before, 1);
  give(horseOwners, during, 1);
  // Stolen while the owner is still there.
  horseOwners.riderChanged(before, 3);
  assert.ok(horseOwners.stolen(before));
  horseOwners.ownerLeft(1);
  assert.equal(time.timers.size, 1);
  // The other one is taken while the owner is away: no horse waits, the timer goes.
  horseOwners.riderChanged(during, 4);
  assert.equal(time.timers.size, 0);
  assert.equal(horseOwners.absences.size, 0);
  time.advance(2 * OWNER_ABSENCE_MS);
  assert.deepEqual(removed, []);
  assert.ok(logs.some((line) => line === 'horse 501 stolen from player 1 by player 4'));
  // An owner whose horses were all stolen before they left starts no timer at all.
  const lone = { id: 503 };
  give(horseOwners, lone, 5);
  horseOwners.riderChanged(lone, 6);
  horseOwners.ownerLeft(5);
  assert.equal(time.timers.size, 0);
});

test('the owner riding it again takes it back; a giveTo ends the wait', () => {
  const time = fakeTime();
  const { horseOwners, removed } = owners(time, new Set([2]));
  const horse = { id: 500 };
  give(horseOwners, horse, 1);
  horseOwners.riderChanged(horse, 3);
  horseOwners.riderChanged(horse, 1);
  assert.ok(!horseOwners.stolen(horse));
  horseOwners.ownerLeft(1);
  assert.ok(horseOwners.waiting(horse));
  // A resource gives it to the returning player (a new connection, a new id).
  give(horseOwners, horse, 2);
  assert.equal(time.timers.size, 0);
  time.advance(OWNER_ABSENCE_MS);
  assert.deepEqual(removed, []);
  // A stolen horse given to someone is theirs, not stolen.
  horseOwners.riderChanged(horse, 3);
  give(horseOwners, horse, 2);
  assert.ok(!horseOwners.stolen(horse));
});

test('a horse destroyed while it waits takes its timer and entries with it', () => {
  const time = fakeTime();
  const { horseOwners, removed } = owners(time);
  const horse = { id: 500 };
  give(horseOwners, horse, 1);
  horseOwners.riderChanged(horse, 2);
  horseOwners.riderChanged(horse, 1);
  horseOwners.ownerLeft(1);
  horseOwners.destroyed(horse);
  assert.equal(time.timers.size, 0);
  assert.equal(horseOwners.absences.size, 0);
  assert.equal(horseOwners.byOwner.size, 0);
  assert.equal(horseOwners.thieves.size, 0);
  time.advance(OWNER_ABSENCE_MS);
  assert.deepEqual(removed, []);
  // Shutdown: every timer goes.
  const other = { id: 501 };
  give(horseOwners, other, 3);
  horseOwners.ownerLeft(3);
  horseOwners.clear();
  assert.equal(time.timers.size, 0);
});

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-horse-owners-'));
  const output = [];
  const logs = [];
  const host = new ResourceHost(root, {
    send: (e) => output.push(e),
    log: (resource, text) => logs.push(text),
  });
  const dir = path.join(root, 'stable');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'stable', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  const time = fakeTime();
  host.horseOwners.clock = time.clock;
  host.horseOwners.setTimer = time.setTimer;
  host.horseOwners.clearTimer = time.clearTimer;
  return { host, output, logs, time, root };
}

function player(host, id) {
  return host.updatePlayer({
    id,
    nickname: `Player ${id}`,
    ready: true,
    position: { x: 10, y: 20, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
}

const STABLE = `
  kcds.core.Events.on('playerCommand', (player, command) => {
    if (command !== 'horse') return;
    kcds.characters.Horse.spawn(player.position).giveTo(player);
  });
  kcds.core.Events.on('horseDestroy', (horse) => kcds.core.Chat.sendToAll('gone ' + horse.id + ' of ' + horse.ownerId));
`;

function report(host, horse, riderId) {
  host.updateHorse({
    id: horse.id,
    position: { x: 10, y: 20, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    dimension: 0xffffffff,
    riderId,
  });
}

test('a given horse stays when its owner leaves and goes 15 minutes later', async () => {
  const { host, output, logs, time, root } = hostWith(STABLE);
  try {
    await host.startAll();
    const owner = player(host, 1);
    host.emit('playerCommand', owner, 'horse', []);
    const [horse] = host.api.Horse.all();
    assert.equal(horse.ownerId, 1);
    // The owner rides it, then leaves: the horse stays in the world, still theirs.
    report(host, horse, 1);
    host.disconnect(1);
    assert.equal(host.api.Horse.all().length, 1);
    assert.equal(horse.ownerId, 1);
    assert.equal(horse.owner, null);
    assert.ok(!output.some((e) => e.op === 'horseDestroy'));
    assert.ok(logs.includes(`horse ${horse.id} waits 900 s for its owner 1`));
    time.advance(OWNER_ABSENCE_MS - 1);
    assert.equal(host.api.Horse.all().length, 1);
    time.advance(1);
    assert.equal(host.api.Horse.all().length, 0);
    const destroys = output.filter((e) => e.op === 'horseDestroy');
    assert.deepEqual(destroys, [{ op: 'horseDestroy', id: horse.id }]);
    // horseDestroy still reads the owner.
    assert.ok(output.some((e) => e.op === 'chat' && e.text === `gone ${horse.id} of 1`));
    assert.ok(logs.includes(`horse ${horse.id} removed: its owner 1 has been away 900 s`));
    assert.equal(time.timers.size, 0);
  } finally {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('a horse another player rode off with stays after its owner left', async () => {
  const { host, output, time, root } = hostWith(STABLE);
  try {
    await host.startAll();
    const owner = player(host, 1);
    player(host, 2);
    host.emit('playerCommand', owner, 'horse', []);
    const [horse] = host.api.Horse.all();
    host.disconnect(1);
    // Player 2 mounts it (the game's own steal) while its owner is away.
    report(host, horse, 2);
    report(host, horse, 0);
    time.advance(2 * OWNER_ABSENCE_MS);
    assert.equal(host.api.Horse.all().length, 1);
    assert.ok(!output.some((e) => e.op === 'horseDestroy'));
    assert.equal(time.timers.size, 0);
  } finally {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('giving the horse to the returning player keeps it', async () => {
  const { host, time, root } = hostWith(STABLE);
  try {
    await host.startAll();
    const owner = player(host, 1);
    host.emit('playerCommand', owner, 'horse', []);
    const [horse] = host.api.Horse.all();
    host.disconnect(1);
    const back = player(host, 2);
    horse.giveTo(back);
    time.advance(OWNER_ABSENCE_MS);
    assert.equal(host.api.Horse.all().length, 1);
    assert.equal(horse.ownerId, 2);
    // Now that player leaves: the wait starts again for them.
    host.disconnect(2);
    time.advance(OWNER_ABSENCE_MS);
    assert.equal(host.api.Horse.all().length, 0);
  } finally {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  }
});

// removedWithOwner and giveTo(null): a horse set to outlive its owner is left ownerless.

const KEEPER = `
  kcds.core.Events.on('playerCommand', (player, command) => {
    if (command !== 'horse') return;
    const horse = kcds.characters.Horse.spawn(player.position);
    horse.giveTo(player);
    horse.removedWithOwner = false;
  });
  kcds.core.Events.on('horseOwnerChanged', (horse, player) => {
    kcds.core.Chat.sendToAll('owner of ' + horse.id + ' is ' + (player ? player.id : 'nobody'));
  });
`;

test('a horse with removedWithOwner false is released the moment its owner leaves', () => {
  const time = fakeTime();
  const released = [];
  const releasing = new HorseOwners({
    clock: time.clock,
    setTimer: time.setTimer,
    clearTimer: time.clearTimer,
    connected: () => false,
    remove: () => assert.fail('no horse is removed'),
    release: (horse) => {
      released.push(horse.id);
      const previous = horse.ownerId;
      horse.ownerId = 0;
      releasing.owned(horse, previous);
    },
  });
  const keeper = { id: 500, removedWithOwner: false };
  const other = { id: 501, removedWithOwner: true };
  give(releasing, keeper, 1);
  give(releasing, other, 1);
  assert.deepEqual(
    releasing.horsesOf(1).map((horse) => horse.id),
    [500, 501],
  );
  releasing.ownerLeft(1);
  assert.deepEqual(released, [500]);
  assert.deepEqual(
    releasing.horsesOf(1).map((horse) => horse.id),
    [501],
  );
  // Only the other horse waits: one timer, and the keeper is never removed.
  assert.equal(time.timers.size, 1);
  assert.equal(keeper.ownerId, 0);
});

test('a horse set to outlive its owner alone starts no timer', () => {
  const time = fakeTime();
  const { horseOwners } = owners(time);
  const keeper = { id: 500, removedWithOwner: false };
  give(horseOwners, keeper, 1);
  horseOwners.ownerLeft(1);
  assert.equal(time.timers.size, 0);
  assert.equal(horseOwners.absences.size, 0);
});

test('removedWithOwner is true by default and takes only a boolean', async () => {
  const { host, root } = hostWith(STABLE);
  try {
    await host.startAll();
    const owner = player(host, 1);
    host.emit('playerCommand', owner, 'horse', []);
    const [horse] = host.api.Horse.all();
    assert.equal(horse.removedWithOwner, true);
    assert.throws(() => {
      horse.removedWithOwner = 'no';
    }, TypeError);
    horse.removedWithOwner = false;
    assert.equal(horse.removedWithOwner, false);
  } finally {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('giveTo(null) leaves the horse ownerless and tells the world', async () => {
  const { host, output, root } = hostWith(KEEPER);
  try {
    await host.startAll();
    const owner = player(host, 1);
    host.emit('playerCommand', owner, 'horse', []);
    const [horse] = host.api.Horse.all();
    assert.equal(horse.ownerId, 1);
    horse.giveTo(null);
    assert.equal(horse.ownerId, 0);
    assert.equal(horse.owner, null);
    assert.equal(host.horseOwners.horsesOf(1).length, 0);
    const owned = output.filter((e) => e.op === 'horseOwner');
    assert.deepEqual(
      owned.map((e) => e.ownerId),
      [1, 0],
    );
    assert.ok(output.some((e) => e.op === 'chat' && e.text === `owner of ${horse.id} is nobody`));
    // A connected player can have it again; somebody who is not connected cannot.
    horse.giveTo(owner);
    assert.equal(horse.ownerId, 1);
    assert.throws(() => horse.giveTo({ id: 99 }), /Unknown owner/);
    assert.throws(() => horse.giveTo(undefined), /Unknown owner/);
  } finally {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('a horse with removedWithOwner false stays, ownerless, when its owner leaves', async () => {
  const { host, output, logs, time, root } = hostWith(KEEPER);
  try {
    await host.startAll();
    const owner = player(host, 1);
    host.emit('playerCommand', owner, 'horse', []);
    const [horse] = host.api.Horse.all();
    host.disconnect(1);
    assert.equal(horse.ownerId, 0);
    assert.equal(horse.owner, null);
    assert.ok(output.some((e) => e.op === 'chat' && e.text === `owner of ${horse.id} is nobody`));
    assert.ok(logs.includes(`horse ${horse.id} stays ownerless: its owner 1 left`));
    // No wait, no removal, however long it takes.
    assert.equal(time.timers.size, 0);
    time.advance(2 * OWNER_ABSENCE_MS);
    assert.equal(host.api.Horse.all().length, 1);
    assert.ok(!output.some((e) => e.op === 'horseDestroy'));
    // It can be handed to somebody later.
    const back = player(host, 2);
    horse.giveTo(back);
    assert.equal(horse.ownerId, 2);
  } finally {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('turning removedWithOwner off for a horse that waits for a gone owner releases it', async () => {
  const { host, output, time, root } = hostWith(STABLE);
  try {
    await host.startAll();
    const owner = player(host, 1);
    host.emit('playerCommand', owner, 'horse', []);
    const [horse] = host.api.Horse.all();
    host.disconnect(1);
    assert.equal(time.timers.size, 1);
    horse.removedWithOwner = false;
    assert.equal(horse.ownerId, 0);
    assert.equal(time.timers.size, 0);
    time.advance(2 * OWNER_ABSENCE_MS);
    assert.equal(host.api.Horse.all().length, 1);
    assert.ok(!output.some((e) => e.op === 'horseDestroy'));
  } finally {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('a stolen horse with removedWithOwner false is released too', async () => {
  const { host, root } = hostWith(KEEPER);
  try {
    await host.startAll();
    const owner = player(host, 1);
    player(host, 2);
    host.emit('playerCommand', owner, 'horse', []);
    const [horse] = host.api.Horse.all();
    report(host, horse, 2);
    assert.equal(host.horseOwners.stolen(horse), true);
    host.disconnect(1);
    assert.equal(horse.ownerId, 0);
    assert.equal(host.horseOwners.stolen(horse), false);
  } finally {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("player.horse and player.mounted follow the server's horse in their saddle", async () => {
  const { host, root } = hostWith(STABLE);
  try {
    await host.startAll();
    const rider = player(host, 1);
    host.emit('playerCommand', rider, 'horse', []);
    const [horse] = host.api.Horse.all();
    assert.equal(rider.horse, null);
    assert.equal(rider.mounted, false);
    report(host, horse, 1);
    assert.equal(rider.horse, horse);
    assert.equal(rider.mounted, true);
    report(host, horse, 0);
    assert.equal(rider.horse, null);
    assert.equal(rider.mounted, false);
  } finally {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('allowRider lets players ride a horse they do not own, and tells their games', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  player(host, 1);
  player(host, 2);
  const Horse = host.api.Horse;
  const horse = Horse.spawn({ x: 1, y: 2, z: 3 });
  const [owner, friend] = [host.players.get(1), host.players.get(2)];
  horse.giveTo(owner);
  assert.deepEqual(horse.riders, []);
  assert.equal(horse.allowRider(friend), true);
  assert.deepEqual(horse.riders, [friend]);
  assert.deepEqual(output.filter((e) => e.op === 'horseRiders').at(-1), {
    op: 'horseRiders',
    id: horse.id,
    riders: [2],
  });
  assert.equal(horse.allowRider(friend, false), true);
  assert.deepEqual(horse.riders, []);
  assert.deepEqual(output.filter((e) => e.op === 'horseRiders').at(-1).riders, []);
  assert.throws(() => horse.allowRider({ id: 2 }), TypeError);
  assert.throws(() => horse.allowRider(friend, 'yes'), TypeError);
  horse.destroy();
  assert.equal(horse.allowRider(friend), false);
});
