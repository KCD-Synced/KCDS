'use strict';

// The client Key and Controls globals (Client/resources/input.cjs): key names, binds, edges,
// labels and the counted Controls holds.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceInput, keyName, KEYS } = require('../../../../Client/resources/input.cjs');

function setup() {
  const sent = [];
  const logs = [];
  const input = new ResourceInput({
    send: (message) => sent.push(message),
    log: (owner, text) => logs.push([owner, text]),
  });
  return { input, sent, logs, hud: input.keyFacade('hud'), shop: input.keyFacade('shop') };
}

test('key names are case-insensitive, take the documented aliases and reject the rest', () => {
  assert.equal(keyName('G'), 'g');
  assert.equal(keyName('Control'), 'ctrl');
  assert.equal(keyName('RETURN'), 'enter');
  assert.equal(keyName('esc'), 'escape');
  assert.equal(keyName('num7'), 'numpad7');
  assert.throws(() => keyName('f13'), /Unknown key/);
  assert.throws(() => keyName(4), TypeError);
  for (const name of ['a', 'z', '0', '9', 'f1', 'f12', 'up', 'lshift', 'ralt', 'pagedown']) {
    assert.equal(keyName(name), name);
  }
  for (const name of ['capslock', 'mouse1', 'mouse5', 'numpad0', 'backspace', 'tab']) {
    assert.equal(keyName(name), name);
  }
  // The punctuation keys of a US keyboard, such as the slash a chat command starts with.
  for (const name of ['slash', 'comma', 'period', 'grave', 'minus', 'backslash', 'apostrophe']) {
    assert.equal(keyName(name), name);
  }
});

test('the key table goes to the game client once a resource binds or reads a key', () => {
  const { hud, sent } = setup();
  assert.deepEqual(sent, []);
  hud.bind('g', () => {});
  hud.isDown('h');
  assert.equal(sent.length, 1);
  assert.equal(sent[0].op, 'keyTable');
  assert.equal(sent[0].keys.length, KEYS.length);
  assert.deepEqual(
    sent[0].keys.find((key) => key.name === 'z'),
    { name: 'z', kind: 'scan', code: 0x2c },
  );
});

test('bind defaults to the down edge and validates its arguments', () => {
  const { input, hud } = setup();
  const calls = [];
  assert.equal(
    hud.bind('g', (key, state) => calls.push([key, state])),
    true,
  );
  assert.throws(() => hud.bind('g', 'twice', () => {}), /Unknown key state/);
  assert.throws(() => hud.bind('g', 'down'), TypeError);
  assert.throws(() => hud.bind('nope', () => {}), /Unknown key/);
  input.receiveEdges({ edges: [['g', 'down']], down: ['g'] });
  input.receiveEdges({ edges: [['g', 'up']], down: [] });
  assert.deepEqual(calls, [['g', 'down']]);
});

test('up and both binds see the matching edges', () => {
  const { input, hud } = setup();
  const calls = [];
  hud.bind('mouse2', 'both', (key, state) => calls.push(['both', state]));
  hud.bind('mouse2', 'up', (key, state) => calls.push(['up', state]));
  input.receiveEdges({ edges: [['mouse2', 'down']], down: ['mouse2'] });
  input.receiveEdges({ edges: [['mouse2', 'up']], down: [] });
  assert.deepEqual(calls, [
    ['both', 'down'],
    ['both', 'up'],
    ['up', 'up'],
  ]);
});

test('isDown answers from the last reported state and false for unknown keys', () => {
  const { input, hud } = setup();
  assert.equal(hud.isDown('lshift'), false);
  input.receiveEdges({ edges: [], down: ['lshift'] });
  assert.equal(hud.isDown('LShift'), true);
  assert.equal(hud.isDown('nope'), false);
  input.receiveEdges({ edges: [], down: [] });
  assert.equal(hud.isDown('lshift'), false);
});

test('unbind removes only the resource own matching binds', () => {
  const { input, hud, shop } = setup();
  const calls = [];
  const jump = () => calls.push('hud-jump');
  hud.bind('space', 'down', jump);
  hud.bind('space', 'up', () => calls.push('hud-up'));
  shop.bind('space', () => calls.push('shop'));
  assert.equal(shop.unbind('space', 'down', jump), false);
  assert.equal(hud.unbind('space', 'down', jump), true);
  input.receiveEdges({ edges: [['space', 'down']], down: ['space'] });
  assert.equal(hud.unbind('space'), true);
  assert.equal(hud.unbind('space'), false);
  input.receiveEdges({ edges: [['space', 'up']], down: [] });
  assert.deepEqual(calls, ['shop']);
});

test('a throwing handler is logged and the others still run', () => {
  const { input, hud, shop, logs } = setup();
  const calls = [];
  hud.bind('e', () => {
    throw new Error('broken');
  });
  shop.bind('e', () => calls.push('shop'));
  input.receiveEdges({ edges: [['e', 'down']], down: ['e'] });
  assert.deepEqual(calls, ['shop']);
  assert.equal(logs[0][0], 'hud');
});

test('labels come from the layout and fall back to the English names', () => {
  const { input, hud } = setup();
  assert.equal(hud.getLabel('z'), 'Z');
  assert.equal(hud.getLabel('return'), 'Enter');
  assert.equal(hud.getLabel('lshift'), 'Left Shift');
  assert.equal(hud.getLabel('mouse1'), 'Mouse 1');
  assert.equal(hud.getLabel('num4'), 'Numpad 4');
  input.receiveLabels({ labels: { z: 'Y', y: 'Z', 2: 'Ľ', nope: 'X' } });
  assert.equal(hud.getLabel('z'), 'Y');
  assert.equal(hud.getLabel('2'), 'Ľ');
  assert.throws(() => hud.getLabel('nope'), /Unknown key/);
});

test('Controls holds are counted per resource and published as totals', () => {
  const { input, sent } = setup();
  const hud = input.controlsFacade('hud');
  const shop = input.controlsFacade('shop');
  assert.equal(hud.isEnabled(), true);
  hud.disable();
  shop.disable();
  assert.equal(hud.isEnabled(), false);
  shop.enable();
  shop.enable(); // no hold left for shop: nothing changes
  assert.equal(hud.isEnabled(), false);
  hud.enable();
  assert.equal(hud.isEnabled(), true);
  hud.showCursor();
  assert.equal(shop.isCursorVisible(), true);
  hud.hideCursor();
  assert.equal(shop.isCursorVisible(), false);
  assert.deepEqual(
    sent.map(({ disabled, cursor }) => [disabled, cursor]),
    [
      [true, false],
      [false, false],
      [false, true],
      [false, false],
    ],
  );
});

test('typed-input capture and the pointer of a focused view count for everyone', () => {
  const { input } = setup();
  const hud = input.controlsFacade('hud');
  input.receiveHost({ captured: true, pointer: true });
  assert.equal(hud.isEnabled(), false);
  assert.equal(hud.isCursorVisible(), true);
  input.receiveHost({ captured: false, pointer: false });
  assert.equal(hud.isEnabled(), true);
  assert.equal(hud.isCursorVisible(), false);
});

test('a stopped resource loses its binds and gives back its holds', () => {
  const { input, hud, sent } = setup();
  const controls = input.controlsFacade('hud');
  const calls = [];
  hud.bind('g', () => calls.push('g'));
  controls.disable();
  controls.showCursor();
  controls.blockAction('use');
  input.removeOwner('hud');
  input.receiveEdges({ edges: [['g', 'down']], down: ['g'] });
  assert.deepEqual(calls, []);
  assert.equal(controls.isEnabled(), true);
  assert.deepEqual(sent.at(-1), { op: 'controls', disabled: false, cursor: false, actions: [] });
});

// Controls.blockAction: one game action is held off, counted per resource.
test('blockAction holds one game action off until every resource gives it back', () => {
  const { input, sent } = setup();
  const hud = input.controlsFacade('hud');
  const shop = input.controlsFacade('shop');
  hud.blockAction('use');
  shop.blockAction('use');
  shop.blockAction('jump');
  hud.unblockAction('use');
  assert.deepEqual(sent.at(-1).actions, ['jump', 'use'], 'shop still holds use');
  shop.unblockAction('use');
  shop.unblockAction('use'); // no hold left: nothing changes
  assert.deepEqual(sent.at(-1).actions, ['jump']);
  input.removeOwner('shop');
  assert.deepEqual(sent.at(-1), { op: 'controls', disabled: false, cursor: false, actions: [] });
  assert.equal(hud.isEnabled(), true, 'one action held off leaves the controls enabled');
  assert.throws(() => hud.blockAction(''), TypeError);
  assert.throws(() => hud.blockAction('use me'), TypeError);
  assert.throws(() => hud.unblockAction(3), TypeError);
  assert.deepEqual(
    sent.map((message) => message.actions),
    [['use'], ['jump', 'use'], ['jump'], []],
  );
});

test('cursorVisibilityChanged comes once for each change of the pointer, held or the game own', () => {
  const emitted = [];
  const input = new ResourceInput({ emit: (name, args) => emitted.push([name, ...args]) });
  const hud = input.controlsFacade('hud');
  const shop = input.controlsFacade('shop');
  hud.showCursor();
  shop.showCursor();
  shop.hideCursor();
  assert.deepEqual(emitted, [['cursorVisibilityChanged', true]]);
  hud.hideCursor();
  assert.deepEqual(emitted.at(-1), ['cursorVisibilityChanged', false]);
  // A focused page shows the game's pointer: the same event, and none while it stays.
  input.receiveHost({ captured: true, pointer: true });
  input.receiveHost({ captured: false, pointer: true });
  input.receiveHost({ captured: false, pointer: false });
  assert.deepEqual(emitted.slice(2), [
    ['cursorVisibilityChanged', true],
    ['cursorVisibilityChanged', false],
  ]);
});
