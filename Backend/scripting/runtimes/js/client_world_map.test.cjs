'use strict';

// The client WorldMap global (Client/resources/world_map.cjs): mirrored map state, the
// player's marker, and the map events.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceWorldMap } = require('../../../../Client/resources/world_map.cjs');
const { Vector3 } = require('./math.cjs');

function setup() {
  const sent = [];
  const emitted = [];
  const asked = [];
  // The synchronous channel: what the game client answers, by the request.
  const sync = {
    answer: () => null,
    call(request) {
      asked.push(request);
      return this.answer(request);
    },
  };
  const map = new ResourceWorldMap({
    send: (message) => sent.push(message),
    sync,
    emit: (name, args) => emitted.push([name, ...args]),
    Vector3,
  });
  return { map, sent, emitted, sync, asked, facade: map.facade() };
}

test('isOpen and mapOpened/mapClosed follow the reported map screen', () => {
  const { map, emitted, facade } = setup();
  assert.equal(facade.isOpen, false);
  map.receiveState({ open: true, waypoint: null });
  map.receiveState({ open: true, waypoint: null });
  assert.equal(facade.isOpen, true);
  map.receiveState({ open: false, waypoint: null });
  assert.deepEqual(
    emitted.map(([name]) => name),
    ['mapOpened', 'mapClosed'],
  );
});

test('getWaypoint reads the reported marker as a Vector3 and its map', () => {
  const { map, facade } = setup();
  assert.equal(facade.getWaypoint(), null);
  map.receiveState({ open: false, waypoint: { x: 10, y: 20, z: 3, mapId: 1 } });
  const waypoint = facade.getWaypoint();
  assert.ok(waypoint.position instanceof Vector3);
  assert.deepEqual([waypoint.position.x, waypoint.position.y, waypoint.position.z], [10, 20, 3]);
  assert.equal(waypoint.mapId, 1);
});

test('shiftWaypoint and clearWaypoint need a marker and go to the game client', () => {
  const { map, sent, facade } = setup();
  assert.equal(facade.shiftWaypoint(new Vector3(1, 2, 3)), false);
  assert.equal(facade.clearWaypoint(), false);
  map.receiveState({ open: false, waypoint: { x: 10, y: 20, z: 3, mapId: 1 } });
  assert.equal(facade.shiftWaypoint(new Vector3(1, 2, 3)), true);
  assert.equal(facade.getWaypoint().position.x, 1);
  assert.equal(facade.clearWaypoint(), true);
  assert.deepEqual(sent, [{ op: 'worldMapMove', x: 1, y: 2, z: 3 }, { op: 'worldMapClear' }]);
  assert.throws(() => facade.shiftWaypoint({ x: 'a', y: 0, z: 0 }), TypeError);
});

test('marker events carry a Vector3, the map and whether it moved', () => {
  const { map, emitted } = setup();
  map.receiveEvent({ name: 'mapWaypointSet', where: { x: 1, y: 2, z: 3, mapId: 0 }, moved: true });
  map.receiveEvent({ name: 'mapWaypointCleared', where: { x: 1, y: 2, z: 3, mapId: 0 } });
  assert.equal(emitted[0][0], 'mapWaypointSet');
  assert.ok(emitted[0][1] instanceof Vector3);
  assert.deepEqual(emitted[0].slice(2), [0, true]);
  assert.deepEqual(
    emitted[1].map((value) => (value instanceof Vector3 ? 'vector' : value)),
    ['mapWaypointCleared', 'vector', 0],
  );
});

test('Compass reads the sampled mark count and hides the whole strip through the HUD', () => {
  const { compassFacade } = require('../../../../Client/resources/world_map.cjs');
  const { ResourceHud } = require('../../../../Client/resources/hud.cjs');
  const { map } = setup();
  const sent = [];
  const hud = new ResourceHud({ send: (message) => sent.push(message) });
  const compass = compassFacade(map, hud.facade('@compass'));
  assert.equal(compass.markerTotal, 0);
  map.receiveState({ open: false, waypoint: null, compassMarks: 7 });
  assert.equal(compass.markerTotal, 7);
  assert.equal(compass.visible, true);
  compass.visible = false;
  assert.equal(compass.visible, false);
  compass.visible = true;
  assert.equal(compass.visible, true);
  assert.deepEqual(
    sent.map((message) => message.elements),
    [['Compass'], []],
  );
});

test('Travel reads the reported fast travel and its position', () => {
  const { travelFacade } = require('../../../../Client/resources/world_map.cjs');
  const { map } = setup();
  const travel = travelFacade(map);
  assert.equal(travel.active, false);
  assert.deepEqual([travel.position.x, travel.position.y, travel.position.z], [0, 0, 0]);
  map.receiveState({ open: true, waypoint: null, travel: { active: true, x: 5, y: 6, z: 7 } });
  assert.equal(travel.active, true);
  assert.ok(travel.position instanceof Vector3);
  assert.deepEqual([travel.position.x, travel.position.y, travel.position.z], [5, 6, 7]);
});

test('Travel.canStart is the code the game reports, null until it has one', () => {
  const { travelFacade } = require('../../../../Client/resources/world_map.cjs');
  const { map } = setup();
  const travel = travelFacade(map);
  assert.equal(travel.canStart(), null);
  map.receiveState({ open: false, waypoint: null, travel: { active: false, canStart: 10 } });
  assert.equal(travel.canStart(), 10);
  map.receiveState({ open: false, waypoint: null, travel: { active: false, canStart: null } });
  assert.equal(travel.canStart(), null);
});

test('category visibility: the multiplayer default and resource choices by mark type', () => {
  const { map, sent, facade } = setup();
  assert.equal(facade.mapCategoryShown('Hub'), true);
  assert.equal(facade.mapCategoryShown('Shop'), false);
  assert.equal(facade.setMapCategory('Shop', true), false); // no map page outside a world
  map.receiveState({ open: false, waypoint: null, inWorld: true });
  assert.equal(facade.setMapCategory('Hub', false), true);
  assert.equal(facade.mapCategoryShown('Shop'), true);
  assert.equal(facade.mapCategoryShown('Hub'), false);
  assert.deepEqual(sent.at(-1), { op: 'worldMapCategories', shown: [12], hidden: [6] });
  assert.throws(() => facade.mapCategoryShown('shop'), RangeError);
  assert.throws(() => facade.setMapCategory('Shop', 1), TypeError);
});

// WorldMap.clearAllFog: the whole level's fog, once the level is up.
test('clearAllFog asks the game client once the level is up', () => {
  const { map, sent, facade } = setup();
  assert.equal(facade.clearAllFog(), false);
  assert.deepEqual(sent, []);
  map.receiveState({ open: false, waypoint: null, inWorld: true });
  assert.equal(facade.clearAllFog(), true);
  assert.deepEqual(sent, [{ op: 'worldMapRevealFog' }]);
});

// WorldMap.clearFog: the fog around a point, x and y only.
test('clearFog asks the game client for one point once the level is up', () => {
  const { map, sent, facade } = setup();
  assert.equal(facade.clearFog({ x: 1, y: 2, z: 3 }), false);
  assert.deepEqual(sent, []);
  map.receiveState({ open: false, waypoint: null, inWorld: true });
  assert.equal(facade.clearFog({ x: 1650.5, y: 2150, z: 30 }), true);
  assert.deepEqual(sent, [{ op: 'worldMapRevealFogAt', x: 1650.5, y: 2150 }]);
  assert.throws(() => facade.clearFog({ x: NaN, y: 0 }), TypeError);
  assert.throws(() => facade.clearFog(null), TypeError);
});

test('a discovered place raises locationDiscovered or poiDiscovered with its GUID fields', () => {
  const { map, emitted } = setup();
  map.receiveDiscovered({ kind: 'location', id: '1a2b3c4d-0102-a0b0-0c0d-0e0f10111213' });
  map.receiveDiscovered({ kind: 'poi', id: 'FFFFFFFF-FFFF-0000-0001-020304050607' });
  map.receiveDiscovered({ kind: 'location', id: 'not-a-guid' });
  map.receiveDiscovered({ kind: 'quest', id: '1a2b3c4d-0102-a0b0-0c0d-0e0f10111213' });
  assert.deepEqual(emitted, [
    [
      'locationDiscovered',
      {
        data1: 0x1a2b3c4d,
        data2: 0x0102,
        data3: 0xa0b0,
        data4: Uint8Array.from([0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13]),
      },
    ],
    [
      'poiDiscovered',
      {
        data1: 0xffffffff,
        data2: 0xffff,
        data3: 0,
        data4: Uint8Array.from([0, 1, 2, 3, 4, 5, 6, 7]),
      },
    ],
  ]);
});

// WorldMap.discover: the game client marks the place, silently, and answers.
test('discover asks the game client with the GUID text once the level is up', () => {
  const { map, sync, asked, emitted, facade } = setup();
  const id = {
    data1: 0x1a2b3c4d,
    data2: 0x0102,
    data3: 0xa0b0,
    data4: new Uint8Array([0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13]),
  };
  assert.equal(facade.discover(id), false);
  assert.deepEqual(asked, []);
  map.receiveState({ open: false, waypoint: null, inWorld: true });
  sync.answer = (request) => request.guid === '1a2b3c4d-0102-a0b0-0c0d-0e0f10111213';
  assert.equal(facade.discover(id), true);
  assert.deepEqual(asked, [
    { op: 'worldMapDiscover', guid: '1a2b3c4d-0102-a0b0-0c0d-0e0f10111213' },
  ]);
  assert.equal(facade.discover({ ...id, data1: 1 }), false);
  // The id a discovery event handed out goes back as the same text.
  map.receiveDiscovered({ kind: 'poi', id: 'ffffffff-ffff-0000-0001-020304050607' });
  facade.discover(emitted.at(-1)[1]);
  assert.equal(asked.at(-1).guid, 'ffffffff-ffff-0000-0001-020304050607');
  assert.throws(() => facade.discover({ ...id, data4: new Uint8Array(7) }), TypeError);
  assert.throws(() => facade.discover({ ...id, data2: 0x10000 }), TypeError);
  assert.throws(() => facade.discover('1a2b3c4d-0102-a0b0-0c0d-0e0f10111213'), TypeError);
});
