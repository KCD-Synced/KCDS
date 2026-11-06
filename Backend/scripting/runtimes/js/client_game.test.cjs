'use strict';

// The client's Game global: Game.quit closes the game.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceGame } = require('../../../../Client/resources/game.cjs');
const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');

test('client Game.quit asks the game to close', () => {
  const sent = [];
  const api = new ResourceGame({ send: (message) => sent.push(message) }).facade();
  api.quit();
  assert.deepEqual(sent, [{ op: 'gameQuit' }]);
  assert.equal(Object.isFrozen(api), true);
});

test('client scripts have the Game global', () => {
  assert.equal(typeof BrowserResourceRuntime, 'function');
  const source = require('node:fs').readFileSync(
    require.resolve('../../../../Client/resources/runtime.cjs'),
    'utf8',
  );
  assert.match(source, /Game: this\.game\.facade\(\)/);
});
