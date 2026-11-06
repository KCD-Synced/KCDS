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
  assert.match(logs[0], /held for \d+ ms \(last resource code: shop\)/);
});

test('a free event loop logs nothing', async () => {
  const logs = [];
  const stop = watchEventLoop((text) => logs.push(text), { thresholdMs: 100, everyMs: 100 });
  await new Promise((resolve) => setTimeout(resolve, 350));
  stop();
  assert.deepEqual(logs, []);
});
