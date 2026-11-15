'use strict';
// A helmet smiths forge at the game's own smithies. The anvil shows a helmet workpiece of this
// resource: the game's half-made helm (the prop its smithies have lying around), held in the tongs
// like the game's axe. Its recipe turns steel and iron into a kettle hat, and a better forging
// gives a better one.
//
// Every smith can use the recipe: it names no perk. Its quality thresholds are the game's 0 to 100
// smithing result, as in the game's BlacksmithRecipes.xml.

const { Crafting } = kcds.world;

// The workpiece: the game's half-made helm, in the tongs like the axe, with two strike zones.
const HELM_WORKPIECE = {
  id: 'helm_kettle',
  like: 'axe',
  model: 'objects/manmade/task_specific_props/metal_industry/smithing/semifinished_helm.cgf',
  zones: 2,
};

// The game's item classes the recipe uses.
const STEEL = '3c1c0ae2-731e-40c1-a917-024fb3f000da'; // bsmt_steelNormal
const IRON = '92aa6120-028e-48ee-8ed1-1c5f91afaa26'; // bsmt_ironGood
const KETTLE_HAT_PLAIN = '868581d6-f9ab-4ae2-b074-9200e7d054bd'; // KettleHat01_m01_E1
const KETTLE_HAT_FINE = '25bd008c-f308-4521-8a4f-a1feeb76ea3c'; // KettleHat03_m01_B3

const KETTLE_HAT_RECIPE = {
  kind: 'smithing',
  id: 'helm_kettle_hat',
  name: 'Kettle hat',
  // The game's icon of the plain kettle hat: its IconId in the game's item table, which is not
  // always the item's name.
  icon: 'KettleHat01_m01_E2',
  description: 'A plain iron helmet with a wide brim. Forge it well and it comes out a fine one.',
  category: 'Other',
  workpiece: HELM_WORKPIECE.id,
  ingredients: [
    { item: STEEL, amount: 2 },
    { item: IRON, amount: 1 },
  ],
  // The best product whose threshold the forging reaches.
  products: [
    { item: KETTLE_HAT_PLAIN, threshold: 0 },
    { item: KETTLE_HAT_FINE, threshold: 60 },
  ],
};

// The workpiece before the recipe that names it.
Crafting.defineWorkpiece(HELM_WORKPIECE);
Crafting.defineRecipe(KETTLE_HAT_RECIPE);
console.info(`helm-smithing: '${KETTLE_HAT_RECIPE.name}' at every smithy`);
