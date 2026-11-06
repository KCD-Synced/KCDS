'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { PlaceNames } = require('./world/place_names.cjs');

function setup() {
  const sent = [];
  const inventories = {
    itemNames: new Map(),
    sendNames: (id, full, names) => sent.push({ id, full, names }),
  };
  return { places: new PlaceNames(inventories), inventories, sent };
}

test('a place is renamed on all its keys and comes back', () => {
  const { places, inventories, sent } = setup();
  assert.equal(places.set('troskovice', 'Zahradka'), true);
  assert.deepEqual(sent.pop().names, [
    ['location_troskovice', 'Zahradka'],
    ['ui_fac_trosecko_settlements_troskovice', 'Zahradka'],
  ]);
  assert.equal(inventories.itemNames.get('location_troskovice'), 'Zahradka');
  const row = places.list().find((place) => place.id === 'troskovice');
  assert.deepEqual(row, { id: 'troskovice', gameName: 'Troskowitz', name: 'Zahradka' });
  assert.equal(places.set('TROSKOVICE', null), true);
  assert.equal(inventories.itemNames.size, 0, 'a late joiner hears nothing of it');
  assert.equal(sent.pop().names[0][1], '');
  assert.equal(places.list().find((place) => place.id === 'troskovice').name, 'Troskowitz');
});

test('an unknown place is false, a bad name throws', () => {
  const { places, sent } = setup();
  assert.equal(places.set('atlantis', 'X'), false);
  assert.throws(() => places.set('troskovice', ''), TypeError);
  assert.throws(() => places.set('troskovice', 'a\nb'), TypeError);
  assert.throws(() => places.set(7, 'X'), TypeError);
  assert.equal(sent.length, 0);
});
