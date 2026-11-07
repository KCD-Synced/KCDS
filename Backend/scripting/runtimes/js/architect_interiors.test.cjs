'use strict';

// The game's own interiors (Client/resources/architect/interiors.cjs): a building's probes, room
// shapes and doors, grouped by the building their names carry.
const test = require('node:test');
const assert = require('node:assert/strict');

const {
  interiorsOf,
  interiorOfModel,
  roomAt,
  buildingOf,
  labelOf,
} = require('../../../../Client/resources/architect/interiors.cjs');

test("a building's own rooms only: a closed house beside one with rooms has none", () => {
  // As at Kuttenberg: vlasska_05 (closed) 14 m from house_city_2s_noarc_e4 (rooms).
  const cityHouse = {
    building: 'structures/living/houses/house_city_2s_noarc_e4_5befa1ab',
    label: 'house_city_2s_noarc_e4',
    rooms: [{ name: 'envprobe_house_city_2s_noarc_e_2', at: { x: 3043.4, y: 719.5, z: 54.2 } }],
  };
  const interiors = [cityHouse];
  const own = interiorOfModel(
    'objects/manmade/structures/living/houses/house_city_2s_noarc_e.cgf',
    { x: 3043.5, y: 719.5, z: 54.2 },
    interiors,
  );
  assert.equal(own, cityHouse);
  const closed = interiorOfModel(
    'objects/manmade/structures/living/houses/unique/kutna_hora/vlasska/vlasska_05_house.cgf',
    { x: 3056.5, y: 713.6, z: 57.9 },
    interiors,
  );
  assert.equal(closed, null);
});

const HOUSE =
  'structures/living/houses/unique/kutna_hora/vlasska/vlasska_11_house1_4d58f1d4-40f0-4282-a7f2-84ca7381ef2b';
const SHED = 'structures/industrial/sheds/shed_kh_c2_95008005-0f1d-4b93-aee4-d324190813c3';

// The level's entities as World.entitiesAround answers them.
const entity = (cls, name, x, y, z) => ({
  class: cls,
  name,
  guid: name,
  position: { x, y, z },
});
const ENTITIES = [
  entity('EnvironmentLight', `envprobe_vlasska_11_house_6[${HOUSE}]`, 2978, 683, 55.1),
  entity('EnvironmentLight', `envprobe_vlasska_11_house_7[${HOUSE}]`, 2970, 690, 52),
  entity('AreaShape', `kkut_vlasska_11_ruthardka_attic_rooms2_sh[${HOUSE}]`, 2973, 680, 55.6),
  entity('AnimDoor', `door_a[${HOUSE}]`, 2968, 676, 51),
  entity('EnvironmentLight', `envprobe_shed_kh_c_1[${SHED}]`, 3156, 614, 53),
  entity('EnvironmentLight', 'kkut_city_underground_probe24', 3068, 660, 44),
];
const BOXES = {
  [`envprobe_vlasska_11_house_6[${HOUSE}]`]: [
    { key: 'BoxSizeX', type: 'number', value: 12.4 },
    { key: 'BoxSizeY', type: 'number', value: 23.8 },
    { key: 'BoxSizeZ', type: 'number', value: 9.5 },
  ],
};

test("an entity names its building in brackets, read without the building's id", () => {
  assert.equal(buildingOf(`envprobe_vlasska_11_house_6[${HOUSE}]`), HOUSE);
  assert.equal(buildingOf('kkut_city_underground_probe24'), null);
  assert.equal(labelOf(HOUSE), 'vlasska_11_house1');
});

test('interiors are grouped by building, with their rooms, boxes, doors and shapes, nearest first', () => {
  const asked = [];
  const found = interiorsOf(ENTITIES, { x: 2975, y: 683, z: 55 }, (e) => {
    asked.push(e.name);
    return BOXES[e.name] ?? [];
  });
  assert.deepEqual(
    found.map((interior) => interior.label),
    ['vlasska_11_house1', 'shed_kh_c2'],
  );
  const house = found[0];
  assert.equal(house.rooms.length, 2);
  assert.deepEqual(house.rooms[0].box, { x: 12.4, y: 23.8, z: 9.5 });
  assert.equal(house.rooms[1].box, null);
  assert.deepEqual(house.doors, [{ name: 'door_a', at: { x: 2968, y: 676, z: 51 } }]);
  assert.deepEqual(house.shapes, ['kkut_vlasska_11_ruthardka_attic_rooms2_sh']);
  assert.equal(house.at.z, 52, 'its lowest room');
  // The level's own probe (underground, no building) is no interior.
  assert.ok(!asked.includes('kkut_city_underground_probe24'));
  assert.deepEqual(
    roomAt(found, { x: 2978, y: 690, z: 56 }).room.name,
    'envprobe_vlasska_11_house_6',
  );
  assert.equal(roomAt(found, { x: 2900, y: 690, z: 56 }), null);
});
