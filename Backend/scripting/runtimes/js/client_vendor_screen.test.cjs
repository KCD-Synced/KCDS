'use strict';

// The client events of a server vendor's trade screen (Client/resources/vendor_screen.cjs):
// vendorOpened once the screen came up, then exactly one vendorClosed.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceVendorScreen } = require('../../../../Client/resources/vendor_screen.cjs');

function setup() {
  const emitted = [];
  const screen = new ResourceVendorScreen({ emit: (name, args) => emitted.push([name, ...args]) });
  return { screen, emitted };
}

test('a screen that came up raises vendorOpened and then one vendorClosed', () => {
  const { screen, emitted } = setup();
  screen.receiveEvent({ session: 4, name: 'opened', npc: 4100, reason: '' });
  screen.receiveEvent({ session: 4, name: 'opened', npc: 4100, reason: '' });
  screen.receiveEvent({ session: 4, name: 'closed', npc: 4100, reason: 'server' });
  screen.receiveEvent({ session: 4, name: 'closed', npc: 4100, reason: 'player' });
  assert.deepEqual(emitted, [
    ['vendorOpened', 4, 4100],
    ['vendorClosed', 4, 'server'],
  ]);
});

test('nobody keeping the shop reads as null, and an unknown reason as the player', () => {
  const { screen, emitted } = setup();
  screen.receiveEvent({ session: 9, name: 'opened', npc: null, reason: '' });
  screen.receiveEvent({ session: 9, name: 'closed', npc: null, reason: 'whatever' });
  assert.deepEqual(emitted, [
    ['vendorOpened', 9, null],
    ['vendorClosed', 9, 'player'],
  ]);
});

test('a screen that never came up raises nothing', () => {
  const { screen, emitted } = setup();
  screen.receiveEvent({ session: 2, name: 'closed', npc: null, reason: 'levelChange' });
  screen.receiveEvent({ session: 0, name: 'opened', npc: null, reason: '' });
  assert.deepEqual(emitted, []);
});
