'use strict';

// Items.register: item types a resource registers.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { guidOf } = require('./inventory/custom_items.cjs');

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-items-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'village');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'village', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  const chat = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  return { host, chat, output };
}

test('Items.register makes a type every game builds, which inventories then take', async () => {
  const { host, chat, output } = hostWith(`
    const timber = kcds.players.Items.register('village:timber', {
      name: 'Village timber',
      description: 'Timber reserved for village repairs.',
      visual: 'bsmt_woodExotic',
      weight: 2,
      price: 12,
    });
    kcds.core.Chat.sendToAll(timber);
    kcds.core.Chat.sendToAll(String(kcds.players.Items.register('village:timber', {
      name: 'Village timber',
      description: 'Timber reserved for village repairs.',
      visual: 'bsmt_woodExotic',
      weight: 2,
      price: 12,
    }) === timber));
    try {
      kcds.players.Items.register('village:timber', { name: 'Other', visual: 'bsmt_woodExotic', weight: 2 });
    } catch (error) {
      kcds.core.Chat.sendToAll('conflict');
    }
    kcds.players.Items.register('village:letter', {
      name: 'Letter',
      visual: 'andelStrazny_riddles',
      weight: 0.1,
      stackable: false,
      usable: true,
      useLabel: 'Read',
    });
  `);
  await host.startAll();
  const timber = guidOf('village:timber');
  assert.match(timber, /^[0-9a-f]{8}-[0-9a-f]{4}-5[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
  assert.deepEqual(chat(), [timber, 'true', 'conflict']);
  const types = output.filter((e) => e.op === 'itemTypes');
  assert.equal(types.length, 2);
  assert.deepEqual(types[0], {
    op: 'itemTypes',
    id: 0,
    full: false,
    types: [
      [
        timber,
        '87a568f2-79f7-415f-a690-9a04c4585455',
        'village:timber',
        'kcds_nm_item_1',
        'kcds_in_item_1',
        'kcds_use_item_1',
        2,
        12,
        true,
        false,
        '',
        '',
        '',
      ],
    ],
  });
  const names = output.filter((e) => e.op === 'itemNames');
  assert.deepEqual(names[0].names, [
    ['kcds_nm_item_1', 'Village timber'],
    ['kcds_in_item_1', 'Timber reserved for village repairs.'],
    ['kcds_use_item_1', 'Use'],
  ]);
  assert.deepEqual(names[1].names[1], ['kcds_in_item_2', ' '], 'no description reads blank');
  // The inventory takes the type by GUID or id, with its weight, price and stacking.
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const p = host.players.get(7);
  const api = host.makeAPI();
  assert.equal(api.Inventory.add(p, { item: timber, amount: 5 }).ok, true);
  assert.equal(api.Inventory.add(p, { item: 'village:letter', amount: 2 }).ok, true);
  assert.deepEqual(
    p.getInventory().items.map((row) => [row.name, row.amount]),
    [
      ['village:timber', 5],
      ['village:letter', 2],
    ],
  );
  assert.ok(api.Inventory.classes().some((entry) => entry.id === timber));
  // A type looks like a miscellaneous item, so it has no equipment slot.
  const timberClass = api.Inventory.classes().find((entry) => entry.id === timber);
  assert.equal(timberClass.slot, null);
  // A player who comes in later hears every type before their inventory.
  output.length = 0;
  host.updatePlayer({
    id: 8,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const joined = output.filter(
    (e) => e.id === 8 && (e.op === 'itemTypes' || e.op === 'inventoryShow'),
  );
  if (joined.length) {
    assert.equal(joined[0].op, 'itemTypes');
    assert.equal(joined[0].full, true);
    assert.equal(joined[0].types.length, 2);
  }
  host.playerInventories.flush();
});

test('Items.register refuses invalid definitions', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const { Items } = host.makeAPI();
  const base = { name: 'Token', visual: 'bsmt_woodExotic', weight: 1 };
  assert.throws(() => Items.register('bad id!', base), /letters/);
  assert.throws(() => Items.register('x'.repeat(129), base), /letters/);
  assert.throws(() => Items.register('t:1', { ...base, name: '' }), /name/);
  assert.throws(() => Items.register('t:1', { ...base, name: 'a\nb' }), /control/);
  assert.throws(() => Items.register('t:1', { ...base, visual: 'longSwordDuel' }), /visual/);
  assert.throws(() => Items.register('t:1', { ...base, visual: 'nothing' }), /visual/);
  assert.throws(() => Items.register('t:1', { ...base, weight: -1 }), /weight/);
  assert.throws(() => Items.register('t:1', { ...base, price: 1.5 }), /price/);
  assert.throws(() => Items.register('t:1', { ...base, stackable: 'yes' }), /stackable/);
  assert.equal(Items.register('t:1', base), guidOf('t:1'));
});

test('choosing the use action on a usable type raises playerCustomItemUse', async () => {
  const { host, chat } = hostWith(`
    globalThis.letter = kcds.players.Items.register('village:letter', {
      name: 'Letter',
      visual: 'andelStrazny_riddles',
      weight: 0.1,
      stackable: false,
      usable: true,
      useLabel: 'Read',
    });
    globalThis.timber = kcds.players.Items.register('village:timber', {
      name: 'Timber',
      visual: 'bsmt_woodExotic',
      weight: 2,
    });
    kcds.core.Events.on('playerCustomItemUse', (player, row, revision) =>
      kcds.core.Chat.sendToAll(['use', player.id, row.name, row.amount, revision, Object.isFrozen(row)].join(' ')));
  `);
  await host.startAll();
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const p = host.players.get(7);
  const api = host.makeAPI();
  api.Inventory.add(p, { item: 'village:letter', amount: 1 });
  api.Inventory.add(p, { item: 'village:timber', amount: 3 });
  const { revision, items } = p.getInventory();
  const [letterRow, timberRow] = items;
  assert.equal(host.customItems.use(p, letterRow.id, revision), true);
  assert.equal(host.customItems.use(p, letterRow.id, revision - 1), false, 'a stale revision');
  assert.equal(host.customItems.use(p, timberRow.id, revision), false, 'not usable');
  assert.equal(host.customItems.use(p, 'nothing', revision), false, 'not theirs');
  assert.deepEqual(chat(), [`use 7 village:letter 1 ${revision} true`]);
  host.playerInventories.flush();
});

test('Items.register takes an icon the resource ships to clients', async () => {
  const fs = require('node:fs');
  const os = require('node:os');
  const path = require('node:path');
  const { ResourceHost } = require('./host.cjs');
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-icon-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'village');
  fs.mkdirSync(path.join(dir, 'icons'), { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({
      name: 'village',
      kcds: { serverScripts: ['index.js'], files: ['icons/*.dds'] },
    }),
  );
  // A .dds header: uncompressed 32-bit, or BC7 (DX10, format 98).
  const dds = (width, height, compressed) => {
    const header = Buffer.alloc(compressed ? 148 : 128);
    header.write('DDS ', 0, 'latin1');
    header.writeUInt32LE(124, 4);
    header.writeUInt32LE(height, 12);
    header.writeUInt32LE(width, 16);
    if (compressed) {
      header.writeUInt32LE(0x4, 80);
      header.write('DX10', 84, 'latin1');
      header.writeUInt32LE(98, 128);
    } else {
      header.writeUInt32LE(0x41, 80);
      header.writeUInt32LE(32, 88);
      header.writeUInt32LE(0xff0000, 92);
      header.writeUInt32LE(0xff00, 96);
      header.writeUInt32LE(0xff, 100);
      header.writeUInt32LE(0xff000000, 104);
    }
    return header;
  };
  fs.writeFileSync(path.join(dir, 'icons', 'Letter.dds'), dds(16, 16, false));
  fs.writeFileSync(path.join(dir, 'icons', 'exact.dds'), dds(64, 64, true));
  fs.writeFileSync(path.join(dir, 'icons', 'big.dds'), dds(128, 128, true));
  fs.writeFileSync(path.join(dir, 'secret.dds'), dds(64, 64, true));
  fs.writeFileSync(
    path.join(dir, 'index.js'),
    `const tryRegister = (id, icon) => {
      try {
        kcds.players.Items.register(id, { name: id, visual: 'bsmt_woodExotic', weight: 1, icon });
        kcds.core.Chat.sendToAll(id + ' ok');
      } catch (error) {
        kcds.core.Chat.sendToAll(id + ' ' + error.constructor.name);
      }
    };
    tryRegister('village:letter', 'icons/Letter.dds');
    tryRegister('village:exact', 'icons/exact.dds');
    tryRegister('village:big', 'icons/big.dds');
    tryRegister('village:secret', 'secret.dds');
    tryRegister('village:png', 'icons/letter.png');
    tryRegister('village:escape', '../other/icon.dds');`,
  );
  await host.startAll();
  const chat = output.filter((e) => e.op === 'chat').map((e) => e.text);
  assert.deepEqual(chat, [
    'village:letter ok',
    'village:exact ok',
    'village:big TypeError',
    'village:secret TypeError',
    'village:png TypeError',
    'village:escape TypeError',
  ]);
  const [row] = output.find((e) => e.op === 'itemTypes').types;
  assert.deepEqual(row.slice(10), ['kcds_village_icons_letter', 'village/icons/Letter.dds', '']);
  await host.shutdown();
  fs.rmSync(root, { recursive: true, force: true });
});

test('Items.register model gives a type a prop mesh on the ground, in shops and in Items.model', async () => {
  const { host, chat, output } = hostWith(`
    const { Items } = kcds.players;
    const bucket = Items.register('village:bucket', {
      name: 'Bucket',
      visual: 'bsmt_woodExotic',
      model: 'manmade/common_furniture/bucket/bucket_a_a',
      weight: 1,
    });
    kcds.core.Chat.sendToAll(Items.model(bucket));
    kcds.core.Chat.sendToAll(Items.model(Items.register('village:plain', { name: 'Plain', visual: 'bsmt_woodExotic', weight: 1 })));
    for (const model of ['no_such_mesh_zz', '', 7]) {
      try {
        Items.register('village:bad', { name: 'Bad', visual: 'bsmt_woodExotic', weight: 1, model });
      } catch (error) {
        kcds.core.Chat.sendToAll(error.name);
      }
    }
  `);
  await host.startAll();
  const [bucket, plain] = output.filter((e) => e.op === 'itemTypes').map((e) => e.types[0]);
  assert.equal(bucket[12], 'objects/manmade/common_furniture/bucket/bucket_a_a.cgf');
  assert.equal(plain[12], '', "the visual's own mesh");
  const said = chat();
  assert.equal(said[0], 'objects/manmade/common_furniture/bucket/bucket_a_a.cgf');
  assert.notEqual(said[1], '', "Items.model of a type without one is its visual's");
  assert.deepEqual(said.slice(2), ['TypeError', 'TypeError', 'TypeError']);
  await host.shutdown();
});

test('Items.info of a registered type has its name and description; a game class has none', async () => {
  const { host, chat } = hostWith(`
    const { Items } = kcds.players;
    const timber = Items.register('village:timber', {
      name: 'Village timber',
      description: 'Timber reserved for village repairs.',
      visual: 'bsmt_woodExotic',
      weight: 2,
    });
    Items.register('village:plain', { name: 'Plain', visual: 'bsmt_woodExotic', weight: 1 });
    for (const item of ['village:timber', timber, 'village:plain', 'bsmt_woodExotic']) {
      const info = Items.info(item);
      kcds.core.Chat.sendToAll(JSON.stringify([info.name, info.displayName, info.description]));
    }
  `);
  await host.startAll();
  assert.deepEqual(
    chat().map((line) => JSON.parse(line)),
    [
      ['village:timber', 'Village timber', 'Timber reserved for village repairs.'],
      ['village:timber', 'Village timber', 'Timber reserved for village repairs.'],
      ['village:plain', 'Plain', ''],
      ['bsmt_woodExotic', null, null],
    ],
  );
  await host.shutdown();
});

test('a renamed row of a miscellaneous item is shown as a view type with its own name', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const p = host.players.get(7);
  const { Inventory } = host.api;
  Inventory.set(p, {
    items: [
      { id: 'timber', item: 'bsmt_woodExotic', amount: 1 },
      { id: 'apple', item: 'apple', amount: 1 },
    ],
  });
  Inventory.editCustom(p, { id: 'timber', name: "Anna's timber" });
  Inventory.editCustom(p, { id: 'apple', name: 'Red apple' });
  const shows = output.filter((e) => e.op === 'inventoryShow');
  const rows = new Map(shows.at(-1).rows.map((row) => [row[0], row[1]]));
  const timber = Inventory.classes().find((c) => c.name === 'bsmt_woodExotic').id;
  const apple = Inventory.classes().find((c) => c.name === 'apple').id;
  assert.notEqual(rows.get('timber'), timber, 'the renamed timber is shown as a view type');
  assert.equal(rows.get('apple'), apple, 'a food row keeps its class (#195, 0.4.2)');
  const types = output.filter((e) => e.op === 'itemTypes').flatMap((e) => e.types);
  const view = types.find((type) => type[0] === rows.get('timber'));
  assert.equal(view[1], timber, 'it looks like the timber');
  assert.equal(view[3], 'kcds_nm_row_1');
  assert.match(view[4], /^ui_in_/, 'the description stays the class own');
  assert.ok(JSON.stringify(output.filter((e) => e.op === 'itemNames')).includes("Anna's timber"));
  // The server keeps the row's own class; a player coming in gets the view type too.
  assert.equal(Inventory.get(p).items.find((row) => row.id === 'timber').item, timber);
  output.length = 0;
  host.customItems.sendAll(p);
  const again = output.filter((e) => e.op === 'itemTypes').flatMap((e) => e.types);
  assert.ok(again.some((type) => type[0] === rows.get('timber')));
  host.playerInventories.flush();
});

test('a renamed weapon or food row keeps its class and names itself to its player alone', async () => {
  const { host, output } = hostWith('');
  await host.startAll();
  for (const id of [7, 8]) {
    host.updatePlayer({
      id,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
    });
  }
  const p = host.players.get(7);
  const { Inventory } = host.api;
  const sword = Inventory.classes().find((c) => c.category === 'meleeWeapon');
  const apple = Inventory.classes().find((c) => c.name === 'apple').id;
  Inventory.set(p, {
    items: [
      { id: 'blade', item: sword.id, amount: 1 },
      { id: 'apple', item: 'apple', amount: 1 },
    ],
  });
  output.length = 0;
  Inventory.editCustom(p, { id: 'blade', name: 'Oathkeeper', description: 'Sworn steel.' });
  Inventory.editCustom(p, { id: 'apple', name: 'Red apple' });
  const rows = new Map(
    output
      .filter((e) => e.op === 'inventoryShow')
      .at(-1)
      .rows.map((row) => [row[0], row[1]]),
  );
  assert.equal(rows.get('blade'), sword.id, 'the sword stays a sword');
  assert.equal(rows.get('apple'), apple);
  const names = output.filter((e) => e.op === 'itemNames');
  assert.ok(
    names.every((e) => e.id === 7 && e.full === false),
    'only its player hears them',
  );
  assert.deepEqual(
    names.flatMap((e) => e.names),
    [
      ['kcds_nm_r_blade', 'Oathkeeper'],
      ['kcds_in_r_blade', 'Sworn steel.'],
      ['kcds_nm_r_apple', 'Red apple'],
    ],
  );
  // A repeat of the stock repeats no name; a row losing its name clears its key.
  output.length = 0;
  Inventory.editCustom(p, { id: 'apple', name: null });
  const cleared = output.filter((e) => e.op === 'itemNames').flatMap((e) => e.names);
  assert.deepEqual(cleared, [['kcds_nm_r_apple', '']]);
  // A player coming in again hears every name of their rows in the full set.
  output.length = 0;
  host.playerInventories.sendAllNames(p);
  const full = output.find((e) => e.op === 'itemNames');
  assert.equal(full.full, true);
  assert.deepEqual(full.names, [
    ['kcds_nm_r_blade', 'Oathkeeper'],
    ['kcds_in_r_blade', 'Sworn steel.'],
  ]);
  host.playerInventories.flush();
});
