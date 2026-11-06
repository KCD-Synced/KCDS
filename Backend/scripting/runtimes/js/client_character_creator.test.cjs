'use strict';

// The client CharacterCreator global (Client/resources/character_creator.cjs): options, the
// stage messages, the creator's keys and every ending of characterCreatorClosed.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceCharacterCreator } = require('../../../../Client/resources/character_creator.cjs');

function setup({ own = null } = {}) {
  const sent = [];
  const emitted = [];
  const panels = [];
  const creator = new ResourceCharacterCreator({
    send: (message) => sent.push(message),
    emit: (name, args) => emitted.push([name, ...args]),
    log: () => {},
    ownLook: () => own,
    createPanel: (_creator, keys) => {
      const panel = { keys, closed: false, keysPressed: [], resets: 0 };
      panel.close = () => {
        panel.closed = true;
      };
      panel.key = (name) => panel.keysPressed.push(name);
      panel.afterLookReset = () => {
        panel.resets += 1;
      };
      panels.push(panel);
      return panel;
    },
  });
  return { creator, sent, emitted, panels, facade: creator.facade('mode') };
}

test('open sends the starting look, then asks for the stage', () => {
  const { sent, facade } = setup();
  assert.equal(facade.open(), true);
  assert.equal(facade.isOpen(), true);
  assert.deepEqual(sent, [
    { op: 'creatorLook', gender: 'male', body: '', head: '', hair: '', beard: '' },
    { op: 'creatorOpen' },
  ]);
});

test('the man starts on the player own look, the woman on the game default', () => {
  const own = { gender: 'male', body: '', head: 'm_head_004', hair: '', beard: '' };
  const male = setup({ own });
  male.facade.open({ genders: 'both' });
  assert.equal(male.sent[0].head, 'm_head_004');
  const female = setup({ own });
  female.facade.open({ genders: 'female' });
  assert.deepEqual(female.sent[0], {
    op: 'creatorLook',
    gender: 'female',
    body: '',
    head: '',
    hair: '',
    beard: '',
  });
});

test('malformed options throw', () => {
  const { facade } = setup();
  assert.throws(() => facade.open(null), TypeError);
  assert.throws(() => facade.open({ genders: 'all' }), TypeError);
  assert.throws(() => facade.open({ color: 'red' }), TypeError);
  assert.throws(() => facade.open({ title: 'x'.repeat(65) }), TypeError);
  assert.throws(() => facade.open({ appearance: { gender: 'male', head: 'nobody' } }), RangeError);
  assert.throws(
    () => facade.open({ appearance: { gender: 'female', body: '' }, genders: 'male' }),
    RangeError,
  );
  assert.equal(facade.isOpen(), false);
});

test('the stage going up puts the panel up with the key names', () => {
  const { creator, panels } = setup();
  creator.open('mode', {});
  creator.receiveStage({ state: 'open', keys: { accept: 'Enter' } });
  creator.receiveStage({ state: 'open', keys: { accept: 'Enter' } });
  assert.equal(panels.length, 1);
  assert.equal(panels[0].keys.accept, 'Enter');
});

test('accept hands the look over and closes the stage', () => {
  const { creator, sent, emitted, panels } = setup();
  creator.open('mode', { appearance: { gender: 'male', head: 'm_head_004' } });
  creator.receiveStage({ state: 'open', keys: {} });
  creator.receiveKey({ key: 'accept' });
  assert.equal(creator.isOpen(), false);
  assert.equal(panels[0].closed, true);
  assert.deepEqual(sent.at(-1), { op: 'creatorClose' });
  const [name, appearance, reason] = emitted[0];
  assert.equal(name, 'characterCreatorClosed');
  assert.equal(reason, 'accepted');
  assert.deepEqual(appearance, {
    gender: 'male',
    body: '',
    head: 'm_head_004',
    hair: '',
    beard: '',
  });
  assert.ok(Object.isFrozen(appearance));
});

test('every other ending reports a reason and no appearance', () => {
  const endings = [
    [(c) => c.close(), 'closed'],
    [(c) => c.receiveStage({ state: 'left' }), 'cancelled'],
    [(c) => c.receiveStage({ state: 'unavailable' }), 'unavailable'],
    [(c) => c.receiveStage({ state: 'interrupted' }), 'interrupted'],
    [(c) => c.removeOwner('mode'), 'interrupted'],
    [(c) => c.open('other', {}), 'replaced'],
  ];
  for (const [end, reason] of endings) {
    const { creator, emitted } = setup();
    creator.open('mode', {});
    end(creator);
    assert.deepEqual(emitted[0], ['characterCreatorClosed', null, reason]);
  }
});

test('close is false when no creator is open', () => {
  const { facade, emitted } = setup();
  assert.equal(facade.close(), false);
  assert.equal(emitted.length, 0);
});

test('the gender key switches only when both genders are offered', () => {
  const both = setup();
  both.creator.open('mode', { genders: 'both' });
  both.creator.receiveStage({ state: 'open', keys: {} });
  both.creator.receiveKey({ key: 'gender' });
  assert.equal(both.creator.look().gender, 'female');
  assert.equal(both.sent.at(-1).gender, 'female');
  const one = setup();
  one.creator.open('mode', { genders: 'male' });
  one.creator.receiveStage({ state: 'open', keys: {} });
  one.creator.receiveKey({ key: 'gender' });
  assert.equal(one.creator.look().gender, 'male');
});

test('random puts on a catalog look of the gender and reset the starting one', () => {
  const { creator } = setup();
  creator.open('mode', { genders: 'female' });
  creator.receiveStage({ state: 'open', keys: {} });
  creator.receiveKey({ key: 'random' });
  const random = creator.look();
  assert.equal(random.gender, 'female');
  assert.notEqual(random.head, '');
  creator.receiveKey({ key: 'reset' });
  assert.deepEqual(creator.look(), {
    gender: 'female',
    body: '',
    head: '',
    hair: '',
    beard: '',
  });
});

test('keys before the stage is up and navigation keys go where they belong', () => {
  const { creator, panels, emitted } = setup();
  creator.open('mode', {});
  creator.receiveKey({ key: 'accept' });
  assert.equal(emitted.length, 0);
  creator.receiveStage({ state: 'open', keys: {} });
  creator.receiveKey({ key: 'down' });
  creator.receiveKey({ key: 'tab' });
  assert.deepEqual(panels[0].keysPressed, ['down', 'tab']);
});

test('frame asks for the head close for every part but the body', () => {
  const { creator, sent } = setup();
  creator.frame('head');
  assert.equal(sent.length, 0);
  creator.open('mode', {});
  creator.frame('hair');
  creator.frame('body');
  assert.deepEqual(sent.slice(-2), [
    { op: 'creatorFrame', close: true },
    { op: 'creatorFrame', close: false },
  ]);
});

// The panel's lists, without a screen: families, variants and the beard rule.
const { createCreatorPanel, prettify } = require('../../../../Client/resources/creator_panel.cjs');

function panelFor(look) {
  const creator = new ResourceCharacterCreator({
    send: () => {},
    emit: () => {},
    log: () => {},
    ownLook: () => null,
  });
  creator.open('mode', { appearance: look });
  const screen = { on: () => {}, close: () => {} };
  return { creator, panel: createCreatorPanel({ createScreen: () => screen }, creator, {}) };
}

test('prettify reads catalog names as words', () => {
  assert.equal(prettify('m_hair_001_black'), 'Hair 001 black');
  assert.equal(prettify('henry'), 'Henry');
});

test('a nested skin belongs to the family at the root of its groups', () => {
  const { panel } = panelFor({ gender: 'female', body: 'f_body_pale_04' });
  panel.tab = 'body';
  assert.equal(panel.family(), 'female_body');
  assert.ok(panel.families().some((row) => row.name === 'female_body'));
  assert.ok(panel.variants().some((row) => row.name === 'f_body_pale_04'));
  assert.equal(panel.families()[0].name, '');
});

test('a new face drops a beard it cannot wear', () => {
  const { Appearances } = require('./appearance/appearances.cjs');
  const beard = Appearances.beards('', 'male')[0].name;
  const bare = Appearances.options('head', 'male').find(
    (option) => option.group === null && Appearances.beards(option.name, 'male').length === 0,
  ).name;
  const { creator, panel } = panelFor({ gender: 'male', head: '', beard });
  panel.tab = 'head';
  panel.choose(bare);
  assert.equal(creator.look().head, bare);
  assert.equal(creator.look().beard, '');
});
