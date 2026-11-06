'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceBuffs } = require('../../../../Client/resources/buffs.cjs');
const { ClientEntities } = require('../../../../Client/resources/entities.cjs');

const MARIGOLD = '8503216a-a34c-49f0-aefa-54d4502046f9';
const HANGOVER = '7e252c71-5e41-472c-ad32-f223a664faab';

function record(id) {
  return {
    id,
    nativeId: id,
    kind: 'player',
    dimension: 0,
    position: { x: id, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  };
}

test('client Player.buffs: the local player only, from the native client', () => {
  let now = 50;
  const buffs = new ResourceBuffs({ clock: () => now });
  const entities = new ClientEntities({ localBuffs: () => buffs.local() });
  entities.updateDirectory({ entities: [record(1), record(2)], localPlayerId: 1 });
  const api = entities.facade();
  assert.deepEqual(api.LocalPlayer.buffs, []);
  buffs.receiveLocal({
    buffs: [
      [HANGOVER, '142339357715286057', false, 2],
      [MARIGOLD, '7', true, 0],
      ['00000000-0000-0000-0000-000000000000', '8', false, 0],
    ],
  });
  now += 1.5;
  assert.deepEqual(api.LocalPlayer.buffs, [
    {
      name: 'hangover',
      class: 'hangover',
      aiTag: 'alcohol_hangover',
      duration: 6.6667,
      custom: false,
      displayName: null,
      description: null,
      since: 3.5,
      source: 'native',
    },
    {
      name: 'potion_marigold_decoction',
      class: 'potion',
      aiTag: null,
      duration: 60,
      custom: false,
      displayName: null,
      description: null,
      since: 1.5,
      source: 'server',
    },
  ]);
  assert.deepEqual(new api.Player(2).buffs, []);
  // A malformed message changes nothing; a reset empties the list.
  buffs.receiveLocal({ buffs: [[MARIGOLD, 7, true, 0]] });
  assert.equal(api.LocalPlayer.buffs.length, 2);
  buffs.reset();
  assert.deepEqual(api.LocalPlayer.buffs, []);
});
