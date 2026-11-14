'use strict';

// The client ActionHint global (Client/resources/action_hints.cjs): options, stacking on one
// control, handlers, ownership and cleanup.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceActionHints, KEYS } = require('../../../../Client/resources/action_hints.cjs');

function setup() {
  const sent = [];
  const logs = [];
  const hints = new ResourceActionHints({
    send: (message) => sent.push(message),
    log: (owner, text) => logs.push([owner, text]),
  });
  const shown = () => sent.at(-1)?.hints ?? [];
  return { hints, sent, logs, shown, a: hints.facade('a'), b: hints.facade('b') };
}

test('keys lists the controls in a fixed order, the mouse wheel last', () => {
  const { a } = setup();
  assert.deepEqual(a.keys(), [
    'primary_action',
    'attack_special',
    'torch',
    'companion_interaction',
    'distract',
    'jump',
    'script_action',
    'mouse_wheel',
  ]);
  assert.deepEqual(KEYS, a.keys());
});

test('create applies the defaults and rejects bad options', () => {
  const { a, shown } = setup();
  const id = a.create({ key: 'companion_interaction', text: 'Count' }, () => {});
  assert.ok(id > 0);
  assert.deepEqual(shown(), [
    {
      id,
      key: 'companion_interaction',
      text: 'Count',
      mode: 'press',
      holdDuration: 1,
      enabled: true,
    },
  ]);
  const handler = () => {};
  assert.throws(() => a.create({ key: 'use', text: 'x' }, handler), RangeError);
  assert.throws(() => a.create({ key: 'jump', text: 'x'.repeat(257) }, handler), RangeError);
  assert.throws(() => a.create({ key: 'jump', text: 'x', mode: 'tap' }, handler), RangeError);
  assert.throws(() => a.create({ key: 'jump', text: 'x', holdDuration: 0.1 }, handler), RangeError);
  assert.throws(() => a.create({ key: 'jump', text: 'x', holdDuration: 11 }, handler), RangeError);
  assert.throws(() => a.create({ key: 'jump', text: 'x', enabled: 1 }, handler), TypeError);
  assert.throws(() => a.create({ key: 'jump', text: 'x' }), TypeError);
});

test('hints on one control stack: the newest is shown and removing it brings back the older', () => {
  const { a, b, shown } = setup();
  const older = a.create({ key: 'torch', text: 'Older' }, () => {});
  const newer = b.create({ key: 'torch', text: 'Newer' }, () => {});
  assert.deepEqual(
    shown().map((hint) => [hint.id, hint.text, hint.mode]),
    [[newer, 'Newer', 'press']],
  );
  assert.equal(b.remove(newer), true);
  assert.equal(b.remove(newer), false);
  assert.deepEqual(
    shown().map((hint) => hint.id),
    [older],
  );
});

// a press hint and a hold hint on one control are both shown (the game client fires
// the press on a short press and the hold on a full one).
test('a press hint and a hold hint on one control are shown together', () => {
  const { a, b, shown } = setup();
  const press = a.create({ key: 'companion_interaction', text: 'Untie' }, () => {});
  const hold = b.create(
    { key: 'companion_interaction', text: 'Lead', mode: 'hold', holdDuration: 1.5 },
    () => {},
  );
  const newerHold = b.create(
    { key: 'companion_interaction', text: 'Lead away', mode: 'hold' },
    () => {},
  );
  assert.deepEqual(
    shown().map((hint) => [hint.id, hint.text, hint.mode]),
    [
      [press, 'Untie', 'press'],
      [newerHold, 'Lead away', 'hold'],
    ],
  );
  assert.equal(b.remove(newerHold), true);
  assert.deepEqual(
    shown().map((hint) => [hint.id, hint.holdDuration]),
    [
      [press, 1],
      [hold, 1.5],
    ],
  );
});

test('a press runs the handler with the id, only while the hint exists and is enabled', () => {
  const { hints, a, logs } = setup();
  const calls = [];
  const id = a.create({ key: 'jump', text: 'Jump' }, (hint) => calls.push(hint));
  hints.receiveFired({ id });
  assert.equal(a.setEnabled(id, false), true);
  hints.receiveFired({ id });
  a.setEnabled(id, true);
  hints.receiveFired({ id });
  const broken = a.create({ key: 'distract', text: 'Broken' }, () => {
    throw new Error('broken');
  });
  hints.receiveFired({ id: broken });
  a.remove(id);
  hints.receiveFired({ id });
  assert.deepEqual(calls, [id, id]);
  assert.equal(logs[0][0], 'a');
});

test('setText rewrites in place; only the owner changes a hint', () => {
  const { a, b, shown } = setup();
  const id = a.create({ key: 'primary_action', text: 'Count' }, () => {});
  assert.equal(a.setText(id, 'Count (1)'), true);
  assert.equal(shown()[0].text, 'Count (1)');
  assert.equal(a.setText(id, 'x'.repeat(257)), false);
  assert.equal(b.setText(id, 'Stolen'), false);
  assert.equal(b.setEnabled(id, false), false);
  assert.equal(b.remove(id), false);
  assert.equal(a.setText(999, 'none'), false);
});

test('a stopped resource takes its hints down', () => {
  const { hints, a, b, shown } = setup();
  b.create({ key: 'attack_special', text: 'B' }, () => {});
  a.create({ key: 'attack_special', text: 'A' }, () => {});
  hints.removeOwner('a');
  assert.deepEqual(
    shown().map((hint) => hint.text),
    ['B'],
  );
});

test('shown lists what the HUD draws, the game prompts too, and each change is raised', () => {
  const events = [];
  const hints = new ResourceActionHints({
    send: () => {},
    log: () => {},
    emit: (name, args) => events.push([name, args]),
  });
  const a = hints.facade('a');
  assert.deepEqual(a.shown(), []);
  assert.deepEqual(a.shownKeys(), []);
  assert.equal(a.isKeyInUse('talk'), false);
  hints.receiveShown({
    hints: [
      {
        key: 'talk',
        map: 'player',
        text: 'Talk',
        mode: 'press',
        enabled: true,
        progress: 0,
        target: 'Bara',
      },
      {
        key: 'primary_action',
        map: 'player',
        text: 'Open',
        mode: 'hold',
        enabled: false,
        progress: 0.5,
        target: '',
      },
      {
        key: 'talk',
        map: 'player',
        text: 'Talk (2)',
        mode: 'press',
        enabled: true,
        progress: 0,
        target: '',
      },
    ],
  });
  assert.deepEqual(events, [['actionHintsChanged', []]]);
  assert.equal(a.shown().length, 3);
  assert.deepEqual(a.shown()[1], {
    key: 'primary_action',
    map: 'player',
    text: 'Open',
    mode: 'hold',
    enabled: false,
    progress: 0.5,
    target: '',
  });
  assert.deepEqual(a.shownKeys(), ['talk', 'primary_action']);
  assert.equal(a.isKeyInUse('talk'), true);
  assert.equal(a.isKeyInUse('jump'), false);
  assert.throws(() => a.isKeyInUse(5), TypeError);
  // A caller's copy cannot change what is kept.
  a.shown()[0].text = 'changed';
  assert.equal(a.shown()[0].text, 'Talk');
});
