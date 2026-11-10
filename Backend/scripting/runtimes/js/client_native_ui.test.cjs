'use strict';

// The client NativeUI global (Client/resources/native_ui.cjs): menu holds per resource, element
// requests over the synchronous channel, and element events.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceNativeUI } = require('../../../../Client/resources/native_ui.cjs');

function setup(answer = () => undefined) {
  const sent = [];
  const asked = [];
  const logged = [];
  const ui = new ResourceNativeUI({
    sync: {
      post(request) {
        asked.push(request);
      },
      call(request) {
        asked.push(request);
        return answer(request);
      },
    },
    send: (message) => sent.push(message),
    log: (owner, text) => logged.push([owner, text]),
  });
  return { ui, sent, asked, logged };
}

test('a menu stays disabled while any resource disables it', () => {
  const { ui, sent } = setup();
  const a = ui.facade('a').NativeUI;
  const b = ui.facade('b').NativeUI;
  assert.equal(a.isMenuEnabled('map'), true);
  a.setMenuEnabled('map', false);
  b.setMenuEnabled('map', false);
  a.setMenuEnabled('map', false);
  assert.equal(b.isMenuEnabled('map'), false);
  a.setMenuEnabled('map', true);
  assert.equal(a.isMenuEnabled('map'), false);
  b.setMenuEnabled('map', true);
  assert.equal(a.isMenuEnabled('map'), true);
  assert.deepEqual(sent, [
    { op: 'nativeMenus', disabled: [6] },
    { op: 'nativeMenus', disabled: [] },
  ]);
});

test('menus map to the game pages and stopping a resource releases its holds', () => {
  const { ui, sent } = setup();
  const nativeUI = ui.facade('a').NativeUI;
  for (const menu of ['inventory', 'player', 'codex', 'journal', 'crafting']) {
    nativeUI.setMenuEnabled(menu, false);
  }
  assert.deepEqual(sent.at(-1), { op: 'nativeMenus', disabled: [2, 4, 7, 10, 1] });
  ui.removeOwner('a');
  assert.deepEqual(sent.at(-1), { op: 'nativeMenus', disabled: [] });
  assert.equal(nativeUI.isMenuEnabled('inventory'), true);
});

test('unknown menus and non-boolean states throw', () => {
  const { ui } = setup();
  const nativeUI = ui.facade('a').NativeUI;
  assert.throws(() => nativeUI.setMenuEnabled('horse', false), TypeError);
  assert.throws(() => nativeUI.isMenuEnabled('horse'), TypeError);
  assert.throws(() => nativeUI.setMenuEnabled('map', 0), TypeError);
});

test('element calls go through the synchronous channel', () => {
  const { ui, asked } = setup((request) => {
    if (request.op === 'elementInfo') {
      return { valid: true, visible: false };
    }
    if (request.op === 'elementGetVariable') {
      return 42;
    }
    return true;
  });
  const element = ui.facade('a').NativeUI.element('Menu', 3);
  assert.equal(element.valid, true);
  assert.equal(element.visible, false);
  assert.equal(element.getVariable('count'), 42);
  assert.equal(element.call('setTitle', 'Hello', 2), true);
  assert.equal(element.setClip('Panel', { x: 10, alpha: 50 }), true);
  assert.equal(element.gotoClip('Panel', 'open', true), true);
  assert.deepEqual(asked.slice(3), [
    { op: 'elementCall', name: 'Menu', instance: 3, fn: 'setTitle', args: ['Hello', 2] },
    { op: 'elementSetClip', name: 'Menu', instance: 3, clip: 'Panel', x: 10, alpha: 50 },
    { op: 'elementGotoClip', name: 'Menu', instance: 3, clip: 'Panel', frame: 'open', play: true },
  ]);
});

test('an element without an answer reads as missing', () => {
  const { ui } = setup();
  const element = ui.facade('a').NativeUI.element('Missing');
  assert.equal(element.valid, false);
  assert.equal(element.getVariable('x'), null);
  assert.equal(element.getArray('list'), null);
  assert.equal(element.show(), false);
});

test('element arguments are checked', () => {
  const { ui } = setup();
  const element = ui.facade('a').NativeUI.element('Menu');
  assert.throws(() => ui.facade('a').NativeUI.element(''), TypeError);
  assert.throws(() => ui.facade('a').NativeUI.element('Menu', -1), RangeError);
  assert.throws(() => element.call('f', [{}]), TypeError);
  assert.throws(() => element.call('f', new Array(17).fill(0)), RangeError);
  assert.throws(() => element.setVariable('v', Number.NaN), TypeError);
  assert.throws(() => element.gotoClip('c', 0), TypeError);
  assert.throws(() => element.setClip('c', { x: 'left' }), TypeError);
});

test('element events reach the handlers of their element and listening stops with the owner', () => {
  const { ui, sent, logged } = setup();
  const element = ui.facade('a').NativeUI.element('Menu', 1);
  const seen = [];
  element.on('OnButton', (args) => seen.push(args));
  element.on('OnButton', () => {
    throw new Error('broken handler');
  });
  ui.receiveElementEvent({ name: 'Menu', instance: 1, event: 'OnButton', args: ['ok'] });
  ui.receiveElementEvent({ name: 'Menu', instance: 2, event: 'OnButton', args: ['other'] });
  assert.deepEqual(seen, [['ok']]);
  assert.equal(logged.length, 1);
  ui.removeOwner('a');
  ui.receiveElementEvent({ name: 'Menu', instance: 1, event: 'OnButton', args: ['late'] });
  assert.deepEqual(seen, [['ok']]);
  assert.deepEqual(sent, [
    { op: 'nativeElementListen', name: 'Menu', instance: 1, event: 'OnButton' },
    { op: 'nativeElementUnlisten', name: 'Menu', instance: 1, event: 'OnButton' },
  ]);
});

test('a shipped picture is sent once, mounted and answered as its book picture name', () => {
  const sent = [];
  const ui = new ResourceNativeUI({
    sync: { call: () => true, post: () => {} },
    send: (message) => sent.push(message),
    log: () => {},
    shippedFile: (owner, path) => (path === 'ui/seal.dds' ? Buffer.from('DDS data') : null),
  });
  const address = ui.shipTexture('letters', 'ui/seal.dds');
  assert.equal(address, 'kcds_letters_ui_seal');
  assert.equal(ui.shipTexture('letters', 'ui/seal.dds'), address);
  assert.deepEqual(
    sent.map((message) => message.op),
    ['nativeFile', 'nativeMount'],
  );
  assert.deepEqual(sent[1], {
    op: 'nativeMount',
    resource: 'letters',
    path: 'ui/seal.dds',
    itemIcon: false,
  });
  assert.throws(() => ui.shipTexture('letters', 'ui/missing.dds'), /does not ship/);
  assert.throws(() => ui.shipTexture('letters', '../x.dds'), TypeError);
  // The same file as an item type's icon is mounted again, where icons load from.
  assert.equal(ui.shipTexture('letters', 'ui/seal.dds', { itemIcon: true }), address);
  assert.deepEqual(sent.at(-1), {
    op: 'nativeMount',
    resource: 'letters',
    path: 'ui/seal.dds',
    itemIcon: true,
  });
  // And as a blip's own map icon.
  assert.equal(ui.shipTexture('letters', 'ui/seal.dds', { mapIcon: true }), address);
  assert.deepEqual(sent.at(-1), {
    op: 'nativeMount',
    resource: 'letters',
    path: 'ui/seal.dds',
    itemIcon: false,
    mapIcon: true,
  });
  // And as a custom buff's own picture.
  assert.equal(ui.shipTexture('letters', 'ui/seal.dds', { buffIcon: true }), address);
  assert.deepEqual(sent.at(-1), {
    op: 'nativeMount',
    resource: 'letters',
    path: 'ui/seal.dds',
    itemIcon: false,
    buffIcon: true,
  });
  assert.throws(
    () => ui.shipTexture('letters', 'ui/missing.dds', { buffIcon: true }),
    /Buff image/,
  );
});

// a resource's own buttons in the game's Esc menu, two at most, and their presses.
test('Esc menu buttons go to the game client and their presses reach the handler', () => {
  const { ui, sent } = setup();
  const a = ui.facade('a').NativeUI;
  const b = ui.facade('b').NativeUI;
  const pressed = [];
  const settings = a.addMenuButton('Server settings', (id) => pressed.push(['settings', id]));
  const rules = b.addMenuButton('Rules', (id) => pressed.push(['rules', id]));
  assert.equal(
    a.addMenuButton('Third', () => {}),
    null,
  );
  assert.deepEqual(sent.at(-1), {
    op: 'nativeMenuButtons',
    buttons: [
      { key: String(settings), label: 'Server settings' },
      { key: String(rules), label: 'Rules' },
    ],
  });
  ui.receiveMenuButton({ key: String(rules) });
  ui.receiveMenuButton({ key: '999' });
  assert.deepEqual(pressed, [['rules', rules]]);
  // Only the resource that added a button removes it; stopping a resource takes its buttons.
  assert.equal(b.removeMenuButton(settings), false);
  ui.removeOwner('a');
  assert.deepEqual(sent.at(-1), {
    op: 'nativeMenuButtons',
    buttons: [{ key: String(rules), label: 'Rules' }],
  });
  assert.equal(b.removeMenuButton(rules), true);
  assert.deepEqual(sent.at(-1), { op: 'nativeMenuButtons', buttons: [] });
  assert.throws(() => a.addMenuButton('', () => {}), TypeError);
  assert.throws(() => a.addMenuButton('x'.repeat(65), () => {}), TypeError);
  assert.throws(() => a.addMenuButton('Help', 'not a function'), TypeError);
});

test("the game's Esc menu opening and closing are raised as nativeMenuOpened and nativeMenuClosed", () => {
  const emitted = [];
  const ui = new ResourceNativeUI({
    sync: { post() {}, call() {} },
    send() {},
    log() {},
    emit: (name, args) => emitted.push([name, ...args]),
  });
  ui.receiveMenu({ open: true });
  ui.receiveMenu({ open: false });
  assert.deepEqual(emitted, [['nativeMenuOpened'], ['nativeMenuClosed']]);
});

test("the game's own screens open and close as events, and openScreens lists the open ones", () => {
  const emitted = [];
  const ui = new ResourceNativeUI({
    sync: { post() {}, call() {} },
    send() {},
    log() {},
    emit: (name, args) => emitted.push([name, ...args]),
  });
  const { NativeUI } = ui.facade('a');
  assert.deepEqual(NativeUI.openScreens(), []);
  ui.receiveScreen({ name: 'ApseInventoryList', open: true });
  ui.receiveScreen({ name: 'ApseCharacter', open: true });
  // A repeat, or a malformed message, changes nothing.
  ui.receiveScreen({ name: 'ApseInventoryList', open: true });
  ui.receiveScreen({ name: 'ApseMap', open: 'yes' });
  ui.receiveScreen({ name: '', open: true });
  assert.deepEqual(NativeUI.openScreens(), ['ApseInventoryList', 'ApseCharacter']);
  ui.receiveScreen({ name: 'ApseInventoryList', open: false });
  ui.receiveScreen({ name: 'ApseMap', open: false });
  assert.deepEqual(NativeUI.openScreens(), ['ApseCharacter']);
  assert.deepEqual(emitted, [
    ['nativeScreenOpened', 'ApseInventoryList'],
    ['nativeScreenOpened', 'ApseCharacter'],
    ['nativeScreenClosed', 'ApseInventoryList'],
  ]);
  // The list is a copy.
  NativeUI.openScreens().push('x');
  assert.deepEqual(NativeUI.openScreens(), ['ApseCharacter']);
});
