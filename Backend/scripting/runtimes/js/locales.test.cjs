'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict');
const { Locales, DEFAULTS } = require('./locales/locales.cjs');

// KCDS texts per language.

test('a player gets their language, else the English text, else the default', () => {
  const locales = new Locales();
  const czech = { language: 'czech' };
  const german = { language: 'German' };
  const unknown = { language: '' };
  const params = { thief: 'Tom' };
  assert.equal(
    locales.text(czech, 'pickpocket.victimNotice', params),
    'Tom tried to pick your pocket',
  );
  locales.set('czech', { 'pickpocket.victimNotice': '{thief} ti chtěl vybrat kapsu' });
  locales.set('english', { 'pickpocket.victimNotice': '{thief} reached for your purse' });
  assert.equal(locales.text(czech, 'pickpocket.victimNotice', params), 'Tom ti chtěl vybrat kapsu');
  assert.equal(
    locales.text(german, 'pickpocket.victimNotice', params),
    'Tom reached for your purse',
  );
  assert.equal(
    locales.text(unknown, 'pickpocket.victimNotice', params),
    'Tom reached for your purse',
  );
  // An empty text gives the key back to the default.
  locales.set('english', { 'pickpocket.victimNotice': '' });
  assert.equal(locales.get('pickpocket.victimNotice'), DEFAULTS['pickpocket.victimNotice']);
});

test('resources add their own keys; placeholders stay when not given', () => {
  const locales = new Locales();
  locales.set('czech', { 'shop.closed': 'Obchod {name} je zavřený' });
  assert.equal(locales.text({ language: 'czech' }, 'shop.closed', {}), 'Obchod {name} je zavřený');
  assert.equal(locales.text({ language: 'english' }, 'shop.closed'), 'shop.closed');
  assert.equal(locales.get('shop.closed', 'czech'), 'Obchod {name} je zavřený');
});

test('a locale is checked', () => {
  const locales = new Locales();
  assert.throws(() => locales.set('cz ech', {}));
  assert.throws(() => locales.set('czech', ['x']));
  assert.throws(() => locales.set('czech', { key: 3 }));
  assert.throws(() => locales.set('czech', { ['k'.repeat(129)]: 'x' }));
});
