'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// Crowns as items: Items.register({ crown }) puts the crown on and takes it off.

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-crown-items-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'regalia');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'regalia', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  const headPieces = () => output.filter((e) => e.op === 'playerHeadPiece').map((e) => e.field);
  return { host, headPieces };
}

function player(host, id = 7) {
  host.updatePlayer({
    id,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  return host.players.get(id);
}

const RESOURCE = `
  kcds.players.Items.register('regalia:crown', {
    name: "Sigismund's crown",
    visual: 'andelStrazny_riddles',
    weight: 1,
    crown: 'sigismund',
  });
`;

function give(host, p, items) {
  host.playerInventories.operate(p, (inventory) => inventory.set({ items }));
  host.playerInventories.flush();
}

function use(host, p, row) {
  return host.customItems.use(p, row, host.playerInventories.stateOf(p).revision);
}

test('a crown item is usable, labelled Wear, and says its crown', async () => {
  const { host } = hostWith(RESOURCE);
  await host.startAll();
  const info = host.api.Items.info('regalia:crown');
  assert.equal(info.crown, 'sigismund');
  const type = host.customItems.types.get('regalia:crown');
  assert.equal(type.definition.usable, true);
  assert.equal(type.definition.useLabel, 'Wear');
  assert.equal(host.api.Items.info('apple').crown, null);
});

test('using a crown item puts the crown on, using it again takes it off', async () => {
  const { host, headPieces } = hostWith(RESOURCE);
  await host.startAll();
  const p = player(host);
  give(host, p, [{ id: 'crown', item: 'regalia:crown', amount: 1 }]);
  assert.equal(use(host, p, 'crown'), true);
  assert.equal(p.crown, 'sigismund');
  assert.deepEqual(headPieces(), ['sigismund|1']);
  assert.equal(use(host, p, 'crown'), true);
  assert.equal(p.crown, null);
  assert.deepEqual(headPieces(), ['sigismund|1', '']);
});

test('the crown comes off when the item leaves the inventory, not a crown a script set', async () => {
  const { host, headPieces } = hostWith(RESOURCE);
  await host.startAll();
  const p = player(host);
  give(host, p, [{ id: 'crown', item: 'regalia:crown', amount: 1 }]);
  use(host, p, 'crown');
  give(host, p, [{ id: 'apples', item: 'apple', amount: 2 }]);
  assert.equal(p.crown, null);
  assert.deepEqual(headPieces(), ['sigismund|1', '']);

  // A wreath a script put on stays when the crown item goes.
  give(host, p, [{ id: 'crown', item: 'regalia:crown', amount: 1 }]);
  use(host, p, 'crown');
  p.crown = 'wreath';
  give(host, p, []);
  assert.equal(p.crown, 'wreath');
});

test('Items.register refuses an unknown crown and a crown item that is not usable', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const definition = { name: 'Crown', visual: 'andelStrazny_riddles', weight: 1 };
  assert.throws(
    () => host.api.Items.register('regalia:a', { ...definition, crown: 'wenceslas' }),
    TypeError,
  );
  assert.throws(
    () => host.api.Items.register('regalia:b', { ...definition, crown: 'wreath', usable: false }),
    TypeError,
  );
});
