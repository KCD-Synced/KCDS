'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');

// Custom recipes and custom crafting stations.

const STATION_AT = { x: 1, y: 2, z: 3 };

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-custom-crafting-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'cidery');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'cidery', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  const chat = () => output.filter((e) => e.op === 'chat').map((e) => e.text);
  return { host, chat, output };
}

function player(host, id = 7) {
  host.updatePlayer({
    id,
    ready: true,
    position: { ...STATION_AT },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  return host.players.get(id);
}

const RESOURCE = `
  kcds.world.Crafting.addRecipe({
    id: 'cider',
    name: 'Cider',
    ingredients: [{ item: 'apple', amount: 2 }],
    products: [{ item: 'potion_antiIflammatoryPotion_high', amount: 1 }],
    duration: 2,
  });
  kcds.world.Crafting.addStation({
    position: { x: 1, y: 2, z: 3 },
    text: 'Press cider',
    recipes: ['cider'],
    autoProgress: AUTO,
  });
  kcds.core.Events.on('craftingStarted', (p, e) => kcds.core.Chat.sendToAll('started ' + e.kind + ' ' + e.recipe));
  kcds.core.Events.on('craftingCompleted', (p, e) =>
    kcds.core.Chat.sendToAll('completed ' + e.recipe + ' ' + e.outputs.map((o) => o.amount).join(',')),
  );
  kcds.core.Events.on('craftingRefunding', (p, e) =>
    kcds.core.Chat.sendToAll('refunding ' + e.materials.map((m) => m.amount).join(',')),
  );
  kcds.core.Events.on('craftingEnded', (p, e) =>
    kcds.core.Chat.sendToAll('ended ' + e.outcome + ' ' + e.reason + ' ' + e.refunded.length),
  );
`;

function units(host, p, name) {
  return host.playerInventories
    .stateOf(p)
    .items.filter((row) => row.name === name)
    .reduce((sum, row) => sum + row.amount, 0);
}

async function cidery(auto) {
  const { host, chat } = hostWith(RESOURCE.replace('AUTO', String(auto)));
  await host.startAll();
  const p = player(host);
  host.playerInventories.operate(p, (inventory) =>
    inventory.set({ items: [{ id: 'apples', item: 'apple', amount: 3 }] }),
  );
  const [interaction] = host.worldInteractions.interactions.keys();
  return { host, chat, p, interaction, Crafting: host.api.Crafting };
}

test('two stations at one table on one key: one pressed, one held, each its own recipe', async () => {
  const { host } = hostWith(`
    for (const id of ['cider', 'perry']) {
      kcds.world.Crafting.addRecipe({
        id,
        ingredients: [{ item: 'apple', amount: 1 }],
        products: [{ item: 'potion_antiIflammatoryPotion_high', amount: 1 }],
      });
    }
    const at = { x: 1, y: 2, z: 3 };
    kcds.world.Crafting.addStation({ position: at, key: 'script_action', text: 'Cider', recipes: ['cider'] });
    kcds.world.Crafting.addStation({
      position: at,
      key: 'script_action',
      mode: 'hold',
      holdDuration: 2,
      text: 'Perry',
      recipes: ['perry'],
    });
  `);
  await host.startAll();
  const p = player(host);
  const hints = [...host.worldInteractions.interactions.values()].map((handle) => {
    const state = handle[Object.getOwnPropertySymbols(handle)[0]];
    return [state.text, state.hold, state.holdSeconds];
  });
  assert.deepEqual(hints, [
    ['Cider', false, 1],
    ['Perry', true, 2],
  ]);
  host.playerInventories.operate(p, (inventory) =>
    inventory.set({ items: [{ id: 'apples', item: 'apple', amount: 3 }] }),
  );
  const [, held] = host.worldInteractions.interactions.keys();
  host.worldInteractions.fired(p, held);
  assert.equal(host.api.Crafting.session(p).recipe, 'perry');
  assert.throws(
    () => host.api.Crafting.addStation({ position: STATION_AT, mode: 'twice' }),
    RangeError,
  );
});

test('a custom station: the hint starts its recipe, the SDK drives it, the product comes', async () => {
  const { host, chat, p, interaction, Crafting } = await cidery(false);
  assert.equal(Crafting.isAvailable('custom'), true);
  const [station] = Crafting.stations('custom');
  assert.equal(station.kind, 'custom');
  assert.deepEqual(station.recipes, ['cider']);
  host.worldInteractions.fired(p, interaction);
  assert.equal(units(host, p, 'apple'), 1);
  const session = Crafting.session(p);
  assert.equal(session.kind, 'custom');
  assert.equal(session.recipe, 'cider');
  assert.equal(Crafting.occupancy(station.id).occupant.playerId, 7);
  // Without autoProgress the time does nothing; only the SDK moves it.
  host.customCrafting.tick(Date.now() + 60000);
  assert.equal(Crafting.session(p).progress, 0);
  assert.equal(Crafting.addProgress(p, 0.5), true);
  assert.equal(Crafting.session(p).progress, 0.5);
  Crafting.addProgress(p, 0.6);
  assert.equal(Crafting.session(p), null);
  assert.equal(units(host, p, 'potion_antiIflammatoryPotion_high'), 1);
  assert.deepEqual(chat(), ['started custom cider', 'completed cider 1', 'ended success  0']);
  // One apple left: not enough for another.
  assert.deepEqual(Crafting.craft(p, station.id, 'cider'), {
    ok: false,
    reason: 'insufficientItems',
  });
  assert.equal(Crafting.addProgress(p, 1), false);
});

test('a custom craft runs by itself, and one cancelled gives its materials back', async () => {
  const { host, chat, p, interaction, Crafting } = await cidery(true);
  host.worldInteractions.fired(p, interaction);
  host.customCrafting.tick(Date.now() + 1000);
  assert.ok(Crafting.session(p).progress > 0.4 && Crafting.session(p).progress < 1);
  host.customCrafting.tick(Date.now() + 3000);
  assert.equal(Crafting.session(p), null);
  assert.equal(units(host, p, 'potion_antiIflammatoryPotion_high'), 1);
  // Two more apples, a new craft, cancelled: they come back.
  host.playerInventories.operate(p, (inventory) => inventory.add({ item: 'apple', amount: 1 }));
  const [station] = Crafting.stations('custom');
  assert.deepEqual(Crafting.craft(p, station.id, 'cider'), { ok: true, reason: '' });
  assert.equal(units(host, p, 'apple'), 0);
  assert.equal(Crafting.cancel(p), true);
  assert.equal(units(host, p, 'apple'), 2);
  assert.deepEqual(chat().slice(-3), [
    'started custom cider',
    'refunding 2',
    'ended cancelled cancelled 1',
  ]);
  assert.equal(Crafting.cancel(p), false);
});

test('one player at a custom station; leaving it or the server cancels', async () => {
  const { host, chat, p, interaction, Crafting } = await cidery(false);
  const other = player(host, 8);
  host.playerInventories.operate(other, (inventory) =>
    inventory.set({ items: [{ id: 'apples', item: 'apple', amount: 2 }] }),
  );
  host.worldInteractions.fired(p, interaction);
  const [station] = Crafting.stations('custom');
  assert.deepEqual(Crafting.craft(other, station.id, 'cider'), { ok: false, reason: 'occupied' });
  // Walking away cancels.
  host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 40, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.customCrafting.tick();
  assert.equal(Crafting.session(p), null);
  assert.equal(units(host, p, 'apple'), 3);
  assert.deepEqual(Crafting.craft(other, station.id, 'cider'), { ok: true, reason: '' });
  host.disconnect(8);
  assert.equal(chat().at(-1), 'ended cancelled disconnected 1');
});

test('custom recipes and stations are checked, and go with their resource', async () => {
  const { host, Crafting } = await cidery(false);
  assert.throws(() => Crafting.addRecipe({ id: 'x', ingredients: [], products: [] }), TypeError);
  assert.throws(
    () =>
      Crafting.addRecipe({
        id: 'x',
        ingredients: [{ item: 'no such item', amount: 1 }],
        products: [{ item: 'apple' }],
      }),
    TypeError,
  );
  assert.throws(() => Crafting.addStation({ position: STATION_AT, colour: 'red' }), TypeError);
  assert.equal(Crafting.recipe('cider').ingredients[0].amount, 2);
  await host.stop('cidery');
  assert.equal(Crafting.stations('custom').length, 0);
  assert.equal(Crafting.recipe('cider'), null);
});

test('a recipe to learn: refused until taught by the SDK or by reading its sketch', async () => {
  const { host, chat } = hostWith(`
    const sketch = kcds.players.Items.register('cidery:sketch', {
      name: 'Cider sketch',
      visual: 'andelStrazny_riddles',
      weight: 0.1,
      usable: true,
      useLabel: 'Read',
    });
    kcds.world.Crafting.addRecipe({
      id: 'cider',
      ingredients: [{ item: 'apple', amount: 1 }],
      products: [{ item: 'potion_antiIflammatoryPotion_high', amount: 1 }],
      learn: true,
    });
    kcds.world.Crafting.addRecipe({
      id: 'perry',
      ingredients: [{ item: 'apple', amount: 1 }],
      products: [{ item: 'potion_antiIflammatoryPotion_high', amount: 1 }],
      sketch,
    });
    kcds.world.Crafting.addStation({ position: { x: 1, y: 2, z: 3 }, recipes: ['cider', 'perry'] });
    kcds.core.Events.on('craftingRecipeLearned', (p, e) => kcds.core.Chat.sendToAll('learned ' + e.recipe + ' ' + e.source));
  `);
  await host.startAll();
  const p = player(host);
  const { Crafting } = host.api;
  host.playerInventories.operate(p, (inventory) =>
    inventory.set({
      items: [
        { id: 'apples', item: 'apple', amount: 3 },
        { id: 'sketch', item: 'cidery:sketch', amount: 1 },
      ],
    }),
  );
  const [station] = Crafting.stations('custom');
  assert.deepEqual(Crafting.recipe('perry').learn, true);
  assert.deepEqual(Crafting.craft(p, station.id, 'cider'), { ok: false, reason: 'notLearned' });
  assert.deepEqual(Crafting.craft(p, station.id, 'perry'), { ok: false, reason: 'notLearned' });
  assert.equal(Crafting.knowsRecipe(p, 'cider'), false);
  // The SDK teaches cider; reading the sketch teaches perry.
  assert.equal(Crafting.learnRecipe(p, 'cider'), true);
  assert.equal(Crafting.learnRecipe(p, 'cider'), true, 'known already: no second event');
  assert.equal(Crafting.learnRecipe(p, 'nothing'), false);
  const revision = host.playerInventories.stateOf(p).revision;
  assert.equal(host.customItems.use(p, 'sketch', revision), true);
  assert.deepEqual(Crafting.learnedRecipes(p), ['cider', 'perry']);
  assert.deepEqual(chat(), ['learned cider sdk', 'learned perry sketch']);
  assert.deepEqual(Crafting.craft(p, station.id, 'cider'), { ok: true, reason: '' });
  // Forgotten, and gone with the player.
  Crafting.cancel(p);
  assert.equal(Crafting.forgetRecipe(p, 'cider'), true);
  assert.equal(Crafting.knowsRecipe(p, 'cider'), false);
  host.customCrafting.left(p);
  assert.deepEqual(Crafting.learnedRecipes(p), []);
});

test('a sketch must be a usable item type', async () => {
  const { host, output } = hostWith(`
    try {
      kcds.world.Crafting.addRecipe({
        id: 'cider',
        ingredients: [{ item: 'apple', amount: 1 }],
        products: [{ item: 'apple', amount: 1 }],
        sketch: 'apple',
      });
    } catch (e) {
      kcds.core.Chat.sendToAll(e.message);
    }
  `);
  await host.startAll();
  const said = output.filter((e) => e.op === 'chat').map((e) => e.text);
  assert.deepEqual(said, [
    'Crafting.addRecipe sketch must be a usable item type (Items.register with usable: true)',
  ]);
});
