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
    { op: 'creatorOpen', showroom: true },
  ]);
});

test('showroom: false asks the stage without its framework', () => {
  const { sent, facade } = setup();
  facade.open({ panel: false, showroom: false });
  assert.deepEqual(sent.at(-1), { op: 'creatorOpen', showroom: false });
  assert.throws(() => setup().facade.open({ showroom: 'no' }), TypeError);
});

test('previewClothes puts worn item classes on the figure by name or GUID, [] takes them off', () => {
  const { sent, facade } = setup();
  assert.equal(facade.previewClothes([]), false, 'nothing open');
  facade.open({ panel: false });
  const { itemClasses } = require('./inventory/catalog.cjs');
  const worn = itemClasses().filter((entry) =>
    ['armor', 'helmet', 'hood'].includes(entry.category),
  );
  const [first, second] = worn;
  assert.equal(facade.previewClothes([first.name, second.id.toUpperCase()]), true);
  assert.deepEqual(sent.at(-1), { op: 'creatorClothes', items: [first.id, second.id] });
  assert.equal(facade.previewClothes(null), true);
  assert.deepEqual(sent.at(-1), { op: 'creatorClothes', items: [] });
  const apple = itemClasses().find((entry) => entry.category === 'food');
  assert.throws(() => facade.previewClothes([apple.name]), RangeError);
  assert.throws(() => facade.previewClothes(['no_such_item_class']), RangeError);
  assert.throws(() => facade.previewClothes('hat'), TypeError);
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

// A server's own editor: no panel, the look previewed, framed, accepted from its script.
test('without the panel a script previews looks, frames the figure and accepts', () => {
  const { Appearances } = require('./appearance/appearances.cjs');
  const { sent, emitted, panels, creator, facade } = setup();
  assert.equal(facade.preview({ gender: 'male' }), false);
  assert.equal(facade.look(), null);
  facade.open({ panel: false, genders: 'male' });
  creator.receiveStage({ state: 'open', keys: {} });
  assert.equal(panels.length, 0);
  const head = Appearances.options('head', 'male')[1].name;
  assert.equal(facade.preview({ gender: 'male', head }), true);
  assert.deepEqual(sent.at(-1), {
    op: 'creatorLook',
    gender: 'male',
    body: '',
    head,
    hair: '',
    beard: '',
  });
  assert.equal(facade.look().head, head);
  assert.deepEqual(emitted.at(-1), [
    'characterCreatorChanged',
    { gender: 'male', body: '', head, hair: '', beard: '' },
  ]);
  // The same look again tells nobody.
  const heard = emitted.length;
  facade.preview({ gender: 'male', head });
  assert.equal(emitted.length, heard);
  // A gender the creator does not offer, a part it does not know.
  assert.throws(() => facade.preview({ gender: 'female' }), RangeError);
  assert.throws(() => facade.preview({ gender: 'male', head: 'no_such_head' }), RangeError);
  assert.equal(facade.frame('face'), true);
  assert.deepEqual(sent.at(-1), { op: 'creatorFrame', close: true });
  facade.frame('body');
  assert.deepEqual(sent.at(-1), { op: 'creatorFrame', close: false });
  assert.throws(() => facade.frame('feet'), TypeError);
  assert.equal(facade.accept(), true);
  assert.deepEqual(emitted.at(-1), [
    'characterCreatorClosed',
    { gender: 'male', body: '', head, hair: '', beard: '' },
    'accepted',
  ]);
  assert.equal(facade.accept(), false);
  assert.equal(facade.cancel(), false);
  facade.open({ panel: false });
  assert.equal(facade.cancel(), true);
  assert.deepEqual(emitted.at(-1), ['characterCreatorClosed', null, 'cancelled']);
});

test('choices limit what the panel offers and what random picks', () => {
  const { Appearances } = require('./appearance/appearances.cjs');
  const heads = Appearances.options('head', 'male')
    .filter((option) => option.group !== null)
    .slice(0, 2)
    .map((option) => option.name);
  assert.throws(() => setup().facade.open({ choices: { head: ['no_such_head'] } }), RangeError);
  assert.throws(() => setup().facade.open({ choices: { feet: [''] } }), TypeError);
  assert.throws(() => setup().facade.open({ choices: { head: [] } }), TypeError);
  assert.throws(() => setup().facade.open({ panel: 'no' }), TypeError);
  const { creator } = setup();
  creator.open('mode', { genders: 'male', choices: { head: heads, beard: [''] } });
  assert.equal(creator.allows('head', heads[0]), true);
  assert.equal(creator.allows('head', ''), false);
  assert.equal(creator.allows('hair', ''), true);
  for (let round = 0; round < 20; round += 1) {
    creator.randomize();
    assert.ok(heads.includes(creator.look().head));
    assert.equal(creator.look().beard, '');
  }
  // The panel lists only those heads' families and those heads under them; no Default row.
  const screen = { on: () => {}, close: () => {} };
  const panel = createCreatorPanel({ createScreen: () => screen }, creator, {});
  panel.tab = 'head';
  const families = panel.families().map((row) => row.name);
  assert.ok(!families.includes(''));
  assert.ok(families.length >= 1);
  for (const family of families) {
    assert.ok(heads.some((head) => panel.rootOf(head) === family));
  }
  for (const row of panel.variants()) {
    assert.ok(heads.includes(row.name));
  }
  // A family pressed gives a head the creator offers.
  panel.choose(families[0]);
  assert.ok(heads.includes(creator.look().head));
  panel.tab = 'beard';
  assert.deepEqual(
    panel.families().map((row) => row.name),
    [''],
  );
});

test("without the panel the creator's keys are off unless keyboard asks for them", () => {
  const off = setup();
  off.facade.open({ panel: false });
  off.creator.receiveStage({ state: 'open', keys: {} });
  off.creator.receiveKey({ key: 'accept' });
  off.creator.receiveKey({ key: 'random' });
  assert.equal(off.facade.isOpen(), true, 'Enter in a page of the server does not accept');
  assert.equal(off.emitted.length, 0);
  // A script's own calls still work.
  assert.equal(off.facade.accept(), true);
  assert.equal(off.emitted.at(-1)[2], 'accepted');

  const on = setup();
  on.facade.open({ panel: false, keyboard: true });
  on.creator.receiveStage({ state: 'open', keys: {} });
  on.creator.receiveKey({ key: 'accept' });
  assert.equal(on.facade.isOpen(), false);

  const panelOff = setup();
  panelOff.facade.open({ keyboard: false });
  panelOff.creator.receiveStage({ state: 'open', keys: {} });
  panelOff.creator.receiveKey({ key: 'accept' });
  assert.equal(panelOff.facade.isOpen(), true, 'keyboard: false with the panel too');
  assert.throws(() => setup().facade.open({ keyboard: 'yes' }), TypeError);
});

test('turn turns only the figure and orbit only the camera, by degrees', () => {
  const { sent, facade } = setup();
  assert.equal(facade.turn(30), false, 'nothing open');
  facade.open({ panel: false });
  assert.equal(facade.turn(30), true);
  assert.equal(facade.orbit(-450), true);
  assert.deepEqual(sent.slice(-2), [
    { op: 'creatorTurn', degrees: 30 },
    { op: 'creatorOrbit', degrees: -90 },
  ]);
  assert.throws(() => facade.turn(Number.NaN), TypeError);
  assert.throws(() => facade.orbit('90'), TypeError);
});
