'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict'),
  fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { progressionCatalog } = require('./progression/catalog.cjs');
const recipes = require('./recipes/recipes.cjs');

// Recipe and Player.learnRecipe, forgetRecipe, knowsRecipe.

const HORSESHOE_PERK = '3190f739-995e-4c60-9aa7-d34ca3428494';

function progressionReport(perks) {
  const tracks = progressionCatalog.tracks().map(() => [5, 0, 0]);
  return { tracks, mainPoints: 0, perks };
}

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcd2s-recipes-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'smithy');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'smithy', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  return { host, output };
}

async function playerWith(perks) {
  const { host, output } = hostWith('');
  await host.startAll();
  const player = host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.updateProgression(7, progressionReport(perks));
  const orders = () => output.filter((e) => e.op === 'progressionOrder');
  return { player, orders };
}

test('Recipe: the smithing and alchemy recipes of the game', () => {
  const Recipe = recipes.define();
  assert.deepEqual(Recipe.kinds, ['smithing', 'alchemy']);
  const smithing = Recipe.all('smithing');
  const alchemy = Recipe.all('alchemy');
  assert.equal(smithing.length, 61);
  assert.equal(alchemy.length, 31);
  assert.equal(Recipe.all().length, 92);
  const horseshoe = Recipe.get('r_horseshoe');
  assert.deepEqual(horseshoe, {
    id: 'r_horseshoe',
    kind: 'smithing',
    uiName: 'ui_nm_horseshoeFarmer',
    perks: [HORSESHOE_PERK],
    ingredients: [{ item: '94d8f5f1-20a7-4840-98ad-1d198d381389', amount: 1 }],
    products: [{ item: '549ab26e-df73-43d6-ac9b-f4f74afec67f', minQuality: 40 }],
    category: 'Other',
    workpiece: 'horseshoe',
    minSkill: 1,
    dlc: false,
    dlcId: 0,
    xp: 135,
  });
  const potion = Recipe.get('potion_hair_o_dog');
  assert.equal(potion.kind, 'alchemy');
  assert.equal(potion.base, 'Water');
  assert.equal(potion.number, 1);
  assert.ok(potion.perks.length >= 1);
  assert.deepEqual(Object.keys(potion.products[0]), ['item', 'quality', 'maxYield']);
  assert.equal(Recipe.get('no such recipe'), null);
  assert.throws(() => Recipe.all('cooking'), TypeError);
  // Fresh objects: changing one changes nothing.
  horseshoe.perks.push('x');
  assert.equal(Recipe.get('r_horseshoe').perks.length, 1);
});

test('learnRecipe gives the perk of the recipe; knowsRecipe reads the report', async () => {
  const { player, orders } = await playerWith([]);
  assert.equal(player.knowsRecipe('r_horseshoe'), false);
  assert.equal(player.learnRecipe('r_horseshoe'), true);
  assert.deepEqual(orders(), [
    { op: 'progressionOrder', id: 7, kind: 'addPerk', perk: HORSESHOE_PERK },
  ]);
  assert.throws(() => player.learnRecipe('no such recipe'), TypeError);
  assert.throws(() => player.knowsRecipe(7), TypeError);
});

test('learnRecipe of a patched recipe no perk teaches is false, not an error', async () => {
  const patch = `<database name="barbora"><AlchemyRecipes version="1">
    <AlchemyRecipe RecipeId="901" BaseMaterial="Water" UIName="recipe_kit_brew_name">
      <Ingredients><AlchemyIngredient IngredientItemId="B9DE1D84-A0C1-4B81-9F60-8D7FBB3CB9D4" Quantity="1" /></Ingredients>
      <Products><AlchemyProduct Quality="1" ProductItemId="7a1e8447-4449-473b-aac9-63f7a324fa0b" MaxYield="1" /></Products>
    </AlchemyRecipe></AlchemyRecipes></database>`;
  recipes.addTablePatch('kit', patch);
  try {
    const { player, orders } = await playerWith([]);
    assert.equal(player.knowsRecipe('kit_brew'), false);
    assert.equal(player.learnRecipe('kit_brew'), false);
    assert.deepEqual(orders(), []);
  } finally {
    recipes.removeOwner('kit');
  }
});

test('a known recipe is not taught again; forgetRecipe takes its perks', async () => {
  const { player, orders } = await playerWith([HORSESHOE_PERK]);
  assert.equal(player.knowsRecipe('r_horseshoe'), true);
  assert.equal(player.learnRecipe('r_horseshoe'), true);
  assert.deepEqual(orders(), []);
  assert.equal(player.forgetRecipe('r_horseshoe'), true);
  assert.deepEqual(orders(), [
    { op: 'progressionOrder', id: 7, kind: 'removePerk', perk: HORSESHOE_PERK },
  ]);
  assert.equal(player.forgetRecipe('r_axebattle01'), false);
});

test('Crafting.knowledge and grantKnowledge: alchemy recipes whole or in part', async () => {
  const potion = recipes.define().get('potion_hair_o_dog');
  const { host, output } = hostWith('');
  await host.startAll();
  const player = host.updatePlayer({
    id: 7,
    ready: true,
    position: { x: 1, y: 2, z: 3 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  host.updateProgression(7, progressionReport([potion.perks[0]]));
  const { Crafting } = host.makeAPI();
  // A recipe the game knows is the whole mask; nothing else is listed.
  assert.deepEqual({ ...Crafting.knowledge(player) }, { potion_hair_o_dog: 536870911 });
  // A part is kept for the session; the game forgets the recipe.
  assert.equal(Crafting.grantKnowledge(player, 'potion_hair_o_dog', 5), true);
  const orders = () => output.filter((e) => e.op === 'progressionOrder');
  assert.equal(orders().at(-1).kind, 'removePerk');
  host.updateProgression(7, progressionReport([]));
  assert.deepEqual({ ...Crafting.knowledge(player) }, { potion_hair_o_dog: 5 });
  // The whole mask teaches it (by its native number too); 0 forgets.
  assert.equal(Crafting.grantKnowledge(player, potion.number, 536870911), true);
  assert.deepEqual(orders().at(-1), {
    op: 'progressionOrder',
    id: 7,
    kind: 'addPerk',
    perk: potion.perks[0],
  });
  assert.equal(Crafting.grantKnowledge(player, 'potion_hair_o_dog', 0), true);
  assert.deepEqual({ ...Crafting.knowledge(player) }, {});
  // Refused: an unknown recipe, a smithing one, a mask out of range, a gone player.
  assert.equal(Crafting.grantKnowledge(player, 'no such recipe', 1), false);
  assert.equal(Crafting.grantKnowledge(player, 'r_horseshoe', 1), false);
  assert.equal(Crafting.grantKnowledge(player, 'potion_hair_o_dog', 536870912), false);
  assert.equal(Crafting.grantKnowledge(player, 'potion_hair_o_dog', 1.5), false);
  host.disconnect(7);
  assert.equal(Crafting.grantKnowledge(player, 'potion_hair_o_dog', 1), false);
});
