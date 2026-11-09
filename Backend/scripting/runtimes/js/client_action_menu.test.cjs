'use strict';

// The client ActionMenu global (Client/resources/action_menu.cjs): the line on the game's hint,
// the wheel moving the mark, the press choosing, ownership and the wheel asked only while open.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceActionHints } = require('../../../../Client/resources/action_hints.cjs');
const { ResourceActionMenus } = require('../../../../Client/resources/action_menu.cjs');

function setup() {
  const sent = [];
  const send = (message) => sent.push(message);
  const hints = new ResourceActionHints({ send, log: () => {} });
  const menus = new ResourceActionMenus({ actionHints: hints, send });
  const shown = () => [...sent].reverse().find((message) => message.op === 'actionHints').hints;
  const wheel = () => sent.filter((message) => message.op === 'wheel').map((m) => m.wanted);
  return { hints, menus, sent, shown, wheel, a: menus.facade('a'), b: menus.facade('b') };
}

const ENTRIES = [
  { id: 'talk', text: 'Talk' },
  { id: 'trade', text: 'Trade' },
  { id: 'give', text: 'Give a gift' },
];

test('a menu is one line on the game hint: the wheel moves the mark, the press chooses', () => {
  const { hints, menus, a, shown, wheel } = setup();
  const chosen = [];
  const id = a.open({ entries: ENTRIES }, (entry) => chosen.push(entry));
  assert.equal(shown().length, 1);
  assert.equal(shown()[0].key, 'primary_action');
  assert.equal(shown()[0].text, '[Talk] · Trade · Give a gift');
  assert.deepEqual(wheel(), [true]);
  // Towards the player: the entry after; past the end, round to the start.
  menus.receiveWheel({ notches: -1 });
  assert.equal(shown()[0].text, 'Talk · [Trade] · Give a gift');
  assert.equal(a.selected(id), 'trade');
  menus.receiveWheel({ notches: -2 });
  assert.equal(a.selected(id), 'talk');
  menus.receiveWheel({ notches: 1 });
  assert.equal(a.selected(id), 'give');
  hints.receiveFired({ id: shown()[0].id });
  assert.deepEqual(chosen, ['give']);
  assert.equal(a.select(id, 'trade'), true);
  assert.equal(a.select(id, 'nobody'), false);
  hints.receiveFired({ id: shown()[0].id });
  assert.deepEqual(chosen, ['give', 'trade']);
  // Closed: no hint, and the wheel is let go.
  assert.equal(a.close(id), true);
  assert.deepEqual(shown(), []);
  assert.deepEqual(wheel(), [true, false]);
  assert.equal(a.selected(id), null);
  assert.equal(a.close(id), false);
});

test('the newest menu takes the wheel; a resource reaches only its own', () => {
  const { menus, a, b, wheel } = setup();
  const first = a.open({ entries: ENTRIES, key: 'script_action' }, () => {});
  const second = b.open({ entries: ENTRIES }, () => {});
  menus.receiveWheel({ notches: -1 });
  assert.equal(a.selected(first), 'talk');
  assert.equal(b.selected(second), 'trade');
  assert.equal(a.close(second), false);
  assert.equal(a.selected(second), null);
  // A stopping resource's menus go; the other one takes the wheel.
  menus.removeOwner('b');
  menus.receiveWheel({ notches: -1 });
  assert.equal(a.selected(first), 'trade');
  assert.deepEqual(wheel(), [true]);
  menus.removeOwner('a');
  assert.deepEqual(wheel(), [true, false]);
  // Nothing open: a wheel turn is nothing.
  menus.receiveWheel({ notches: 3 });
});

test('open checks its entries, key and handler', () => {
  const { a } = setup();
  const handler = () => {};
  assert.throws(() => a.open({ entries: [ENTRIES[0]] }, handler), RangeError);
  assert.throws(
    () => a.open({ entries: [...ENTRIES, ...ENTRIES].slice(0, 7) }, handler),
    RangeError,
  );
  assert.throws(() => a.open({ entries: [ENTRIES[0], ENTRIES[0]] }, handler), RangeError);
  assert.throws(
    () => a.open({ entries: [ENTRIES[0], { id: 'a b', text: 'x' }] }, handler),
    TypeError,
  );
  assert.throws(
    () => a.open({ entries: [ENTRIES[0], { id: 'x', text: ' ' }] }, handler),
    TypeError,
  );
  assert.throws(
    () => a.open({ entries: [ENTRIES[0], { id: 'x', text: 'x'.repeat(33) }] }, handler),
    RangeError,
  );
  assert.throws(() => a.open({ entries: ENTRIES, key: 'use' }, handler), RangeError);
  assert.throws(() => a.open({ entries: ENTRIES }, 'no'), TypeError);
  assert.throws(() => a.open(null, handler), TypeError);
});
