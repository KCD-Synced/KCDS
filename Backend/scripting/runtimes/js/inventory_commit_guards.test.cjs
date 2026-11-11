'use strict';

// Inventory.beforeCommit: what players' own actions do to inventories waits for the resources.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { runGuardedAction } = require('./inventory/guarded_actions.cjs');

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-commit-guards-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'bank');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'bank', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  const chat = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  return { host, chat, output };
}

function player(host, id = 7) {
  host.updatePlayer({
    id,
    ready: true,
    position: { x: 2, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  return host.players.get(id);
}

const settle = () => new Promise((resolve) => setImmediate(resolve));

const ARROWS = `kcds.world.GroundItem.spawn('arrow_crude', { x: 1, y: 2, z: 3 }, {}, 20);`;

test('without a guard an action commits at once', async () => {
  const { host } = hostWith(ARROWS);
  await host.startAll();
  const p = player(host);
  const [arrows] = host.makeAPI().GroundItem.all();
  runGuardedAction(host, { op: 'groundItemPickup', id: p.id, stack: arrows.id });
  assert.equal(p.getInventory().items[0].amount, 20);
  assert.equal(host.makeAPI().GroundItem.all().length, 0);
});

test('a guard sees what would move, and the action waits for its verdict', async () => {
  const { host, chat } = hostWith(`${ARROWS}
    const waiting = [];
    kcds.players.Inventory.beforeCommit((transaction) => {
      const [line] = transaction.lines;
      kcds.core.Chat.sendToAll([transaction.kind, transaction.player.id, line.name, line.amount,
        line.from, line.to, transaction.groundItem.amount, typeof transaction.id].join(' '));
      return new Promise((resolve) => waiting.push(resolve));
    });
    kcds.core.Events.on('consoleCommand', (command, args) => {
      waiting.shift()(args[0] === 'yes');
    });`);
  await host.startAll();
  const p = player(host);
  const [arrows] = host.makeAPI().GroundItem.all();
  runGuardedAction(host, { op: 'groundItemPickup', id: p.id, stack: arrows.id });
  // The same stack again while it waits is the same action.
  runGuardedAction(host, { op: 'groundItemPickup', id: p.id, stack: arrows.id });
  await settle();
  assert.deepEqual(chat(), ['pickup 7 arrow_crude 20 ground player 20 string']);
  assert.deepEqual(p.getInventory().items, []);
  host.emit('consoleCommand', 'verdict', ['no']);
  await settle();
  assert.deepEqual(p.getInventory().items, []);
  assert.equal(host.makeAPI().GroundItem.all().length, 1);
  // Taken up again, and allowed this time.
  runGuardedAction(host, { op: 'groundItemPickup', id: p.id, stack: arrows.id });
  await settle();
  host.emit('consoleCommand', 'verdict', ['yes']);
  await settle();
  assert.equal(p.getInventory().items[0].amount, 20);
  assert.equal(host.makeAPI().GroundItem.all().length, 0);
});

test("one player's actions commit in order, each against the inventory as it is then", async () => {
  const { host, output } = hostWith(`
    const waiting = [];
    kcds.players.Inventory.beforeCommit((transaction, verdict) => {
      waiting.push(verdict);
    });
    kcds.core.Events.on('consoleCommand', () => waiting.shift()(true));`);
  await host.startAll();
  const p = player(host);
  const { Inventory } = host.api;
  const food = Inventory.classes().find((entry) => entry.category === 'food' && !entry.quest);
  const row = Inventory.add(p, { item: food.id, amount: 1 }).items[0].id;
  // Two drops of the only unit: the second waits for the first, then no longer fits.
  runGuardedAction(host, { op: 'inventoryDrop', id: p.id, row, amount: 1 });
  runGuardedAction(host, { op: 'inventoryDrop', id: p.id, row, amount: 1 });
  await settle();
  assert.equal(host.makeAPI().GroundItem.all().length, 0);
  host.emit('consoleCommand', 'allow', []);
  await settle();
  assert.equal(host.makeAPI().GroundItem.all().length, 1);
  assert.deepEqual(p.getInventory().items, []);
  // The second was described against the empty inventory: it commits at once and refuses
  // itself, so no guard is asked.
  assert.equal(host.makeAPI().GroundItem.all().length, 1);
  assert.equal(output.filter((e) => e.op === 'groundItemSpawn').length, 1);
});

test('a refused or unanswered use tells the game it is refused', async () => {
  const { host, output } = hostWith(`
    kcds.players.Inventory.beforeCommit((transaction, verdict) => {
      if (transaction.lines[0].amount === 1 && transaction.kind === 'use') {
        if (globalThis.answer) verdict(false, 'the bank is closed');
      }
    });
    kcds.core.Events.on('consoleCommand', () => { globalThis.answer = true; });`);
  await host.startAll();
  host.commitGuards.verdictMs = 30;
  const p = player(host);
  const { Inventory } = host.api;
  const food = Inventory.classes().find((entry) => entry.category === 'food' && !entry.quest);
  const row = Inventory.add(p, { item: food.id, amount: 2 }).items[0].id;
  const verdicts = () =>
    output.filter((e) => e.op === 'inventoryConsumeReply').map((e) => [e.request, e.verdict]);
  // No answer in time.
  runGuardedAction(host, { op: 'inventoryConsume', id: p.id, request: 4, row });
  await new Promise((resolve) => setTimeout(resolve, 60));
  assert.deepEqual(verdicts(), [[4, 'refused']]);
  // A refusal.
  host.emit('consoleCommand', 'answer', []);
  runGuardedAction(host, { op: 'inventoryConsume', id: p.id, request: 5, row });
  await settle();
  assert.deepEqual(verdicts(), [
    [4, 'refused'],
    [5, 'refused'],
  ]);
  assert.equal(p.getInventory().items[0].amount, 2);
});

test('a guard goes with stopBeforeCommit and with its resource', async () => {
  const { host } = hostWith(`${ARROWS}
    const id = kcds.players.Inventory.beforeCommit(() => false);
    kcds.core.Events.on('consoleCommand', () => kcds.players.Inventory.stopBeforeCommit(id));
    kcds.players.Inventory.beforeCommit(() => true);`);
  await host.startAll();
  const p = player(host);
  const [twenty] = host.makeAPI().GroundItem.all();
  runGuardedAction(host, { op: 'groundItemPickup', id: p.id, stack: twenty.id });
  await settle();
  assert.deepEqual(p.getInventory().items, []);
  host.emit('consoleCommand', 'open', []);
  runGuardedAction(host, { op: 'groundItemPickup', id: p.id, stack: twenty.id });
  await settle();
  assert.equal(p.getInventory().items[0].amount, 20);
  assert.throws(() => host.api.Inventory.beforeCommit('no'), TypeError);
  await host.stop('bank');
  assert.equal(host.commitGuards.guards.size, 0);
  const five = host.makeAPI().GroundItem.spawn('arrow_crude', { x: 1, y: 2, z: 3 }, {}, 5);
  runGuardedAction(host, { op: 'groundItemPickup', id: p.id, stack: five.id });
  assert.equal(p.getInventory().items[0].amount, 25);
});

test('each transaction settles with its id: the changes it made name it, then applied or refused', async () => {
  const { host } = hostWith(`${ARROWS}
    kcds.world.GroundItem.spawn('arrow_crude', { x: 1, y: 2, z: 3 }, {}, 5);
    const seen = [];
    globalThis.seen = seen;
    kcds.players.Inventory.beforeCommit((transaction, verdict) => {
      seen.push(transaction.id);
      if (transaction.groundItem.amount === 5) {
        verdict(false, 'over the limit');
      } else {
        verdict(true);
      }
    });`);
  await host.startAll();
  const p = player(host);
  const events = [];
  host.on('playerInventoryChanged', (who, change) => events.push(['changed', change.transaction]));
  host.on('playerInventoryTransaction', (who, outcome) => events.push(['settled', outcome]));
  const [twenty, five] = host.makeAPI().GroundItem.all();
  runGuardedAction(host, { op: 'groundItemPickup', id: p.id, stack: twenty.id });
  runGuardedAction(host, { op: 'groundItemPickup', id: p.id, stack: five.id });
  await settle();
  await settle();
  host.playerInventories.flush();
  const ids = events.filter((event) => event[0] === 'settled').map((event) => event[1].id);
  assert.equal(ids.length, 2);
  for (const id of ids) {
    assert.match(id, /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/);
  }
  assert.notEqual(ids[0], ids[1]);
  // The change the first made carries its id and comes before its outcome.
  assert.deepEqual(events[0], ['changed', ids[0]]);
  assert.deepEqual(events[1], [
    'settled',
    {
      id: ids[0],
      kind: 'pickup',
      applied: true,
      code: '',
      reason: '',
      revision: p.getInventory().revision,
    },
  ]);
  assert.deepEqual(
    { ...events[2][1], revision: 0 },
    {
      id: ids[1],
      kind: 'pickup',
      applied: false,
      code: 'refused',
      reason: 'over the limit',
      revision: 0,
    },
  );
  assert.equal(events.length, 3);
});
