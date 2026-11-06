'use strict';
// Appearance: the Appearances catalog, Player.appearance and Player.setAppearance.
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { Appearances } = require('./appearance/appearances.cjs');

function fixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-appearance-'));
  const output = [];
  return {
    root,
    output,
    host: new ResourceHost(root, { send: (e) => output.push(e), log: () => {} }),
  };
}

function resource(root, name, code) {
  const dir = path.join(root, name);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name, kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
}

function report(host, appearance, id = 7) {
  return host.updatePlayer({
    id,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    appearance,
  });
}

test('the catalog carries the game tree of both genders', () => {
  const counts = (gender) =>
    ['head', 'hair', 'beard', 'body'].map((part) => Appearances.options(part, gender).length);
  assert.deepEqual(counts(), [212, 262, 51, 47]);
  assert.deepEqual(counts('female'), [61, 71, 0, 41]);
  for (const option of Appearances.options('hair')) {
    assert.equal(option.part, 'hair');
    assert.equal(option.gender, 'male');
    assert.ok(Object.isFrozen(option));
  }
  assert.throws(() => Appearances.options('eyes'), TypeError);
  assert.throws(() => Appearances.options('head', 'dwarf'), TypeError);
});

test('find looks one name up in one gender', () => {
  assert.deepEqual(Appearances.find('hair', 'm_hair_001_black'), {
    name: 'm_hair_001_black',
    part: 'hair',
    gender: 'male',
    group: 'x_hair_01',
  });
  assert.equal(Appearances.find('hair', 'x_hair_01').group, null);
  assert.equal(Appearances.find('head', 'henry').name, 'henry');
  assert.equal(Appearances.find('head', 'henry', 'female'), null);
  assert.equal(Appearances.find('beard', 'no_such_beard'), null);
  assert.throws(() => Appearances.find('hair', 3), TypeError);
});

test('beards lists what a face can wear', () => {
  const henry = Appearances.beards().map((option) => option.name);
  assert.equal(henry.length, 15);
  assert.deepEqual(Appearances.beards('henry'), Appearances.beards());
  assert.ok(henry.includes('UC_beard_barber_01'));
  assert.ok(!henry.includes('UC_beard_aulitz'));
  assert.equal(Appearances.beards('m_head_004').length, 24);
  assert.deepEqual(Appearances.beards('no_such_head'), []);
  assert.deepEqual(Appearances.beards('f_head_000', 'female'), []);
  const faces = Appearances.options('head').map((head) => Appearances.beards(head.name).length);
  assert.equal(faces.filter((count) => count === 0).length, 14);
});

test('random always draws an appearance a client accepts', () => {
  for (let draw = 0; draw < 200; ++draw) {
    const male = Appearances.random();
    assert.equal(male.gender, 'male');
    assert.ok(Appearances.find('head', male.head));
    assert.ok(Appearances.find('hair', male.hair));
    assert.ok(Appearances.find('body', male.body));
    if (male.beard !== '') {
      assert.ok(Appearances.beards(male.head).some((option) => option.name === male.beard));
    }
    const female = Appearances.random('female');
    assert.equal(female.beard, '');
    assert.ok(Appearances.find('head', female.head, 'female'));
  }
});

test('setAppearance asks the owner, appearance reads what it published', async () => {
  const f = fixture();
  resource(
    f.root,
    'looks',
    `'use strict';
    kcds.core.Events.on('playerCommand', (p, c) => {
      const outcomes = [];
      const attempt = (request) => {
        try {
          outcomes.push(String(p.setAppearance(request)));
        } catch (error) {
          outcomes.push(error.constructor.name);
        }
      };
      if (c === 'look') {
        attempt({ hair: 'm_hair_005_ginger' });
        attempt({ head: 'm_head_004', beard: 'UC_beard_radzig' });
        attempt({ beard: 'UC_beard_aulitz' });
        attempt({ hair: 'no_such_hair' });
        attempt({ gender: 'female' });
        attempt({ gender: 'female', head: 'henry' });
        attempt({ body: '' });
        attempt(null);
        attempt({ hair: 3 });
        attempt({ eyes: 'blue' });
      }
      if (c === 'read') {
        kcds.core.Chat.sendToPlayer(p, JSON.stringify(p.appearance));
      }
      kcds.core.Chat.sendToPlayer(p, outcomes.join(','));
    });`,
  );
  try {
    await f.host.startAll();
    const p = report(f.host);
    // Everyone starts as the default body.
    assert.deepEqual(
      { ...p.appearance },
      { gender: 'male', body: '', head: '', hair: '', beard: '' },
    );

    f.host.receiveChat(p.id, '/look');
    assert.equal(
      f.output.at(-1).text,
      'true,true,false,false,true,false,true,TypeError,TypeError,TypeError',
    );
    // The face and beard always travel together; a gender change names every part.
    assert.deepEqual(
      f.output.filter((e) => e.op === 'appearance').map((e) => ({ ...e })),
      [
        { op: 'appearance', id: 7, head: '', beard: '', hair: 'm_hair_005_ginger' },
        { op: 'appearance', id: 7, head: 'm_head_004', beard: 'UC_beard_radzig' },
        { op: 'appearance', id: 7, head: '', beard: '', gender: 'female', body: '', hair: '' },
        { op: 'appearance', id: 7, head: '', beard: '', body: '' },
      ],
    );

    // What the owner published, once it has put it on.
    report(f.host, {
      gender: 'male',
      body: '',
      head: 'm_head_004',
      hair: 'm_hair_005_ginger',
      beard: 'UC_beard_radzig',
    });
    f.host.receiveChat(p.id, '/read');
    assert.equal(
      f.output.at(-2).text,
      '{"gender":"male","body":"","head":"m_head_004","hair":"m_hair_005_ginger","beard":"UC_beard_radzig"}',
    );
    // A beard keeps its face: a new hairstyle sends the face and beard the player wears.
    f.output.length = 0;
    assert.equal(p.setAppearance({ hair: '' }), true);
    assert.deepEqual(
      { ...f.output.at(-1) },
      {
        op: 'appearance',
        id: 7,
        head: 'm_head_004',
        beard: 'UC_beard_radzig',
        hair: '',
      },
    );
    // The face without its beard's model is refused whole.
    assert.equal(p.setAppearance({ head: 'henry' }), false);

    // Drawn until the owner publishes otherwise; setVisible asks for a boolean.
    assert.equal(p.visible, true);
    assert.equal(p.setVisible(false), true);
    assert.deepEqual({ ...f.output.at(-1) }, { op: 'visible', id: 7, visible: false });
    assert.throws(() => p.setVisible('no'), TypeError);
    assert.throws(() => p.setVisible(), TypeError);
    f.host.updatePlayer({
      id: 7,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      visible: false,
    });
    assert.equal(p.visible, false);

    // The visor as the owner's game reports it; false until it says so.
    assert.equal(p.visorDown, false);
    f.host.updatePlayer({
      id: 7,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      visorDown: true,
    });
    assert.equal(p.visorDown, true);
    assert.throws(() => {
      p.visorDown = false;
    }, TypeError);

    // A disguise is a prop-catalog mesh by its path or file stem; null is the player's own body.
    assert.equal(p.disguise, null);
    assert.equal(p.setDisguise('beer_barrel'), true);
    const barrel = 'objects/manmade/task_specific_props/entertainment/tavern/beer_barrel.cgf';
    assert.deepEqual({ ...f.output.at(-1) }, { op: 'disguise', id: 7, model: barrel });
    assert.equal(p.setDisguise(barrel.toUpperCase()), true);
    assert.equal(f.output.at(-1).model, barrel);
    assert.equal(p.setDisguise(null), true);
    assert.equal(f.output.at(-1).model, '');
    assert.throws(() => p.setDisguise('no_such_mesh'), RangeError);
    assert.throws(() => p.setDisguise(3), TypeError);
    assert.throws(() => p.setDisguise(), TypeError);
    f.host.updatePlayer({
      id: 7,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      disguise: barrel,
    });
    assert.equal(p.disguise, barrel);

    // Worn outside the inventory: the whole list goes to the player's own client.
    assert.deepEqual(p.wornItems, []);
    assert.equal(p.wearItem('Noose_mHangman'), true);
    assert.equal(p.wearItem('Noose_mHangman'), false);
    assert.equal(f.output.at(-1).op, 'worn');
    assert.equal(f.output.at(-1).id, 7);
    assert.equal(f.output.at(-1).items.length, 1);
    assert.match(f.output.at(-1).items[0], /^[0-9a-f]{32}$/);
    assert.equal(p.wornItems.length, 1);
    assert.throws(() => p.wearItem('no_such_item'), TypeError);
    assert.throws(() => p.wearItem('longSwordDuel'), RangeError);
    assert.equal(p.removeWorn('Noose_mHangman'), true);
    assert.equal(p.removeWorn('Noose_mHangman'), false);
    assert.deepEqual(f.output.at(-1).items, []);

    // A player that left has no client to ask.
    f.host.disconnect(p.id);
    assert.equal(p.setAppearance({ hair: '' }), false);
    assert.equal(p.setVisible(true), false);
    assert.equal(p.setDisguise(null), false);
    assert.equal(p.wearItem('Noose_mHangman'), false);
  } finally {
    fs.rmSync(f.root, { recursive: true, force: true });
  }
});
