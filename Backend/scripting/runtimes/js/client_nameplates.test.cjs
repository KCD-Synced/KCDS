'use strict';

// The client's Nameplates global: the local view switches and the labels.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceNameplates } = require('../../../../Client/resources/nameplates.cjs');

function fixture() {
  const sent = [];
  const nameplates = new ResourceNameplates({ send: (message) => sent.push(message) });
  return { nameplates, sent };
}

test('client Nameplates: the view switches are local and go down whole', () => {
  const { nameplates, sent } = fixture();
  const api = nameplates.facade('hud');
  assert.equal(api.isVisible(), true);
  assert.equal(api.isHealthVisible(), true);
  assert.equal(api.isSelfVisible(), false);
  api.setVisible(false);
  api.setHealthVisible(false);
  api.setSelfVisible(true);
  assert.deepEqual(sent, [
    { op: 'nameplateView', visible: false, health: true, name: true, self: false },
    { op: 'nameplateView', visible: false, health: false, name: true, self: false },
    { op: 'nameplateView', visible: false, health: false, name: true, self: true },
  ]);
  assert.equal(api.isVisible(), false);
  assert.equal(api.isSelfVisible(), true);
  assert.throws(() => api.setVisible('yes'), /setVisible takes a boolean/);
});

test('client Nameplates: the mode shows the name, the health bar, both or neither', () => {
  const { nameplates, sent } = fixture();
  const api = nameplates.facade('hud');
  assert.equal(api.getMode(), 'both');
  api.setMode('health');
  assert.deepEqual(sent.at(-1), {
    op: 'nameplateView',
    visible: true,
    health: true,
    name: false,
    self: false,
  });
  assert.equal(api.isNameVisible(), false);
  assert.equal(api.getMode(), 'health');
  api.setMode('name');
  assert.equal(api.isHealthVisible(), false);
  assert.equal(api.getMode(), 'name');
  api.setNameVisible(false);
  assert.equal(api.getMode(), 'none');
  api.setMode('both');
  assert.equal(api.getMode(), 'both');
  assert.throws(() => api.setMode('hp'), /setMode takes one of/);
  assert.throws(() => api.setNameVisible(1), /setNameVisible takes a boolean/);
});

test('client Nameplates: labels with their defaults, cleared one by one, all, or with their resource', () => {
  const { nameplates, sent } = fixture();
  const speech = nameplates.facade('speech');
  const status = nameplates.facade('status');
  speech.setLabel(3, 'Hello there');
  speech.setLabel(4, 'Ouch', -1, 0xffff0000);
  status.setLabel(5, 'AFK', 1500.4);
  assert.deepEqual(sent, [
    { op: 'nameplateLabel', entity: 3, text: 'Hello there', durationMs: 6000, color: 0 },
    { op: 'nameplateLabel', entity: 4, text: 'Ouch', durationMs: 0, color: 0xffff0000 },
    { op: 'nameplateLabel', entity: 5, text: 'AFK', durationMs: 1500, color: 0 },
  ]);
  sent.length = 0;
  speech.clearLabel(3);
  nameplates.removeOwner('speech');
  assert.deepEqual(sent, [
    { op: 'nameplateLabel', entity: 3, text: '', durationMs: 0, color: 0 },
    { op: 'nameplateLabel', entity: 4, text: '', durationMs: 0, color: 0 },
  ]);
  sent.length = 0;
  status.clearLabels();
  assert.deepEqual(sent, [{ op: 'nameplateLabelsClear' }]);
  assert.throws(() => status.setLabel(0, 'x'), /entity id/);
  assert.throws(() => status.setLabel(1, 'x'.repeat(300)), /at most 256/);
  assert.throws(() => status.setLabel(1, 'x', 10, -5), /0xAARRGGBB/);
});
