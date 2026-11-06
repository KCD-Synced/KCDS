'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { EntityStateStore } = require('./shared_state.cjs');

function server(options = {}) {
  const store = new EntityStateStore(options);
  store.attach(1);
  return { store, bag: store.bag(1) };
}

test('state stores JSON copies, null and primitive values; keys sort and prototype keys remain data', () => {
  const { store, bag } = server();
  assert.throws(() => new store.SharedState(), /obtained/);
  const input = { nested: [1] };
  assert.equal(bag.set('z', input), true);
  input.nested.push(2);
  assert.deepEqual(bag.get('z'), { nested: [1] });
  bag.get('z').nested.push(3);
  assert.deepEqual(bag.get('z'), { nested: [1] });
  bag.set('a', null);
  bag.set('__proto__', { data: true });
  const copied = bag.toObject();
  assert.equal(Object.getPrototypeOf(copied), Object.prototype);
  assert.deepEqual(copied.__proto__, { data: true });
  assert.deepEqual(bag.keys(), ['__proto__', 'a', 'z']);
  assert.equal(bag.has('a'), true);
  assert.equal(bag.get('a'), null);
  assert.equal(bag.get('missing'), undefined);
  assert.equal(bag.set('z', { nested: [1] }), false);
  assert.equal(bag.remove('a'), true);
  assert.equal(bag.remove('a'), false);
  assert.equal(copied.a, null);
});

test('UTF-8 key/value and key-count boundaries reject writes atomically', () => {
  const { bag } = server();
  bag.set('č'.repeat(32), 'č'.repeat(2048));
  assert.throws(() => bag.set('č'.repeat(33), 1), /64 UTF-8/);
  assert.throws(() => bag.set('long', 'č'.repeat(2049)), /4096/);
  assert.throws(() => bag.set('long', { value: 'x'.repeat(4096) }), /4096/);
  assert.throws(() => bag.set('bad', undefined), /serialize/);
  assert.throws(() => bag.set('bad', Infinity), /finite/);
  const circular = {};
  circular.self = circular;
  assert.throws(() => bag.set('bad', circular), TypeError);
  for (let index = 1; index < 128; index++) bag.set(String(index), index);
  assert.equal(bag.keys().length, 128);
  assert.throws(() => bag.set('overflow', 1), /128/);
  assert.equal(bag.set('1', 2), true);
  assert.equal(bag.has('overflow'), false);
});

test('per-view snapshots narrow scope, transfer owner keys and never expose server storage', () => {
  const { store, bag } = server();
  const viewer = new EntityStateStore({ side: 'client' });
  viewer.attach(1);
  const remote = viewer.bag(1);
  assert.equal(typeof remote.set, 'undefined');
  assert.equal(typeof remote.remove, 'undefined');
  bag.set('team', 'red');
  bag.set('wanted', 3, { scope: 'owner' });
  bag.set('secret', 'private', { scope: 'server' });
  viewer.replace(1, store.project(1, true));
  assert.deepEqual(remote.toObject(), { team: 'red', wanted: 3 });
  const changes = [];
  remote.onChange(null, (...args) => changes.push(args));
  viewer.replace(1, store.project(1, false));
  assert.deepEqual(changes, [['wanted', undefined, 3]]);
  bag.set('team', 'red', { scope: 'owner' });
  viewer.replace(1, store.project(1, false));
  assert.deepEqual(remote.keys(), []);
  viewer.replace(1, store.project(1, true));
  assert.deepEqual(remote.toObject(), { team: 'red', wanted: 3 });
  assert.equal(bag.set('team', 'red', { scope: 'owner' }), false);
  assert.throws(() => bag.set('team', 0, { scope: 'unknown' }), /scope/);
  assert.throws(() => store.replace(1, []), /Server cannot/);
});

test('subscriptions are filtered, synchronous and resource-owned; reload preserves values', () => {
  const active = new Set();
  const first = {},
    replacement = {};
  active.add(first);
  const changes = [],
    global = [];
  const { store, bag } = server({
    ownerAlive: (owner) => owner === undefined || active.has(owner),
    onChange: (...args) => global.push(args),
  });
  const owned = store.bag(1, first);
  owned.onChange('score', (...args) => changes.push(args));
  bag.set('unrelated', 1);
  bag.set('score', null);
  bag.set('score', 2);
  bag.remove('score');
  assert.deepEqual(changes, [
    ['score', null, undefined],
    ['score', 2, null],
    ['score', undefined, 2],
  ]);
  // The store-level callback feeds EventMap.entityStateChange: (id, key, value, previous).
  assert.deepEqual(global.at(-1), [1, 'score', undefined, 2]);
  bag.set('score', 4);
  store.removeResource(first);
  active.delete(first);
  active.add(replacement);
  const restored = store.bag(1, replacement);
  assert.equal(restored.get('score'), 4);
  const length = changes.length;
  bag.set('score', 5);
  assert.equal(changes.length, length);
  assert.throws(() => owned.onChange(null, () => {}), /stopped/);
  assert.throws(() => owned.set('score', 6), /stopped/);
});

test('destroy and reconnect invalidate old bag handles even when a numeric ID is reused', () => {
  const { store, bag } = server();
  let calls = 0;
  bag.onChange(null, () => calls++);
  bag.set('score', 1);
  store.destroy(1);
  store.attach(1);
  const next = store.bag(1);
  assert.equal(next.get('score'), undefined);
  assert.throws(() => bag.get('score'), /no longer exists/);
  assert.throws(() => bag.set('score', 2), /no longer exists/);
  next.set('score', 3);
  assert.equal(calls, 1);
  store.disconnect();
  assert.throws(() => next.keys(), /no longer exists/);
});

test('one failing listener cannot swallow others; reentrant writes remain ordered and copied', () => {
  const errors = [],
    observed = [];
  const { bag } = server({ onError: (error) => errors.push(error.message) });
  bag.onChange('value', () => {
    throw new Error('listener failed');
  });
  bag.onChange('value', (key, value) => {
    value.count = 99;
    bag.set('nested', 2);
  });
  bag.onChange(null, (key, value) => observed.push([key, value]));
  bag.set('value', { count: 1 });
  assert.deepEqual(observed, [
    ['value', { count: 1 }],
    ['nested', 2],
  ]);
  assert.deepEqual(errors, ['listener failed']);
  assert.deepEqual(bag.get('value'), { count: 1 });
});

test('malformed client snapshots leave existing state untouched and never notify partially', () => {
  const store = new EntityStateStore({ side: 'client' });
  store.attach(1);
  const bag = store.bag(1);
  store.replace(1, [{ key: 'score', kind: 'number', value: 1 }]);
  const changes = [];
  bag.onChange(null, (...args) => changes.push(args));
  const valid = { key: 'score', kind: 'number', value: 2 };
  for (const invalid of [
    { key: 'bad', kind: 'json', value: '{' },
    { key: 'bad', kind: 'number', value: '2' },
    { key: 'bad', kind: 'json', value: '"' + 'x'.repeat(4097) + '"' },
    valid,
  ]) {
    assert.throws(() => store.replace(1, [valid, invalid]));
    assert.equal(bag.get('score'), 1);
    assert.deepEqual(changes, []);
  }
});
