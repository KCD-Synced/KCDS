'use strict';

// The client Keybinds global (Client/resources/keybinds.cjs): actions and categories
// for the game's key binding screen, the player's choices from the game client, the binds they
// make in the Key path, and the engine's input names.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceInput } = require('../../../../Client/resources/input.cjs');
const {
  ResourceKeybinds,
  GAME_CATEGORIES,
  toEngine,
  fromEngine,
} = require('../../../../Client/resources/keybinds.cjs');

function setup() {
  const sent = [];
  const logs = [];
  const input = new ResourceInput({
    send: (message) => sent.push(message),
    log: (owner, text) => logs.push([owner, text]),
  });
  const keybinds = new ResourceKeybinds({
    send: (message) => sent.push(message),
    input,
    log: (owner, text) => logs.push([owner, text]),
  });
  return {
    input,
    keybinds,
    sent,
    logs,
    rp: keybinds.facade('my-rp'),
    shop: keybinds.facade('shop'),
  };
}

// The microtask the whole set is sent in.
const settled = () => new Promise((resolve) => setImmediate(resolve));

const lastSet = (sent) => sent.filter((message) => message.op === 'keybinds').at(-1);

test('actions and categories go to the game client as one set, with engine input names', async () => {
  const { rp, sent } = setup();
  assert.equal(rp.addCategory('roleplay', 'Roleplay'), 'roleplay');
  assert.equal(
    rp.add('emote_menu', {
      label: 'Emote menu',
      category: 'roleplay',
      key: 'numpad1',
      controller: 'xi_dpad_up',
    }),
    true,
  );
  rp.add('radio', { label: 'Talk on the radio', key: 'pageup' });
  rp.add('silent', { label: 'No key yet' });
  await settled();
  const set = lastSet(sent);
  assert.equal(sent.filter((message) => message.op === 'keybinds').length, 1);
  assert.deepEqual(set.groups, [{ name: 'kcds_g_my_rp__roleplay', label: 'Roleplay' }]);
  assert.deepEqual(set.actions, [
    {
      name: 'kcds_a_my_rp__emote_menu',
      group: 'kcds_g_my_rp__roleplay',
      label: 'Emote menu',
      key: 'np_1',
      controller: 'xi_dpad_up',
    },
    {
      name: 'kcds_a_my_rp__radio',
      group: 'general',
      label: 'Talk on the radio',
      key: 'pgup',
      controller: '',
    },
    {
      name: 'kcds_a_my_rp__silent',
      group: 'general',
      label: 'No key yet',
      key: '',
      controller: '',
    },
  ]);
});

test('add validates ids, labels, categories, keys and controller buttons', () => {
  const { rp, shop } = setup();
  assert.throws(() => rp.add('Bad Id', { label: 'x' }), /lower-case letters/);
  assert.throws(() => rp.add('a__b', { label: 'x' }), /single underscores/);
  assert.throws(() => rp.add('ok', {}), /label/);
  assert.throws(() => rp.add('ok', { label: 'x', category: 'nowhere' }), /Unknown category/);
  assert.throws(() => rp.add('ok', { label: 'x', key: 'xi_a' }), /controller button/);
  assert.throws(() => rp.add('ok', { label: 'x', controller: 'f6' }), /not one/);
  assert.throws(() => rp.add('ok', { label: 'x', key: 'f13' }), /Unknown key/);
  shop.addCategory('trade', 'Trade');
  // Another resource's category is not this one's.
  assert.throws(() => rp.add('ok', { label: 'x', category: 'trade' }), /Unknown category/);
  for (const category of GAME_CATEGORIES) {
    assert.equal(rp.add(`in_${category}`, { label: category, category }), true);
  }
  assert.deepEqual(shop.categories(), [...GAME_CATEGORIES, 'trade']);
});

test('get and list report defaults until the game client reports the player choices', () => {
  const { rp, keybinds } = setup();
  rp.add('radio', { label: 'Radio', key: 'f6', controller: 'xi_a' });
  assert.deepEqual(rp.get('radio'), {
    id: 'radio',
    label: 'Radio',
    category: 'general',
    key: 'f6',
    keys: ['f6'],
    controller: 'xi_a',
    defaultKey: 'f6',
    defaultController: 'xi_a',
  });
  keybinds.receiveChoices({
    actions: [{ name: 'kcds_a_my_rp__radio', keys: ['tilde', 'mwheel_up'], controller: 'xi_y' }],
  });
  const radio = rp.get('radio');
  assert.deepEqual(radio.keys, ['grave']); // the mouse wheel is no key KCDS polls
  assert.equal(radio.controller, 'xi_y');
  assert.equal(rp.get('missing'), null);
  assert.deepEqual(
    rp.list().map((action) => action.id),
    ['radio'],
  );
});

test('handlers fire through the Key path on the bound key and controller button', () => {
  const { rp, keybinds, input, sent } = setup();
  rp.add('radio', { label: 'Radio', key: 'f6', controller: 'xi_a' });
  const calls = [];
  rp.on('radio', (id, edge) => calls.push([id, edge]));
  rp.on('radio', 'up', (id, edge) => calls.push(['up only', edge]));
  assert.ok(sent.some((message) => message.op === 'keyTable'));
  input.receiveEdges({ edges: [['f6', 'down']], down: ['f6'] });
  input.receiveEdges({ edges: [['f6', 'up']], down: [] });
  input.receiveEdges({ edges: [['xi_a', 'down']], down: ['xi_a'] });
  assert.deepEqual(calls, [
    ['radio', 'down'],
    ['up only', 'up'],
    ['radio', 'down'],
  ]);
  // The player moved it to F7 on the game's screen: F6 does nothing any more.
  keybinds.receiveChoices({
    actions: [{ name: 'kcds_a_my_rp__radio', keys: ['f7'], controller: '' }],
  });
  calls.length = 0;
  input.receiveEdges({ edges: [['f6', 'down']], down: ['f6'] });
  input.receiveEdges({ edges: [['f7', 'down']], down: ['f7'] });
  input.receiveEdges({ edges: [['xi_a', 'down']], down: ['xi_a'] });
  assert.deepEqual(calls, [['radio', 'down']]);
  assert.equal(rp.off('radio', 'down'), true);
  input.receiveEdges({ edges: [['f7', 'down']], down: ['f7'] });
  assert.deepEqual(calls, [['radio', 'down']]);
});

test('set and reset tell the game client and bind at once', () => {
  const { rp, sent, input } = setup();
  rp.add('radio', { label: 'Radio', key: 'numpad1' });
  const calls = [];
  rp.on('radio', (id) => calls.push(id));
  rp.set('radio', { key: 'pageup', controller: 'xi_triggerr_btn' });
  assert.deepEqual(sent.filter((message) => message.op === 'keybindSet').at(-1), {
    op: 'keybindSet',
    name: 'kcds_a_my_rp__radio',
    keys: ['pgup'],
    controller: 'xi_triggerr_btn',
  });
  assert.deepEqual(rp.get('radio').keys, ['pageup']);
  input.receiveEdges({ edges: [['pageup', 'down']], down: ['pageup'] });
  assert.deepEqual(calls, ['radio']);
  rp.set('radio', { key: null });
  assert.deepEqual(sent.filter((message) => message.op === 'keybindSet').at(-1), {
    op: 'keybindSet',
    name: 'kcds_a_my_rp__radio',
    keys: [],
  });
  assert.equal(rp.get('radio').key, null);
  assert.equal(rp.reset('radio'), true);
  assert.deepEqual(sent.at(-1), { op: 'keybindReset', name: 'kcds_a_my_rp__radio' });
  assert.deepEqual(rp.get('radio').keys, ['numpad1']);
  assert.throws(() => rp.set('missing', { key: 'f6' }), /No key binding action/);
  assert.throws(() => rp.set('radio', { controller: 'f6' }), /not one/);
});

test('a stopping resource takes its actions, categories and binds away', async () => {
  const { rp, shop, keybinds, input, sent } = setup();
  rp.addCategory('roleplay', 'Roleplay');
  rp.add('radio', { label: 'Radio', key: 'f6', category: 'roleplay' });
  rp.on('radio', () => {});
  shop.add('buy', { label: 'Buy', key: 'b' });
  keybinds.removeOwner('my-rp');
  await settled();
  const set = lastSet(sent);
  assert.deepEqual(set.groups, []);
  assert.deepEqual(
    set.actions.map((action) => action.name),
    ['kcds_a_shop__buy'],
  );
  assert.equal(
    input.binds.some((bind) => bind.keybind === 'kcds_a_my_rp__radio'),
    false,
  );
  assert.equal(rp.get('radio'), null);
});

test('engine input names map to KCDS key names and back', () => {
  assert.equal(toEngine('numpad7'), 'np_7');
  assert.equal(toEngine('grave'), 'tilde');
  assert.equal(toEngine('pagedown'), 'pgdn');
  assert.equal(toEngine('shift'), 'lshift');
  assert.equal(toEngine('f6'), 'f6');
  assert.equal(fromEngine('np_0'), 'numpad0');
  assert.equal(fromEngine('tilde'), 'grave');
  assert.equal(fromEngine('lshift'), 'lshift');
  assert.equal(fromEngine('mouse4'), 'mouse4');
  assert.equal(fromEngine('xi_a'), 'xi_a');
  assert.equal(fromEngine('mwheel_up'), null);
  assert.equal(fromEngine('np_add'), null);
});
