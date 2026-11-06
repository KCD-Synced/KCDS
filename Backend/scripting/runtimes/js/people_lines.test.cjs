'use strict';

// Lines the level's people say to players near them (npcs/people_lines.cjs).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { PeopleLines, settingsOf } = require('./npcs/people_lines.cjs');

function person(id, name, x, y) {
  const said = [];
  return {
    name,
    said,
    npc: {
      id,
      position: { x, y, z: 0 },
      say(text, options) {
        said.push([text, options.duration]);
        return true;
      },
    },
  };
}

function setup(settings, people) {
  let now = 0;
  const lines = new PeopleLines({
    bodies: () => people,
    now: () => now,
    random: () => 0.5,
  });
  lines.set(settings);
  return {
    lines,
    advance: (seconds) => {
      now += seconds * 1000;
    },
  };
}

const henry = { id: 1, nickname: 'Henry', position: { x: 0, y: 0, z: 0 } };

test('people lines: the nearest person near a player says a line now and then', () => {
  const near = person(10, 'Baker', 3, 0);
  const nearer = person(11, 'Baker', 1, 0);
  const far = person(12, 'Baker', 30, 0);
  const { lines, advance } = setup(
    { everySeconds: 20, radius: 8, seconds: 4, lines: ['Good day, {player}.'] },
    [near, nearer, far],
  );
  // The first tick only starts the player's wait (about everySeconds).
  lines.tick([henry]);
  assert.equal(nearer.said.length, 0);
  advance(19);
  lines.tick([henry]);
  assert.equal(nearer.said.length, 0, 'not before the pause');
  advance(2);
  lines.tick([henry]);
  assert.deepEqual(nearer.said, [['Good day, Henry.', 4000]]);
  assert.equal(near.said.length + far.said.length, 0);
  // The next line comes only after another pause, again from the nearest one.
  advance(10);
  lines.tick([henry]);
  assert.equal(nearer.said.length, 1);
  advance(11);
  lines.tick([henry]);
  assert.equal(nearer.said.length, 2);
});

test('people lines: a person who just spoke waits; the next player hears the next nearest', () => {
  const near = person(10, 'Baker', 3, 0);
  const nearer = person(11, 'Baker', 1, 0);
  const { lines, advance } = setup({ everySeconds: 20, lines: ['Good day, {player}.'] }, [
    near,
    nearer,
  ]);
  const hans = { id: 2, nickname: 'Hans', position: { x: 1, y: 0, z: 0 } };
  lines.tick([henry, hans]);
  advance(21);
  lines.tick([henry, hans]);
  assert.deepEqual(nearer.said, [['Good day, Henry.', 5000]]);
  assert.deepEqual(near.said, [['Good day, Hans.', 5000]]);
});

test('people lines: a name of its own lines; nobody too far, none without lines', () => {
  const guard = person(20, 'Guard', 2, 0);
  const baker = person(21, 'Baker', 1, 0);
  const { lines, advance } = setup(
    { everySeconds: 10, radius: 5, byName: { Guard: ['Move along.'] } },
    [guard, baker],
  );
  lines.tick([henry]);
  advance(20);
  lines.tick([henry]);
  // The baker has no lines (no general ones): the guard speaks.
  assert.deepEqual(guard.said, [['Move along.', 5000]]);
  assert.equal(baker.said.length, 0);
  const off = setup({ enabled: false, lines: ['Hello'] }, [baker]);
  off.lines.tick([henry]);
  off.advance(1000);
  off.lines.tick([henry]);
  assert.equal(baker.said.length, 0);
});

test('people lines: the file is read from the resources folder; a bad one is refused', () => {
  assert.throws(() => settingsOf({ everySeconds: 1 }), RangeError);
  assert.throws(() => settingsOf({ lines: [''] }), TypeError);
  assert.throws(() => settingsOf({ byName: [] }), TypeError);
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-lines-'));
  fs.writeFileSync(path.join(root, 'people-lines.json'), JSON.stringify({ lines: ['Hi'] }));
  const logged = [];
  const lines = new PeopleLines({ bodies: () => [], log: (text) => logged.push(text) });
  lines.loadFile(root);
  assert.deepEqual(lines.settings.lines, ['Hi']);
  fs.writeFileSync(path.join(root, 'people-lines.json'), '{"radius": 99}');
  lines.loadFile(root);
  assert.match(logged[0], /radius/);
  assert.deepEqual(lines.settings.lines, ['Hi'], 'a bad file keeps what was there');
});
