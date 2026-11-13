'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const { Watermark } = require('./hud/watermark.cjs');

function hostWith(players) {
  const sent = [];
  const host = {
    players: new Map(players.map((player) => [player.id, player])),
    bridge: { send: (message) => sent.push(message) },
  };
  return { host, sent };
}

test('Watermark.setCorner moves every player without their own corner', () => {
  const first = { id: 1, ready: true };
  const second = { id: 2, ready: true };
  const waiting = { id: 3, ready: false };
  const { host, sent } = hostWith([first, second, waiting]);
  const watermark = new Watermark(host);
  const api = watermark.api();
  assert.equal(api.corner, 'bottomLeft');
  watermark.setFor(second, 'topLeft');
  sent.length = 0;
  api.setCorner('topRight');
  assert.equal(api.corner, 'topRight');
  // The corners by number: 0 bottom left, 1 bottom right, 2 top right, 3 top left.
  assert.deepEqual(sent, [{ op: 'watermarkCorner', id: 1, corner: 2 }]);
  // The same corner again tells nobody.
  sent.length = 0;
  api.setCorner('topRight');
  assert.deepEqual(sent, []);
  assert.throws(() => api.setCorner('middle'), TypeError);
  assert.throws(() => api.setCorner(null), TypeError);
});

test("player.setWatermarkCorner overrides the server's corner until null", () => {
  const player = { id: 7, ready: true };
  const { host, sent } = hostWith([player]);
  const watermark = new Watermark(host);
  watermark.setFor(player, 'bottomRight');
  assert.equal(watermark.cornerFor(player), 'bottomRight');
  assert.deepEqual(sent, [{ op: 'watermarkCorner', id: 7, corner: 1 }]);
  watermark.setFor(player, null);
  assert.equal(watermark.cornerFor(player), 'bottomLeft');
  assert.deepEqual(sent.at(-1), { op: 'watermarkCorner', id: 7, corner: 0 });
  // No way to turn it off.
  assert.throws(() => watermark.setFor(player, false), TypeError);
  assert.throws(() => watermark.setFor(player, 'hidden'), TypeError);
});

test('a player whose world gets ready hears a corner only when it is not the bottom left one', () => {
  const player = { id: 4, ready: true };
  const { host, sent } = hostWith([player]);
  const watermark = new Watermark(host);
  watermark.arrived(player);
  assert.deepEqual(sent, []);
  watermark.api().setCorner('topLeft');
  sent.length = 0;
  watermark.arrived(player);
  assert.deepEqual(sent, [{ op: 'watermarkCorner', id: 4, corner: 3 }]);
});
