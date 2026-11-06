'use strict';

// The tutorial hint extension of the client Hud global (Client/resources/tutorial_hint.cjs):
// options, the game's markup, and every ending of tutorialHintClosed.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceTutorialHint, markup } = require('../../../../Client/resources/tutorial_hint.cjs');

function setup() {
  const calls = [];
  const emitted = [];
  const hints = new ResourceTutorialHint({
    hud: () => ({ call: (name, args) => calls.push([name, ...args]) }),
    emit: (name, args) => emitted.push([name, ...args]),
    log: () => {},
  });
  return { hints, calls, emitted, facade: hints.facade('mode') };
}

test('markup puts the title in the head and every line in a body, escaped', () => {
  assert.equal(
    markup('A <b>', 'Press $use; now\nTom & Jerry'),
    '<head>A &lt;b&gt;</head><body>Press $use; now</body><body>Tom &amp; Jerry</body>',
  );
  assert.equal(markup('', 'x'), '<body>x</body>');
});

test('show draws the HUD tutorial and returns its id', () => {
  const { calls, facade } = setup();
  const id = facade.showTutorialHint({ title: 'Hi', text: 'There', closeHint: false });
  assert.equal(id, 'kcds_hint_1');
  assert.deepEqual(calls[0], [
    'ShowTutorial',
    'kcds_hint_1',
    '<head>Hi</head><body>There</body>',
    0,
    false,
    100,
    0,
    false,
    '',
  ]);
  assert.equal(facade.isTutorialHintShown(id), true);
  assert.equal(facade.isTutorialHintShown(), true);
});

test('malformed options throw', () => {
  const { facade } = setup();
  assert.throws(() => facade.showTutorialHint(null), TypeError);
  assert.throws(() => facade.showTutorialHint({ text: '' }), TypeError);
  assert.throws(() => facade.showTutorialHint({ text: 'x', color: 1 }), TypeError);
  assert.throws(() => facade.showTutorialHint({ text: 'x', title: 5 }), TypeError);
  assert.throws(() => facade.showTutorialHint({ text: 'x', closeHint: 'yes' }), TypeError);
  assert.throws(() => facade.showTutorialHint({ text: 'x', durationMs: -1 }), RangeError);
  assert.throws(() => facade.hideTutorialHint(5), TypeError);
});

test('a new hint replaces the shown one', () => {
  const { emitted, facade } = setup();
  facade.showTutorialHint({ text: 'one' });
  facade.showTutorialHint({ text: 'two' });
  assert.deepEqual(emitted, [['tutorialHintClosed', 'kcds_hint_1', 'replaced']]);
  assert.equal(facade.isTutorialHintShown('kcds_hint_2'), true);
});

test('hide takes the hint down; a wrong id or no hint is false', () => {
  const { calls, emitted, facade } = setup();
  assert.equal(facade.hideTutorialHint(), false);
  const id = facade.showTutorialHint({ text: 'one' });
  assert.equal(facade.hideTutorialHint('kcds_hint_9'), false);
  assert.equal(facade.hideTutorialHint(id), true);
  assert.deepEqual(calls.at(-1), ['HideTutorial', id]);
  assert.deepEqual(emitted, [['tutorialHintClosed', id, 'script']]);
  assert.equal(facade.isTutorialHintShown(), false);
});

test('the game taking the box down is the player closing it', () => {
  for (const call of ['TutorialsStopped', 'HudHidden', 'HideCurrentTutorial', 'HideAllTutorials']) {
    const { hints, emitted, facade } = setup();
    const id = facade.showTutorialHint({ text: 'one' });
    hints.receiveGame({ call, id: '' });
    assert.deepEqual(emitted, [['tutorialHintClosed', id, 'player']]);
  }
});

test('the game showing its own tutorial replaces the hint; our own calls do not', () => {
  const { hints, emitted, facade } = setup();
  const id = facade.showTutorialHint({ text: 'one' });
  hints.receiveGame({ call: 'ShowTutorial', id });
  hints.receiveGame({ call: 'ShowTutorial', id: 'kcds_hint_7' });
  assert.equal(emitted.length, 0);
  hints.receiveGame({ call: 'ShowTutorial', id: 'tutorial_visor' });
  assert.deepEqual(emitted, [['tutorialHintClosed', id, 'replaced']]);
});

test('a duration ends the hint as timeout; the owner stopping as resourceStopped', async () => {
  const { hints, emitted, facade } = setup();
  const id = facade.showTutorialHint({ text: 'one', durationMs: 10 });
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.deepEqual(emitted, [['tutorialHintClosed', id, 'timeout']]);
  const second = facade.showTutorialHint({ text: 'two' });
  hints.removeOwner('other');
  hints.removeOwner('mode');
  assert.deepEqual(emitted.at(-1), ['tutorialHintClosed', second, 'resourceStopped']);
});
