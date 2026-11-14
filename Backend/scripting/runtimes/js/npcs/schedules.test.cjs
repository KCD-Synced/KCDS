'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { ResourceHost } = require('../host.cjs');
const { activeIndex, kMostEntries } = require('./schedules.cjs');

// Npc.setSchedule: entries by the world clock's hour, each ordered when its hour comes.

async function startedHost(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-schedules-'));
  const output = [];
  const host = new ResourceHost(root, {
    entityIds: { first: 4097, count: 2048 },
    send: (message) => output.push(message),
    log: () => {},
  });
  await host.startAll();
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  let hour = 7;
  host.npcs.schedules.hour = () => hour;
  const sent = (op) => output.filter((message) => message.op === op);
  return { host, Npc: host.npcs.Npc, sent, setHour: (value) => (hour = value) };
}

test('the entry that holds is the last started, or yesterday evening before the first', () => {
  const entries = [{ hour: 6 }, { hour: 12 }, { hour: 20 }];
  assert.equal(activeIndex(entries, 5.9), 2);
  assert.equal(activeIndex(entries, 6), 0);
  assert.equal(activeIndex(entries, 13.5), 1);
  assert.equal(activeIndex(entries, 23.99), 2);
});

test('each entry orders the NPC when its hour comes, with its action', async (t) => {
  const { host, Npc, sent, setHour } = await startedHost(t);
  const baker = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const started = [];
  host.on('npcScheduleEntry', (npc, entry) => started.push([npc, entry.index, entry.hour]));
  baker.setSchedule([
    { hour: 20, position: { x: 3, y: 3, z: 3 } },
    { hour: 6, position: { x: 1, y: 1, z: 1 }, clip: 'kneel_idle', loop: true, facing: 180 },
    { hour: 12, position: { x: 2, y: 2, z: 2 }, speed: 'jog' },
  ]);
  // Kept by the hour; at 7 the morning entry holds and is ordered at once.
  assert.deepEqual(
    baker.schedule.map((entry) => entry.hour),
    [6, 12, 20],
  );
  assert.deepEqual(started, [[baker, 0, 6]]);
  assert.match(sent('npcIntent').at(-1).intent, /^1\|moveTo\|1\|1\|1\|walk\|/);
  // Arriving there plays the morning's action.
  host.npcs.intentDone({ id: baker.id, status: 'reached', sequence: 1 });
  assert.equal(sent('npcAnimation').at(-1).animation, '1|1|kneel_idle');
  // Nothing new until the next hour comes.
  host.npcs.schedules.tick();
  assert.equal(sent('npcIntent').length, 1);
  setHour(12.5);
  host.npcs.schedules.tick();
  assert.match(sent('npcIntent').at(-1).intent, /^2\|moveTo\|2\|2\|2\|jog\|/);
  setHour(21);
  host.npcs.schedules.tick();
  setHour(2);
  host.npcs.schedules.tick();
  assert.deepEqual(
    started.map(([, index]) => index),
    [0, 1, 2],
  );
  // Removed with the NPC; null clears it.
  const smith = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  smith.setSchedule([{ hour: 1, position: { x: 4, y: 4, z: 4 } }]);
  smith.setSchedule(null);
  assert.equal(smith.schedule, null);
  baker.remove();
  host.npcs.schedules.tick();
  assert.equal(host.npcs.schedules.schedules.size, 0);
  assert.equal(host.npcs.schedules.timer, null);
});

test('setSchedule refuses entries it cannot use, before anything is ordered', async (t) => {
  const { Npc, sent } = await startedHost(t);
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  const place = { x: 1, y: 1, z: 1 };
  assert.throws(() => npc.setSchedule('morning'), TypeError);
  assert.throws(() => npc.setSchedule([{ hour: 24, position: place }]), RangeError);
  assert.throws(() => npc.setSchedule([{ hour: -1, position: place }]), RangeError);
  assert.throws(() => npc.setSchedule([{ hour: 8 }]), TypeError);
  assert.throws(() => npc.setSchedule([{ hour: 8, position: place, sing: true }]), TypeError);
  assert.throws(() => npc.setSchedule([{ hour: 8, position: place, loop: true }]), TypeError);
  assert.throws(() => npc.setSchedule([{ hour: 8, position: place, speed: 'fly' }]));
  assert.throws(
    () => npc.setSchedule([{ hour: 8, position: place, clip: 'kneel_idle', facing: 'north' }]),
    TypeError,
  );
  const many = Array.from({ length: kMostEntries + 1 }, (_, hour) => ({
    hour: hour / 2,
    position: place,
  }));
  assert.throws(() => npc.setSchedule(many), RangeError);
  assert.equal(sent('npcIntent').length, 0);
  assert.equal(npc.schedule, null);
});
