'use strict';

// The game's smithing and alchemy recipes: what Recipe.all and Recipe.get answer and
// what Player.learnRecipe, forgetRecipe and knowsRecipe go by. A player knows a recipe through a
// perk (smithing: the recipe's own; alchemy: any of the perks the game gives the recipe), so
// learning one at once is giving that perk and forgetting it is taking its perks away.
// catalog.json is built from a game installation by Dev/tools/recipes/build_catalog.py.
//
// A resource may add recipes to the game's own tables: its
// replace/Libs/Tables/minigame/BlacksmithRecipes__<name>.xml and AlchemyRecipe__<name>.xml rows,
// which every client loads into the game's smithy and alchemy table
// (Client/runtime/crafting/recipe_tables.hpp), join this catalog while the resource runs
// (addTablePatch, removeOwner); a row of a game recipe's id stands in for it.
//
// A resource may register a recipe by its definition (Crafting.defineRecipe,
// registered.cjs): it joins the catalog for the rest of the server's life, and its table patch
// goes to every client (registeredTables).

const catalog = require('./catalog.json');
const registered = require('./registered.cjs');

const KINDS = ['smithing', 'alchemy'];
// The craftsmanship XP the game gives for a finished workpiece, before the player's multipliers
// (rpg_param BlacksmithingFinishingXP; smithing recipes carry no XP of their own).
const SMITHING_FINISHING_XP = catalog.smithingFinishingXp;
const GAME_RECIPES = KINDS.flatMap((kind) =>
  catalog[kind].map((recipe) => ({ ...recipe, kind, source: 'game' })),
);
const GAME_ALCHEMY_NUMBERS = new Set(catalog.alchemy.map((recipe) => recipe.number));
// The catalog now: the game's recipes and the rows of the running resources' table patches.
const RECIPES = [...GAME_RECIPES];
const BY_ID = new Map(RECIPES.map((recipe) => [recipe.id, recipe]));
// Resource name -> the rows its patches added.
const PATCHED = new Map();
// Resource name -> its perk_recipe rows ({ perk, number }): perks of its own that make an alchemy
// recipe known, as the game's perk_recipe.xml does for its recipes.
const PERK_LINKS = new Map();
// "<kind>:<id>" -> { owner, key (the definition's JSON), row, table: { file, patch } } of a
// registered recipe, in the order they were registered.
const REGISTERED = new Map();

function ingredientInfo([item, amount]) {
  return { item, amount };
}

function productInfo(kind, product) {
  if (kind === 'smithing') {
    const [item, minQuality] = product;
    return { item, minQuality };
  }
  const [item, quality, maxYield] = product;
  return { item, quality, maxYield };
}

// A recipe as resources see it: a fresh object, so changing it changes nothing here.
function recipeInfo(recipe) {
  const info = {
    id: recipe.id,
    kind: recipe.kind,
    uiName: recipe.uiName,
    perks: [...recipe.perks],
    ingredients: recipe.ingredients.map(ingredientInfo),
    products: recipe.products.map((product) => productInfo(recipe.kind, product)),
  };
  if (recipe.kind === 'smithing') {
    info.category = recipe.category;
    info.workpiece = recipe.workpiece;
    info.minSkill = recipe.minSkill;
    info.dlc = recipe.dlcId !== 0;
    info.dlcId = recipe.dlcId;
    info.xp = SMITHING_FINISHING_XP;
  } else {
    info.base = recipe.base;
    info.number = recipe.number;
    info.dlcId = recipe.dlcId ?? 0;
    info.xp = recipe.xp;
  }
  return info;
}

// The catalog's recipe of that id; throws for anything else.
function requireRecipe(id) {
  const recipe = typeof id === 'string' ? BY_ID.get(id) : undefined;
  if (!recipe) {
    throw new TypeError(`Unknown recipe: ${String(id)}`);
  }
  return recipe;
}

// The attributes of an XML element's opening tag ('<Name a="1" b="2">'), unescaped.
function attributesOf(tag) {
  const attributes = {};
  for (const [, name, value] of tag.matchAll(/([A-Za-z_][\w.-]*)="([^"]*)"/g)) {
    attributes[name] = value
      .replaceAll('&quot;', '"')
      .replaceAll('&apos;', "'")
      .replaceAll('&lt;', '<')
      .replaceAll('&gt;', '>')
      .replaceAll('&amp;', '&');
  }
  return attributes;
}

// A BlacksmithRecipes patch's rows, in the catalog's shape: { id, perks, uiName, uiText,
// category, workpiece, minSkill, dlcId, ingredients: [[item, amount]], products: [[item,
// minQuality]] }.
function smithingRows(text) {
  const rows = [];
  for (const [, open, body] of text.matchAll(
    /<BlacksmithRecipe(\s[^>]*)>([\s\S]*?)<\/BlacksmithRecipe>/g,
  )) {
    const row = attributesOf(open);
    if (!row.Id) {
      continue;
    }
    const name = attributesOf(/<UIName(\s[^>]*)\/?>/.exec(body)?.[1] ?? '');
    const ingredients = [...body.matchAll(/<BlacksmithRecipeIngredient(\s[^>]*)\/?>/g)].map(
      ([, tag]) => {
        const ingredient = attributesOf(tag);
        return [ingredient.ItemClassId, Number(ingredient.Amount) || 1];
      },
    );
    const products = [...body.matchAll(/<BlacksmithRecipeProduct(\s[^>]*)\/?>/g)].map(([, tag]) => {
      const product = attributesOf(tag);
      return [product.ItemClassId, Number(product.MinimalQuality) || 0];
    });
    rows.push({
      id: row.Id,
      kind: 'smithing',
      perks: row.PerkId ? [row.PerkId] : [],
      uiName: name.StringName ?? '',
      uiText: name.Text ?? '',
      category: row.Category ?? 'Other',
      workpiece: row.WorkpieceId ?? '',
      minSkill: Number(row.MinSkillLevel) || 0,
      dlcId: Number(row.DlcId) || 0,
      ingredients,
      products,
    });
  }
  return rows;
}

// An AlchemyRecipe patch's rows, in the catalog's shape: { id (its UIName without recipe_ and
// _name, as the game's), number (RecipeId), perks: [], uiName, base, xp, ingredients: [[item,
// quantity]], products: [[item, quality, maxYield]] }. The game knows an alchemy recipe through
// its perk_recipe rows, which a patch of this table does not give: without a resource's own
// perk_recipe row (addPerkRecipePatch) a row is brewed by its steps and is not in the player's book.
function alchemyRows(text) {
  const rows = [];
  for (const [, open, body] of text.matchAll(
    /<AlchemyRecipe(\s[^>]*)>([\s\S]*?)<\/AlchemyRecipe>/g,
  )) {
    const row = attributesOf(open);
    if (!row.RecipeId || !row.UIName) {
      continue;
    }
    let id = row.UIName;
    if (id.startsWith('recipe_')) {
      id = id.slice('recipe_'.length);
    }
    if (id.endsWith('_name')) {
      id = id.slice(0, -'_name'.length);
    }
    rows.push({
      id,
      kind: 'alchemy',
      number: Number(row.RecipeId),
      perks: [],
      uiName: row.UIName,
      base: row.BaseMaterial ?? '',
      xp: Number(row.BaseQualityRewardXP) || 0,
      ingredients: [...body.matchAll(/<AlchemyIngredient(\s[^>]*)\/?>/g)].map(([, tag]) => {
        const ingredient = attributesOf(tag);
        return [
          String(ingredient.IngredientItemId).toLowerCase(),
          Number(ingredient.Quantity) || 1,
        ];
      }),
      products: [...body.matchAll(/<AlchemyProduct(\s[^>]*)\/?>/g)].map(([, tag]) => {
        const product = attributesOf(tag);
        return [
          String(product.ProductItemId).toLowerCase(),
          Number(product.Quality) || 0,
          Number(product.MaxYield) || 1,
        ];
      }),
    });
  }
  return rows;
}

// Puts the catalog together again from the game's recipes and the running patches; a patch's
// row of a game recipe's id stands in for it.
function rebuild() {
  const rows = new Map(GAME_RECIPES.map((recipe) => [recipe.id, recipe]));
  for (const patched of PATCHED.values()) {
    for (const row of patched) {
      rows.set(row.id, { ...row, source: 'mod' });
    }
  }
  for (const { row } of REGISTERED.values()) {
    rows.set(row.id, { ...row, source: 'script' });
  }
  for (const links of PERK_LINKS.values()) {
    for (const { perk, number } of links) {
      for (const [id, recipe] of rows) {
        if (recipe.kind === 'alchemy' && recipe.number === number && !recipe.perks.includes(perk)) {
          rows.set(id, { ...recipe, perks: [...recipe.perks, perk] });
        }
      }
    }
  }
  RECIPES.length = 0;
  RECIPES.push(...rows.values());
  BY_ID.clear();
  for (const recipe of RECIPES) {
    BY_ID.set(recipe.id, recipe);
  }
}

// A resource's patch of the game's BlacksmithRecipes or AlchemyRecipe table (its text): its rows
// join the catalog. The ids it added.
function addTablePatch(owner, text) {
  const rows = [...smithingRows(String(text)), ...alchemyRows(String(text))];
  PATCHED.set(owner, [...(PATCHED.get(owner) ?? []), ...rows]);
  rebuild();
  return rows.map((row) => row.id);
}

// A resource's patch of the game's perk_recipe table (its text, rows
// <perk_recipe perk_id="<GUID>" recipe_id="<RecipeId>" />): each perk makes that alchemy recipe
// known, so learning the recipe gives the perk and the game's book shows it. Players' games load
// the same file with the game's tables at their next start. The links it added, as "perk -> number".
function addPerkRecipePatch(owner, text) {
  const links = [];
  for (const [, tag] of String(text).matchAll(/<perk_recipe(\s[^>]*)\/?>/g)) {
    const row = attributesOf(tag);
    const number = Number(row.recipe_id);
    if (!row.perk_id || !Number.isInteger(number)) {
      continue;
    }
    links.push({ perk: row.perk_id.toLowerCase(), number });
  }
  PERK_LINKS.set(owner, [...(PERK_LINKS.get(owner) ?? []), ...links]);
  rebuild();
  return links.map(({ perk, number }) => `${perk} -> ${number}`);
}

// Whether a recipe of the game or of a running resource's patch has this id (smithing) or
// RecipeId (alchemy).
function usedElsewhere(kind, id) {
  return RECIPES.some(
    (recipe) =>
      recipe.kind === kind &&
      recipe.source !== 'script' &&
      (kind === 'alchemy' ? recipe.number === id : recipe.id === id),
  );
}

// Registers a resource's recipe definition (Crafting.defineRecipe): its table patch
// { file, patch } for the clients, or null when the same definition was registered before.
// Throws for an invalid definition, an id already taken by the game or another resource, or a different
// definition under an id already registered.
function defineRecipe(owner, definition) {
  const recipe = registered.normalize(definition, GAME_ALCHEMY_NUMBERS);
  const slot = `${recipe.kind}:${recipe.id}`;
  const key = JSON.stringify(recipe);
  const before = REGISTERED.get(slot);
  if (before) {
    if (before.key === key) {
      return null;
    }
    throw new TypeError(
      `Crafting.defineRecipe: ${recipe.kind} recipe ${recipe.id} is already registered ` +
        'with another definition',
    );
  }
  if (usedElsewhere(recipe.kind, recipe.id)) {
    throw new TypeError(
      `Crafting.defineRecipe: the game or a resource uses ${recipe.kind} recipe ${recipe.id}`,
    );
  }
  const table = registered.patchOf(recipe);
  REGISTERED.set(slot, { owner, key, row: registered.rowOf(recipe), table, recipe });
  rebuild();
  return table;
}

// The table patches of the registered recipes, oldest first: for a client whose world became
// ready.
function registeredTables() {
  return [...REGISTERED.values()].map(({ table }) => table);
}

// Where the catalog's recipe of that id comes from: 'game', 'mod' (a resource's table patch) or
// 'script' (Crafting.defineRecipe); null for none.
function sourceOf(id) {
  return BY_ID.get(id)?.source ?? null;
}

// The resource stopped: its rows leave the catalog.
function removeOwner(owner) {
  const patched = PATCHED.delete(owner);
  const linked = PERK_LINKS.delete(owner);
  if (patched || linked) {
    rebuild();
  }
}

// The resources' Recipe: the game's recipes, read only.
function define() {
  return Object.freeze({
    kinds: Object.freeze([...KINDS]),
    // Every recipe, or every one of a kind ('smithing' or 'alchemy').
    all(kind) {
      if (kind !== undefined && !KINDS.includes(kind)) {
        throw new TypeError(`Unknown recipe kind: ${String(kind)}`);
      }
      return RECIPES.filter((recipe) => kind === undefined || recipe.kind === kind).map(recipeInfo);
    },
    // The recipe of that id, or null.
    get(id) {
      const recipe = typeof id === 'string' ? BY_ID.get(id) : undefined;
      return recipe ? recipeInfo(recipe) : null;
    },
  });
}

module.exports = {
  define,
  requireRecipe,
  addTablePatch,
  addPerkRecipePatch,
  defineRecipe,
  registeredTables,
  sourceOf,
  removeOwner,
  smithingRows,
  alchemyRows,
  KINDS,
  SMITHING_FINISHING_XP,
};
