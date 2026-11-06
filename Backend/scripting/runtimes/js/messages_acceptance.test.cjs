'use strict';

// Acceptance coverage (Messages request/reply). These tests drive
// the real ResourceServices facade (the same core the server host and the
// client runtime wrap) to lock the implemented contract: owner-scoped handler
// registration, request/reply correlation, the two distinct missing-target and
// missing-type rejections, error propagation before a reply, fire-and-forget
// send, and pending-request cancellation when either side stops. The internal
// boundedness guards — an optional, default-disabled per-request timeout and a
// bounded in-flight pending set — are exercised explicitly below with a fake
// clock and small caps; they never alter the public request(resource, type,
// payload) contract, which still waits forever and rejects only on stop.

const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceServices } = require('./services.cjs');
const { normalizeManifest } = require('./manifest.cjs');

function setup() {
  const logs = [];
  const queue = [];
  const core = new ResourceServices({
    log: (...args) => logs.push(args),
    defer: (fn) => queue.push(fn),
  });
  const add = (name, options = {}) => core.add(name, normalizeManifest({ name, kcds: options }));
  const tick = () => {
    for (const fn of queue.splice(0)) fn();
  };
  const settle = async () => {
    // Flush deferred work and the microtask queue until both are idle.
    for (let i = 0; i < 20 && queue.length; i += 1) {
      tick();
      await Promise.resolve();
    }
  };
  return { core, add, tick, settle, logs };
}

test('Messages: request correlates to the typed handler reply', async () => {
  const { add, settle } = setup();
  const a = add('a', { requires: ['b'] });
  const b = add('b');
  b.Messages.handle('sum', (payload, reply) => reply(payload.x + payload.y));

  const pending = a.Messages.request('b', 'sum', { x: 2, y: 5 });
  await settle();
  assert.equal(await pending, 7);
});

test('Messages: a handler that throws before replying rejects the request', async () => {
  const { add, settle } = setup();
  const a = add('a');
  const b = add('b');
  b.Messages.handle('boom', () => {
    throw new Error('handler failed');
  });

  const pending = a.Messages.request('b', 'boom', {});
  const assertion = assert.rejects(pending, /handler failed/);
  await settle();
  await assertion;
});

test('Messages: unknown target or type rejects without dispatching', async () => {
  const { add } = setup();
  const a = add('a');
  add('b'); // b has no handlers registered
  await assert.rejects(a.Messages.request('missing', 'x', {}), /Resource is not available/);
  await assert.rejects(a.Messages.request('b', 'x', {}), /Resource is not available/);
});

test('Messages: a known target without the requested type rejects distinctly', async () => {
  const { add } = setup();
  const a = add('a');
  const b = add('b');
  // The target has a handler, just not for the requested type. The
  // contract distinguishes this from a target that has no handlers at all.
  b.Messages.handle('known', (_payload, reply) => reply('ok'));
  await assert.rejects(
    a.Messages.request('b', 'unknown', {}),
    /Resource does not handle this message type/,
  );
});

test('Messages: send is fire-and-forget and needs no reply', async () => {
  const { add, settle } = setup();
  const a = add('a');
  const b = add('b');
  const seen = [];
  b.Messages.handle('note', (payload) => seen.push(payload));

  a.Messages.send('b', 'note', { hello: 1 });
  await settle();
  assert.deepEqual(seen, [{ hello: 1 }]);
});

test('Messages: stopping either side cancels an in-flight request', async () => {
  const { core, add, settle } = setup();
  const a = add('a');
  const b = add('b');
  // Handler never replies; only lifecycle teardown should resolve the request.
  b.Messages.handle('wait', () => {});

  const pending = a.Messages.request('b', 'wait', {});
  const assertion = assert.rejects(pending, /Resource stopped before message reply/);
  core.remove('b');
  await settle();
  await assertion;
});

// --- Internal boundedness guards for #5. ---
// request(resource, type, payload) waits forever and rejects only when a
// resource stops. That public contract is preserved: the timeout is an internal
// host setting that defaults to disabled, and the pending caps default high
// enough never to touch ordinary traffic. The tests below enable the guards
// explicitly with a fake clock and small caps to prove deadline, late/double
// reply handling, cleanup and a concrete concurrency bound (not 1000-player
// readiness, which requires measured load evidence).

function guardedSetup(options) {
  const queue = [];
  const timers = new Map();
  let nextTimer = 1;
  let now = 0;
  const core = new ResourceServices({
    defer: (fn) => queue.push(fn),
    setTimer: (fn, ms) => {
      const id = nextTimer;
      nextTimer += 1;
      timers.set(id, { fn, at: now + ms });
      return id;
    },
    clearTimer: (id) => timers.delete(id),
    ...options,
  });
  const add = (name, opts = {}) => core.add(name, normalizeManifest({ name, kcds: opts }));
  const settle = async () => {
    for (let i = 0; i < 20 && queue.length; i += 1) {
      for (const fn of queue.splice(0)) fn();
      await Promise.resolve();
    }
  };
  const advance = async (ms) => {
    now += ms;
    for (const [id, timer] of [...timers]) {
      if (timer.at <= now) {
        timers.delete(id);
        timer.fn();
      }
    }
    await settle();
  };
  return { core, add, settle, advance, timers };
}

test('Messages: default contract has no timeout and waits for a stop', async () => {
  const { core, add, settle } = guardedSetup();
  const a = add('a');
  const b = add('b');
  let settledFlag = 'pending';
  b.Messages.handle('wait', () => {});
  const pending = a.Messages.request('b', 'wait', {}).then(
    () => (settledFlag = 'resolved'),
    () => (settledFlag = 'rejected'),
  );
  await settle();
  assert.equal(settledFlag, 'pending', 'documented contract: a request without a reply waits');
  core.remove('b');
  await pending;
  assert.equal(settledFlag, 'rejected');
});

test('Messages: enabled per-request timeout rejects, then ignores a late reply', async () => {
  const { add, advance, timers } = guardedSetup({ requestTimeoutMs: 5000 });
  const a = add('a');
  const b = add('b');
  let captured;
  b.Messages.handle('slow', (_payload, reply) => {
    captured = reply; // never replies in time
  });

  const pending = a.Messages.request('b', 'slow', {});
  const assertion = assert.rejects(pending, /Message request timed out/);
  await advance(4000);
  assert.equal(timers.size, 1, 'deadline still armed before expiry');
  await advance(2000); // crosses the 5s deadline
  await assertion;
  assert.equal(timers.size, 0, 'deadline timer cleared on timeout');

  // A late reply after the deadline must be a no-op (no throw, no resolve).
  assert.doesNotThrow(() => captured('too late'));
});

test('Messages: a reply resolves once and a double reply is ignored', async () => {
  const { add, settle } = guardedSetup();
  const a = add('a');
  const b = add('b');
  b.Messages.handle('echo', (payload, reply) => {
    reply(payload);
    reply('second'); // ignored
  });
  const pending = a.Messages.request('b', 'echo', 'first');
  await settle();
  assert.equal(await pending, 'first');
});

test('Messages: bounded in-flight requests apply backpressure per resource', async () => {
  const { core, add, settle } = guardedSetup({ maxPendingRequestsPerResource: 2 });
  const a = add('a');
  const b = add('b');
  b.Messages.handle('hold', () => {}); // never replies: entries stay in flight

  // Two requests from 'a' are accepted and held in flight.
  const first = a.Messages.request('b', 'hold', {});
  const second = a.Messages.request('b', 'hold', {});
  assert.equal(core.pending.size, 2, 'exactly the bound is in flight, counted not estimated');

  // The third exceeds the per-resource cap and rejects predictably without
  // enqueueing anything (no unbounded queue).
  await assert.rejects(
    a.Messages.request('b', 'hold', {}),
    /Pending request limit reached for resource/,
  );
  assert.equal(core.pending.size, 2, 'rejected request was not enqueued');

  // Stopping the target cancels the two held requests and releases the slots.
  const drained = Promise.allSettled([first, second]);
  core.remove('b');
  await settle();
  const outcomes = await drained;
  assert.deepEqual(
    outcomes.map((o) => o.status),
    ['rejected', 'rejected'],
  );
  assert.equal(core.pending.size, 0);
  assert.equal(core.pendingPerSource.size, 0, 'per-source counters released on cleanup');
});

test('Messages: a global pending cap also rejects predictably', async () => {
  const { core, add } = guardedSetup({ maxPendingRequests: 1 });
  const a = add('a');
  const b = add('b');
  b.Messages.handle('hold', () => {});
  a.Messages.request('b', 'hold', {});
  await assert.rejects(a.Messages.request('b', 'hold', {}), /Pending request limit reached/);
  assert.equal(core.pending.size, 1);
});

test('Messages between client scripts: a target started later answers once it handles', async () => {
  const queue = [];
  const core = new ResourceServices({ side: 'client', defer: (fn) => queue.push(fn) });
  const add = (name, options = {}) => core.add(name, normalizeManifest({ name, kcds: options }));
  const settle = async () => {
    for (let i = 0; i < 20 && queue.length; i += 1) {
      for (const fn of queue.splice(0)) fn();
      await Promise.resolve();
    }
  };
  const ui = add('ui');
  // Asked before the other client script started: not available, as the docs say.
  await assert.rejects(ui.Messages.request('proximity', 'near', {}), /Resource is not available/);
  const proximity = add('proximity');
  proximity.Messages.handle('near', (payload, reply) => reply(['Henry']));
  const pending = ui.Messages.request('proximity', 'near', {});
  await settle();
  assert.deepEqual(await pending, ['Henry']);
});
