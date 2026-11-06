'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceServices } = require('./services.cjs');
const { normalizeManifest } = require('./manifest.cjs');

function setup() {
  const logs = [];
  const queue = [];
  const sent = [];
  const core = new ResourceServices({
    log: (...args) => logs.push(args),
    send: (message) => sent.push(message),
    defer: (fn) => queue.push(fn),
  });
  const add = (name, options = {}) => core.add(name, normalizeManifest({ name, kcds: options }));
  const a = add('a', { exports: ['value', 'fn'] });
  const b = add('b', { requires: ['a'] });
  const tick = () => {
    for (const fn of queue.splice(0)) fn();
  };
  return { core, a, b, add, tick, logs, sent };
}

test('shared, directed and private events respect resource ownership and off', async () => {
  const { a, b } = setup();
  const results = [];
  const shared = (value) => results.push('a:' + value);
  a.Events.on('same', shared);
  b.Events.on('same', shared);
  a.Events.listen('same', () => results.push('private-a'));
  b.Events.listen('same', () => results.push('private-b'));
  await a.Events.toResource('b', 'same', 1);
  await a.Events.fire('same');
  assert.deepEqual(results, ['a:1', 'private-a']);
  a.Events.off('same', shared);
  assert.equal(b.Events.listenerCount('same'), 1);
  await a.Events.emit('same', 2);
  assert.deepEqual(results, ['a:1', 'private-a', 'a:2']);
});

test('once is removed before recursive emit, removed handlers do not run from snapshots', async () => {
  const { core, a, b } = setup();
  let calls = 0;
  a.Events.once('once', () => {
    calls++;
    return a.Events.emit('once');
  });
  await a.Events.emit('once');
  assert.equal(calls, 1);
  assert.equal(a.Events.listenerCount('once'), 0);
  a.Events.on('stop', () => core.remove('b'));
  b.Events.on('stop', () => {
    throw new Error('Must not run after stop');
  });
  await a.Events.emit('stop');
});

test('emit waits for all handlers and aggregates sync and async errors', async () => {
  const { a, b, logs } = setup();
  let completed = false;
  a.Events.on('fail', () => {
    throw new Error('sync');
  });
  b.Events.on('fail', async () => {
    await Promise.resolve();
    throw new Error('async');
  });
  b.Events.on('fail', async () => {
    await Promise.resolve();
    completed = true;
  });
  await assert.rejects(a.Events.emit('fail'), (error) => {
    assert.equal(completed, true);
    assert.equal(error.errors.length, 2);
    return error instanceof AggregateError;
  });
  // Only the synchronous throw is logged, with its stack.
  assert.equal(logs.length, 1);
  assert.equal(logs[0][0], 'a');
  assert.match(logs[0][1], /^Error: sync\n\s+at /);
});

test('message requests resolve on the next tick with only the first reply', async () => {
  const { a, b, tick } = setup();
  b.Messages.handle('ask', (payload, reply) => {
    reply(payload + 1);
    reply(999);
  });
  let resolved = false;
  const pending = a.Messages.request('b', 'ask', 41).then((value) => {
    resolved = true;
    return value;
  });
  tick();
  await Promise.resolve();
  assert.equal(resolved, false);
  tick();
  assert.equal(await pending, 42);
});

test('message delivery errors and stops reject; async rejection does not invent a reply timeout', async () => {
  const { core, a, b, tick } = setup();
  await assert.rejects(a.Messages.request('missing', 'ask'), /Resource is not available/);
  await assert.rejects(a.Messages.request('b', 'ask'), /Resource is not available/);
  b.Messages.handle('other', () => {});
  await assert.rejects(a.Messages.request('b', 'ask'), /does not handle this message type/);
  b.Messages.handle('throw', () => {
    throw new Error('handler error');
  });
  const failure = a.Messages.request('b', 'throw');
  tick();
  await assert.rejects(failure, /handler error/);
  b.Messages.handle('pending', async () => {
    throw new Error('async failure');
  });
  const pending = a.Messages.request('b', 'pending');
  tick();
  await Promise.resolve();
  assert.equal(core.pending.size, 1);
  core.remove('a');
  await assert.rejects(pending, /Resource stopped/);
});

test('notification reply is ignored, replaced handlers win and stopped sources cancel delivery', () => {
  const { core, a, b, tick } = setup();
  const values = [];
  b.Messages.handle('notice', () => values.push('old'));
  b.Messages.handle('notice', (payload, reply) => {
    values.push(payload);
    reply('ignored');
  });
  a.Messages.send('b', 'notice', 'new');
  tick();
  assert.deepEqual(values, ['new']);
  a.Messages.send('b', 'notice', 'cancelled');
  core.remove('a');
  tick();
  assert.deepEqual(values, ['new']);
});

test('exports retain real values/functions, undeclared registration warns without storing', () => {
  const { core, a, b, add, logs } = setup();
  const value = { count: 1 };
  const fn = (number) => number + 1;
  assert.equal(a.Exports.register('value', value), true);
  a.Exports.register('fn', fn);
  assert.equal(b.Exports.get('a', 'value'), value);
  assert.equal(b.Imports.get('a').fn, fn);
  assert.equal(b.Exports.get('a', 'fn')(41), 42);
  assert.equal(a.Exports.register('undeclared', 42), true);
  assert.throws(() => b.Exports.get('a', 'undeclared'), /Export not found/);
  assert.equal(logs.length, 1);
  const c = add('c');
  c.Imports.get('a');
  assert.match(logs.at(-1)[1], /Undeclared resource dependency/);
  core.remove('a');
  assert.throws(() => b.Imports.get('a'), /Resource is not available/);
  assert.throws(() => a.Exports.register('value', 2), /Resource is stopped/);
});

test('a string payload that is not JSON is sent as a JSON string', () => {
  const { a, sent } = setup();
  a.Events.broadcast('text', 'hello');
  a.Events.broadcast('empty', '');
  a.Events.broadcast('json', '{"answer":42}');
  a.Events.broadcast('number', '7');
  assert.deepEqual(
    sent.map((message) => message.payload),
    ['"hello"', '""', '{"answer":42}', '7'],
  );
  assert.equal(JSON.parse(sent[0].payload), 'hello');
});

test('client events use a separate namespace and serialize wire payloads', async () => {
  const { core, a, sent } = setup();
  let native = 0;
  let client = 0;
  a.Events.on('playerReady', () => native++);
  const sender = { id: 7 };
  a.Events.fromClient('playerReady', (from, payload) => {
    client++;
    assert.equal(from, sender);
    assert.deepEqual(payload, {});
  });
  await core.emit('client', 'playerReady', [sender, {}]);
  assert.equal(native, 0);
  assert.equal(client, 1);
  a.Events.broadcast('test', { answer: 42 });
  assert.deepEqual(sent, [{ op: 'event', name: 'test', payload: '{"answer":42}' }]);
  const browser = new ResourceServices({ side: 'client' });
  const api = browser.add('mode', normalizeManifest({ name: 'mode' }));
  assert.equal(api.Events.fromClient, undefined);
  assert.equal(api.Runtime.isClient, true);
  assert.equal(a.Runtime.isServer, true);
  assert.equal(Object.isFrozen(a.Runtime), true);
});
