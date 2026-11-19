'use strict';

// A hold of the script runtime is logged with the resource and the code that held it.

const test = require('node:test');
const assert = require('node:assert/strict');
const { AsyncLocalStorage } = require('node:async_hooks');

const { ScriptHolds } = require('./script_holds.cjs');

// Holds on clocks the test moves: run code advances them by the ms it "takes".
function setup(options = {}) {
  let now = 0;
  let wall = Date.UTC(2026, 10, 19, 12, 0, 0);
  const logs = [];
  const holds = new ScriptHolds({
    log: (source, text) => logs.push({ source, text }),
    now: () => now,
    clock: () => wall,
    ...options,
  });
  const take = (ms) => {
    now += ms;
    wall += ms;
  };
  const wait = (ms) => {
    now += ms;
    wall += ms;
  };
  return { holds, logs, take, wait };
}

test('a run over the threshold is logged as it ends, under its resource', () => {
  const { holds, logs, take } = setup();
  holds.run('shop', 'setInterval callback, every 60000 ms', () => take(250));
  holds.run('shop', 'playerJoin handler', () => take(20));
  assert.deepEqual(logs, [
    {
      source: 'shop',
      text:
        'The script runtime was held for 250 ms by shop (setInterval callback, every 60000 ms); ' +
        'every resource and server event waited',
    },
  ]);
});

test('an event one resource emits and another handles: each takes its own time', () => {
  const { holds, logs, take, wait } = setup();
  holds.run('quests', 'playerJoin handler', () => {
    take(10);
    holds.run('inventory', 'questStarted handler', () => take(400));
    take(10);
  });
  assert.equal(logs.length, 1);
  assert.equal(logs[0].source, 'inventory');
  assert.equal(
    logs[0].text,
    'The script runtime was held for 420 ms by inventory (questStarted handler); ' +
      'every resource and server event waited',
  );
  // A resource taking a fair share of the hold is named too.
  wait(10000);
  holds.run('quests', 'questDone handler', () => {
    take(150);
    holds.run('inventory', 'rewardGiven handler', () => take(250));
  });
  assert.match(
    logs[1].text,
    /by inventory \(rewardGiven handler\), 150 ms in quests \(questDone handler\);/,
  );
});

test('one line per resource every ten seconds, the holds between counted in the next', () => {
  const { holds, logs, take, wait } = setup();
  holds.run('shop', 'reconcile', () => take(300));
  wait(1000);
  holds.run('shop', 'reconcile', () => take(900));
  wait(1000);
  holds.run('shop', 'reconcile', () => take(400));
  assert.equal(logs.length, 1);
  // Another resource is not held back by it.
  holds.run('bank', 'resourceStart handler', () => take(2000));
  assert.equal(logs.length, 2);
  // The monitor is told the time of every hold, the counted ones too.
  assert.equal(holds.takeNamedMs(), 300 + 900 + 400 + 2000);
  assert.equal(holds.takeNamedMs(), 0);
  wait(10000);
  holds.run('shop', 'reconcile', () => take(250));
  assert.equal(logs.length, 3);
  assert.match(logs[2].text, /\(and 2 more such holds in the last \d+ s, the longest 900 ms\)$/);
});

test('a run that throws still counts', () => {
  const { holds, logs, take } = setup();
  assert.throws(() =>
    holds.run('shop', 'message buy handler', () => {
      take(300);
      throw new Error('no money');
    }),
  );
  assert.equal(logs.length, 1);
  holds.run('shop', 'message buy handler', () => take(10));
  assert.equal(holds.usageOf('shop').cpuMs, 310, 'the stack is clean again');
});

test('the time of the last minute and the slowest runs, for KCDS-Admin', () => {
  const { holds, take, wait } = setup();
  for (let index = 0; index < 8; index += 1) {
    holds.run('shop', `tick ${index}`, () => take(10 + index));
    wait(1000);
  }
  holds.run('shop', 'tick 0', () => take(1));
  const usage = holds.usageOf('shop');
  assert.equal(usage.cpuMs, 10 + 11 + 12 + 13 + 14 + 15 + 16 + 17 + 1);
  assert.deepEqual(
    usage.slowest.map((run) => [run.kind, run.ms]),
    [
      ['tick 7', 17],
      ['tick 6', 16],
      ['tick 5', 15],
      ['tick 4', 14],
      ['tick 3', 13],
    ],
  );
  assert.match(usage.slowest[0].at, /^2026-11-19T12:00:\d\d\.\d{3}Z$/);
  // A minute later the time is gone; the slowest stay ten minutes.
  wait(61000);
  assert.equal(holds.usageOf('shop').cpuMs, 0);
  assert.equal(holds.usageOf('shop').slowest.length, 5);
  wait(10 * 60 * 1000);
  assert.deepEqual(holds.usageOf('shop'), { cpuMs: 0, slowest: [] });
  holds.forget('shop');
  assert.deepEqual(holds.usageOf('nobody'), { cpuMs: 0, slowest: [] });
});

test('traced, code after an await is named with its resource', async () => {
  const context = new AsyncLocalStorage();
  const logs = [];
  const holds = new ScriptHolds({
    log: (source, text) => logs.push({ source, text }),
    owner: () => context.getStore()?.name ?? null,
    thresholdMs: 80,
  });
  const reconcile = () =>
    context.run({ name: 'shop' }, () =>
      holds.run('shop', 'setInterval callback', async () => {
        await new Promise((resolve) => setTimeout(resolve, 5));
        const until = Date.now() + 120;
        while (Date.now() < until) {
          // the work after the query
        }
      }),
    );
  // Not traced, the work after the await is no run of the host's.
  await reconcile();
  assert.deepEqual(logs, []);
  holds.traceAsync(60000);
  assert.ok(holds.tracing);
  await reconcile();
  holds.stopTracing();
  assert.ok(!holds.tracing);
  assert.equal(logs.length, 1, JSON.stringify(logs));
  assert.equal(logs[0].source, 'shop');
  assert.match(logs[0].text, /held for \d+ ms by shop \(code after an await\)/);
});

test('tracing ends by itself', async () => {
  const holds = new ScriptHolds({ log: () => {} });
  holds.traceAsync(0);
  await new Promise((resolve) => setTimeout(resolve, 5));
  await null;
  assert.ok(!holds.tracing);
});
