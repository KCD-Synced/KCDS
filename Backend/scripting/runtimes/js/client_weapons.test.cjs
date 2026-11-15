'use strict';

// The client's Weapons global: the game's answers and the actions' ends.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceWeapons } = require('../../../../Client/resources/weapons.cjs');

function fixture(answer) {
  const asked = [];
  const events = [];
  const sync = {
    available: true,
    call(request) {
      asked.push(request);
      return typeof answer === 'function' ? answer(request) : answer;
    },
  };
  const weapons = new ResourceWeapons({ sync, emit: (name, args) => events.push([name, ...args]) });
  return { api: weapons.facade(), weapons, asked, events };
}

test('Weapons.draw asks the game for the set and answers null once it started', () => {
  const { api, asked } = fixture('started');
  assert.equal(api.draw(), null);
  assert.equal(api.holster(), null);
  assert.equal(api.toggle(), null);
  assert.deepEqual(asked, [
    { op: 'weaponsSet', how: 'draw' },
    { op: 'weaponsSet', how: 'holster' },
    { op: 'weaponsSet', how: 'toggle' },
  ]);
});

test('Weapons.select picks a set from 0 to 3', () => {
  const { api, asked } = fixture('started');
  assert.equal(api.select(2), null);
  assert.deepEqual(asked, [{ op: 'weaponsSelect', slot: 2 }]);
  assert.throws(() => api.select(4), RangeError);
  assert.throws(() => api.select(1.5), RangeError);
  assert.equal(fixture('notAllowed').api.select(3), 'notAllowed');
});

test('Weapons torch calls are apart from the weapon set', () => {
  const { api, asked } = fixture('started');
  assert.equal(api.toggleTorch(), null);
  assert.equal(api.drawTorch(), null);
  assert.equal(api.holsterTorch(), null);
  assert.deepEqual(
    asked.map((request) => request.op),
    ['weaponsTorch', 'weaponsTorch', 'weaponsTorch'],
  );
});

test("Weapons gives the game's reason, and no answer is unavailable", () => {
  assert.equal(fixture('noWeapon').api.draw(), 'noWeapon');
  assert.equal(fixture('alreadyDrawn').api.draw(), 'alreadyDrawn');
  assert.equal(fixture('notDrawn').api.holsterTorch(), 'notDrawn');
  assert.equal(fixture(undefined).api.draw(), 'unavailable');
  assert.equal(fixture('somethingNew').api.toggle(), 'unavailable');
  const none = fixture('started');
  none.weapons.sync = { available: false, call: () => 'started' };
  assert.equal(none.api.draw(), 'unavailable');
});

test('Weapons.state reads the game, null outside the world', () => {
  const reply = {
    available: true,
    slot: 2,
    hasWeapon: true,
    drawn: false,
    allowed: true,
    torch: true,
    busy: false,
  };
  const { api } = fixture(reply);
  assert.deepEqual(
    { ...api.state() },
    { slot: 2, hasWeapon: true, drawn: false, allowed: true, torch: true, busy: false },
  );
  assert.equal(fixture(null).api.state(), null);
  assert.equal(fixture({ available: false }).api.state(), null);
});

test('Weapons tells when an action ended', () => {
  const { weapons, events } = fixture('started');
  weapons.receiveEvent({ action: 'draw', ok: true, reason: '' });
  weapons.receiveEvent({ action: 'torchDraw', ok: false, reason: 'refused' });
  weapons.receiveEvent({ action: 7 });
  assert.deepEqual(events, [
    ['weaponActionEnded', 'draw', true, ''],
    ['weaponActionEnded', 'torchDraw', false, 'refused'],
  ]);
});
