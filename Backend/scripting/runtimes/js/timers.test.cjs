'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict');
const { resourceTimers } = require('./timers.cjs');

// Direct contract tests for the resource timer facade. The host wires these
// into every resource vm and relies on the ownership set (state.timers) to
// cancel work on stop or reload, so the facade must keep that set exact.
function harness(active = true) {
  const state = { active, timers: new Set() };
  const failures = [];
  const timers = resourceTimers(
    state,
    (fn) => fn(),
    (error) => failures.push(error),
  );
  return { state, timers, failures };
}
const tick = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

test('server Timeout preserves native reference control and return identity', () => {
  const { timers } = harness();
  const handle = timers.setTimeout(() => {}, 1000);
  try {
    assert.equal(handle.hasRef(), true);
    assert.equal(handle.unref(), handle);
    assert.equal(handle.hasRef(), false);
    assert.equal(handle.ref(), handle);
    assert.equal(handle.hasRef(), true);
    assert.equal(handle.refresh(), handle);
  } finally {
    timers.clearTimeout(handle);
  }
});

test('a refreshed timeout fires again and removes itself from the ownership set', async () => {
  const { state, timers } = harness();
  let fired = 0;
  const handle = timers.setTimeout(() => {
    fired += 1;
  }, 1);
  await tick(5);
  assert.equal(fired, 1);
  assert.equal(state.timers.has(handle), false);
  handle.refresh();
  assert.equal(state.timers.has(handle), true);
  await tick(5);
  assert.equal(fired, 2);
  assert.equal(state.timers.size, 0);
});

test('a stopped resource refuses new timers and never revives a refreshed one', async () => {
  const { state, timers } = harness();
  const handle = timers.setTimeout(() => {}, 1);
  await tick(5);
  state.active = false;
  assert.throws(() => timers.setTimeout(() => {}, 1), /Resource is stopped/);
  assert.throws(() => timers.setInterval(() => {}, 1), /Resource is stopped/);
  assert.throws(() => timers.queueMicrotask(() => {}), /Resource is stopped/);
  assert.equal(handle.refresh(), handle);
  assert.equal(state.timers.has(handle), false);
});

test('clearInterval stops the ticks and releases ownership', async () => {
  const { state, timers } = harness();
  let ticks = 0;
  const handle = timers.setInterval(() => {
    ticks += 1;
  }, 8);
  await tick(50);
  const seen = ticks;
  assert.ok(seen >= 2);
  timers.clearInterval(handle);
  assert.equal(state.timers.has(handle), false);
  await tick(50);
  assert.equal(ticks, seen);
});

test('queued microtasks keep FIFO order and route handler errors to the failure sink', async () => {
  const { timers, failures } = harness();
  const order = [];
  timers.queueMicrotask(() => order.push('first'));
  timers.queueMicrotask(() => {
    order.push('second');
    throw new Error('microtask fault');
  });
  timers.queueMicrotask(() => order.push('third'));
  await tick(5);
  assert.deepEqual(order, ['first', 'second', 'third']);
  assert.equal(failures.length, 1);
  assert.match(failures[0].message, /microtask fault/);
});

test('a microtask queued before stop does not run its handler afterwards', async () => {
  const { state, timers } = harness();
  let ran = false;
  timers.queueMicrotask(() => {
    ran = true;
  });
  state.active = false;
  await tick(5);
  assert.equal(ran, false);
});
