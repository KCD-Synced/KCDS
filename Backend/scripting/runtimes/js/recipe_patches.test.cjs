'use strict';

// A resource's rows for the game's own smithy: a BlacksmithRecipes patch joins the
// server's recipe catalog while the resource runs.
const test = require('node:test');
const assert = require('node:assert/strict');
const recipes = require('./recipes/recipes.cjs');

const PATCH = `<?xml version="1.0" encoding="us-ascii"?>
<database name="barbora">
  <BlacksmithRecipes version="1">
    <BlacksmithRecipe Id="kit_shoe" UIIcon="horseshoeFarmer" Category="Other" MinSkillLevel="3" WorkpieceId="horseshoe" PerkId="3190f739-995e-4c60-9aa7-d34ca3428494">
      <UIName StringName="kcds_recipe_kit_shoe" Text="Kit &amp; shoe" />
      <UIDesc StringName="kcds_recipe_kit_shoe_desc" Text="A recipe a server added." />
      <Ingredients>
        <BlacksmithRecipeIngredient ItemClassId="94d8f5f1-20a7-4840-98ad-1d198d381389" Amount="2" />
      </Ingredients>
      <Products>
        <BlacksmithRecipeProduct ItemClassId="549ab26e-df73-43d6-ac9b-f4f74afec67f" MinimalQuality="40" />
      </Products>
    </BlacksmithRecipe>
  </BlacksmithRecipes>
</database>`;

test('a smithy patch row is read in the catalog shape', () => {
  const [row] = recipes.smithingRows(PATCH);
  assert.deepEqual(row, {
    id: 'kit_shoe',
    kind: 'smithing',
    perks: ['3190f739-995e-4c60-9aa7-d34ca3428494'],
    uiName: 'kcds_recipe_kit_shoe',
    uiText: 'Kit & shoe',
    category: 'Other',
    workpiece: 'horseshoe',
    minSkill: 3,
    dlcId: 0,
    ingredients: [['94d8f5f1-20a7-4840-98ad-1d198d381389', 2]],
    products: [['549ab26e-df73-43d6-ac9b-f4f74afec67f', 40]],
  });
});

test('a resource adds its rows while it runs, and they go with it', () => {
  const catalog = recipes.define();
  const before = catalog.all('smithing').length;
  assert.equal(catalog.get('kit_shoe'), null);
  assert.deepEqual(recipes.addTablePatch('kit', PATCH), ['kit_shoe']);
  assert.equal(catalog.all('smithing').length, before + 1);
  const recipe = catalog.get('kit_shoe');
  assert.equal(recipe.kind, 'smithing');
  assert.equal(recipe.minSkill, 3);
  assert.deepEqual(recipe.ingredients, [
    { item: '94d8f5f1-20a7-4840-98ad-1d198d381389', amount: 2 },
  ]);
  recipes.removeOwner('kit');
  assert.equal(catalog.get('kit_shoe'), null);
  assert.equal(catalog.all('smithing').length, before);
});

test('a row of a game recipe id stands in for it, and the game recipe comes back', () => {
  const catalog = recipes.define();
  const game = catalog.get('r_horseshoe');
  recipes.addTablePatch('kit', PATCH.replace('Id="kit_shoe"', 'Id="r_horseshoe"'));
  assert.equal(catalog.get('r_horseshoe').minSkill, 3);
  recipes.removeOwner('kit');
  assert.deepEqual(catalog.get('r_horseshoe'), game);
});

test('an alchemy patch row joins the catalog by its UIName, with its number', () => {
  const patch = `<database name="barbora"><AlchemyRecipes version="1">
    <AlchemyRecipe RecipeId="900" BaseMaterial="Water" UIName="recipe_kit_tonic_name" BaseQualityRewardXP="12">
      <Ingredients><AlchemyIngredient IngredientItemId="B9DE1D84-A0C1-4B81-9F60-8D7FBB3CB9D4" Quantity="2" /></Ingredients>
      <Products><AlchemyProduct Quality="1" ProductItemId="7a1e8447-4449-473b-aac9-63f7a324fa0b" MaxYield="2" /></Products>
      <Steps><AlchemyRecipeStep StepId="1" StepType="Trivial" Condition="PotionBase()" /></Steps>
    </AlchemyRecipe></AlchemyRecipes></database>`;
  const catalog = recipes.define();
  assert.deepEqual(recipes.addTablePatch('kit', patch), ['kit_tonic']);
  const tonic = catalog.get('kit_tonic');
  assert.equal(tonic.kind, 'alchemy');
  assert.equal(tonic.number, 900);
  assert.equal(tonic.base, 'Water');
  assert.equal(tonic.xp, 12);
  assert.deepEqual(tonic.ingredients, [
    { item: 'b9de1d84-a0c1-4b81-9f60-8d7fbb3cb9d4', amount: 2 },
  ]);
  assert.deepEqual(tonic.products, [
    { item: '7a1e8447-4449-473b-aac9-63f7a324fa0b', quality: 1, maxYield: 2 },
  ]);
  recipes.removeOwner('kit');
  assert.equal(catalog.get('kit_tonic'), null);
});
