'use strict';

// a resource holding the shared event loop is logged.

const test = require('node:test');
const assert = require('node:assert/strict');

const { watchEventLoop } = require('./event_loop.cjs');

test('a long hold of the event loop is logged with the last resource', async () => {
  const logs = [];
  const stop = watchEventLoop((text) => logs.push(text), {
    thresholdMs: 50,
    everyMs: 300,
    lastResource: () => 'shop',
  });
  await new Promise((resolve) => setTimeout(resolve, 20));
  const until = Date.now() + 150;
  while (Date.now() < until) {
    // a resource computing without yielding
  }
  await new Promise((resolve) => setTimeout(resolve, 400));
  stop();
  assert.equal(logs.length, 1, JSON.stringify(logs));
  assert.match(logs[0], /held for \d+ ms in the last \d+ s \(last resource code: shop\)/);
});

// A resource computing without yielding for ms.
function hold(ms) {
  const until = Date.now() + ms;
  while (Date.now() < until) {
    // busy
  }
}

test('a hold already logged with its resource is not reported again', async () => {
  const logs = [];
  const stop = watchEventLoop((text) => logs.push(text), {
    thresholdMs: 50,
    everyMs: 300,
    named: () => 150,
  });
  await new Promise((resolve) => setTimeout(resolve, 20));
  hold(150);
  await new Promise((resolve) => setTimeout(resolve, 400));
  stop();
  assert.deepEqual(logs, []);
});

test('a hold no logged hold explains is reported, with what is done about it', async () => {
  const logs = [];
  const seen = [];
  const stop = watchEventLoop((text) => logs.push(text), {
    thresholdMs: 50,
    everyMs: 300,
    named: () => 0,
    unnamed: (ms) => {
      seen.push(ms);
      return '; tracing';
    },
  });
  await new Promise((resolve) => setTimeout(resolve, 20));
  hold(150);
  await new Promise((resolve) => setTimeout(resolve, 400));
  stop();
  assert.equal(logs.length, 1, JSON.stringify(logs));
  assert.match(logs[0], /waited; tracing$/);
  assert.ok(seen[0] >= 100, `measured ${seen[0]} ms`);
});

test('a free event loop logs nothing', async () => {
  const logs = [];
  const stop = watchEventLoop((text) => logs.push(text), { thresholdMs: 100, everyMs: 100 });
  await new Promise((resolve) => setTimeout(resolve, 350));
  stop();
  assert.deepEqual(logs, []);
});
