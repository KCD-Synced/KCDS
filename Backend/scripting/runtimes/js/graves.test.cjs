'use strict';

// Grave: the level's graves, dug out and filled for every player.
const test = require('node:test');
const assert = require('node:assert/strict');
const { Graves } = require('./graves/graves.cjs');

const FIRST = '318710,49590,4050';
const SECOND = '318900,50000,4100';

// A host with two ready players.
function fakeHost() {
  const output = [];
  const events = [];
  const players = new Map([
    [1, { id: 1, ready: true }],
    [2, { id: 2, ready: true }],
  ]);
  const host = {
    players,
    bridge: { send: (e) => output.push(e) },
    emit: (...args) => {
      events.push(args);
      return true;
    },
  };
  const graves = new Graves(host);
  return { graves, Grave: graves.define(), output, events, players };
}

test('a grave becomes known from the first report, with the state that game shows', () => {
  const { graves, Grave, output } = fakeHost();
  assert.deepEqual(Grave.all(), []);
  graves.report({ id: 1 }, FIRST, { x: 3187.1, y: 495.9, z: 40.5 }, false);
  graves.report({ id: 1 }, SECOND, { x: 3189, y: 500, z: 41 }, true);
  assert.deepEqual(
    Grave.all().map((grave) => [grave.id, grave.dug]),
    [
      [FIRST, false],
      [SECOND, true],
    ],
  );
  assert.equal(Grave.get(FIRST), Grave.all()[0]);
  assert.deepEqual(Grave.get(FIRST).position, { x: 3187.1, y: 495.9, z: 40.5 });
  assert.equal(Grave.get('1,2,3'), null);
  assert.deepEqual(output, []);
});

test('a game showing another state than the server is set right', () => {
  const { graves, output } = fakeHost();
  graves.report({ id: 1 }, FIRST, { x: 3187.1, y: 495.9, z: 40.5 }, false);
  graves.report({ id: 2 }, FIRST, { x: 3187.1, y: 495.9, z: 40.5 }, true);
  assert.deepEqual(output, [{ op: 'graveSet', to: 2, grave: FIRST, dug: false }]);
});

test('a dug grave is open for everyone and raises graveDug once', () => {
  const { graves, Grave, output, events } = fakeHost();
  const digger = { id: 1 };
  graves.report(digger, FIRST, { x: 3187.1, y: 495.9, z: 40.5 }, false);
  graves.dug(digger, FIRST, { x: 3187.1, y: 495.9, z: 40.5 });
  graves.dug(digger, FIRST, { x: 3187.1, y: 495.9, z: 40.5 });
  assert.equal(Grave.get(FIRST).dug, true);
  assert.deepEqual(output, [{ op: 'graveSet', to: 0, grave: FIRST, dug: true }]);
  assert.equal(events.length, 1);
  assert.equal(events[0][0], 'graveDug');
  assert.equal(events[0][1], digger);
  assert.equal(events[0][2], Grave.get(FIRST));
});

test('a grave nobody reported yet becomes known when it is dug', () => {
  const { graves, Grave } = fakeHost();
  graves.dug({ id: 1 }, SECOND, { x: 3189, y: 500, z: 41 });
  assert.equal(Grave.get(SECOND).dug, true);
});

test('scripts dig graves out and fill them again', () => {
  const { graves, Grave, output, events } = fakeHost();
  graves.report({ id: 1 }, FIRST, { x: 3187.1, y: 495.9, z: 40.5 }, false);
  graves.report({ id: 1 }, SECOND, { x: 3189, y: 500, z: 41 }, true);
  const grave = Grave.get(FIRST);
  assert.equal(grave.dig(), true);
  assert.equal(grave.dig(), false);
  assert.equal(grave.reset(), true);
  assert.equal(grave.reset(), false);
  assert.equal(Grave.resetAll(), 1);
  assert.equal(Grave.resetAll(), 0);
  assert.deepEqual(output, [
    { op: 'graveSet', to: 0, grave: FIRST, dug: true },
    { op: 'graveSet', to: 0, grave: FIRST, dug: false },
    { op: 'graveSet', to: 0, grave: SECOND, dug: false },
  ]);
  assert.deepEqual(events, []);
  assert.equal(String(grave), `Grave(${FIRST}, full)`);
});

test('a player whose world is ready hears the dug-out graves', () => {
  const { graves, output } = fakeHost();
  graves.report({ id: 1 }, FIRST, { x: 3187.1, y: 495.9, z: 40.5 }, false);
  graves.report({ id: 1 }, SECOND, { x: 3189, y: 500, z: 41 }, true);
  graves.arrived({ id: 2 });
  assert.deepEqual(output, [{ op: 'graveSet', to: 2, grave: SECOND, dug: true }]);
});

test('nearest and all(near) sort by distance', () => {
  const { graves, Grave } = fakeHost();
  graves.report({ id: 1 }, FIRST, { x: 0, y: 0, z: 0 }, false);
  graves.report({ id: 1 }, SECOND, { x: 10, y: 0, z: 0 }, false);
  assert.equal(Grave.nearest({ x: 9, y: 0, z: 0 }).id, SECOND);
  assert.equal(Grave.nearest({ x: 5, y: 40, z: 0 }, 10), null);
  assert.deepEqual(
    Grave.all({ x: 9, y: 0, z: 0 }).map((grave) => grave.id),
    [SECOND, FIRST],
  );
  assert.equal(Grave.get(FIRST).distanceTo({ x: 3, y: 4, z: 0 }), 5);
  assert.throws(() => Grave.nearest('here'));
});

test('malformed reports are ignored', () => {
  const { graves, Grave } = fakeHost();
  graves.report({ id: 1 }, 'not an id', { x: 0, y: 0, z: 0 }, false);
  graves.report({ id: 1 }, FIRST, null, false);
  graves.report(undefined, FIRST, { x: 0, y: 0, z: 0 }, false);
  graves.dug({ id: 1 }, '1,2', { x: 0, y: 0, z: 0 });
  assert.deepEqual(Grave.all(), []);
});
