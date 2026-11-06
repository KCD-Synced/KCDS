'use strict';

// The client Prefab (Client/resources/prefabs.cjs, World Architect): the game's prefabs a
// client script builds in this game only.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourcePrefabs, FIRST_ID } = require('../../../../Client/resources/prefabs.cjs');

const DOBESOVICKO = 'structures/living/houses/unique/dobesovicko/house_1s_4r_angled_a_dobesovicko';

function setup() {
  const sent = [];
  const prefabs = new ResourcePrefabs({ send: (message) => sent.push(message) });
  return {
    prefabs,
    sent,
    Prefab: prefabs.facade('editor'),
    Other: prefabs.facade('other'),
  };
}

test('place builds a prefab by its name with a local id; moves and destroy go down', () => {
  const { sent, Prefab } = setup();
  const house = Prefab.place('house_1s_4r_angled_a_dobesovicko', { x: 10, y: 20, z: 30 });
  assert.equal(house.id, FIRST_ID);
  assert.equal(house.valid, true);
  assert.equal(house.name, DOBESOVICKO);
  assert.deepEqual(sent[0], {
    op: 'localPrefab',
    kind: 'spawn',
    id: FIRST_ID,
    template: '551ac3b8-8105-42b8-93c0-6541cf948ddf',
    position: { x: 10, y: 20, z: 30 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
  });

  house.position = { x: 11, y: 20, z: 30 };
  assert.equal(sent.at(-1).kind, 'move');
  assert.deepEqual(sent.at(-1).position, { x: 11, y: 20, z: 30 });
  assert.equal(house.position.x, 11);
  house.setPose({ x: 12, y: 20, z: 30 }, { x: 0, y: 0, z: 90 });
  assert.equal(sent.at(-1).kind, 'move');
  assert.ok(Math.abs(sent.at(-1).rotation.z - Math.SQRT1_2) < 1e-6);

  house.destroy();
  house.destroy();
  assert.equal(house.valid, false);
  assert.equal(house.position.x, 12, 'a gone prefab still reads its last place');
  assert.deepEqual(sent.at(-1), { op: 'localPrefab', kind: 'destroy', id: FIRST_ID });
  const count = sent.length;
  house.position = { x: 0, y: 0, z: 0 };
  assert.equal(sent.length, count, 'a gone prefab sends nothing');
});

test('a house with prefab doors is placed; unknown and unbuildable prefabs throw', () => {
  const { Prefab } = setup();
  assert.equal(
    Prefab.place('house_1s_2r_a', { x: 0, y: 0, z: 0 }).name,
    'structures/living/houses/house_1s_2r_a',
  );
  assert.throws(() => Prefab.place('no_such_prefab', { x: 0, y: 0, z: 0 }), /does not carry/);
  assert.throws(() => Prefab.place('wild_beehive_01', { x: 0, y: 0, z: 0 }), /does not have/);
  assert.throws(() => Prefab.place('', { x: 0, y: 0, z: 0 }), TypeError);
  assert.throws(() => Prefab.place('house_1s_2r_a', { x: 1e9, y: 0, z: 0 }), RangeError);
  assert.throws(() => Prefab.place('house_1s_2r_a'), TypeError);
  assert.throws(() => new Prefab(), TypeError);
});

test('find lists buildable prefabs by part of their path; all and a stop are per resource', () => {
  const { prefabs, Prefab, Other } = setup();
  const houses = Prefab.find('living/houses/house_1s', 5);
  assert.equal(houses.length, 5);
  assert.ok(houses.every((path) => path.includes('living/houses/house_1s')));
  assert.ok(Prefab.find('DOBESOVICKO').includes(DOBESOVICKO));
  assert.deepEqual(Prefab.find('wild_beehive_01'), []);
  assert.throws(() => Prefab.find(3), TypeError);

  const mine = Prefab.place('house_1s_2r_a', { x: 0, y: 0, z: 0 });
  const theirs = Other.place('house_1s_2r_a', { x: 5, y: 0, z: 0 });
  assert.deepEqual(Prefab.all(), [mine]);
  prefabs.removeOwner('editor');
  assert.equal(mine.valid, false);
  assert.equal(theirs.valid, true);
});
