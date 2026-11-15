'use strict';

// The time a Lua call may run: 5 seconds, or the server's luaCallLimit (KCDS_LUA_CALL_LIMIT).

const test = require('node:test');
const assert = require('node:assert/strict');
const { runLimitMs } = require('./languages/lua_host.cjs');

test('a Lua call runs 5 seconds unless the server sets luaCallLimit', (t) => {
  t.mock.method(console, 'warn', () => {});
  assert.equal(runLimitMs(undefined), 5000);
  assert.equal(runLimitMs({}), 5000);
  assert.equal(runLimitMs({ KCDS_LUA_CALL_LIMIT: '' }), 5000);
  assert.equal(runLimitMs({ KCDS_LUA_CALL_LIMIT: '30' }), 30000);
  assert.equal(runLimitMs({ KCDS_LUA_CALL_LIMIT: '1.5' }), 1500);
  assert.equal(runLimitMs({ KCDS_LUA_CALL_LIMIT: '600' }), 600000);
  // Out of range or not a number: the 5 seconds stay, with a warning.
  for (const bad of ['0', '-3', '601', 'ten', 'Infinity']) {
    assert.equal(runLimitMs({ KCDS_LUA_CALL_LIMIT: bad }), 5000, bad);
  }
  assert.equal(console.warn.mock.callCount(), 5);
});
