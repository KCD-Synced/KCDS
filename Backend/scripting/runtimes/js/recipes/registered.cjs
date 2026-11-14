'use strict';

// The recipes resources register: Crafting.defineRecipe(definition). A definition
// is checked field for field as a row of the game's AlchemyRecipe.xml or BlacksmithRecipes.xml,
// then written as a patch of that table, which every client loads into its game's recipe table
// (op recipeTable; Client/runtime/crafting/recipe_tables.hpp), and its row joins the recipe
// catalog (recipes.cjs) as the script's.
//
// Texts are shown as written: they travel under KCDS's own keys (kcds_recipe_...). A smithing row
// carries them as the game's own UIName and UIDesc elements do; an alchemy row names its keys in
// attributes, so their texts ride in a comment the game's parser skips and the client reads
// (recipe_tables.cpp give_texts). A recipe shows the item icon `icon` names, or its own iconImage,
// fitted onto the game's 64x64 icon square and sent with the patch as an item icon of its own
// (picture.cjs). An alchemy recipe's two pictures in the recipe book (productImage, 256x256, and
// recipeImage, 1024x1024, as DXT5) travel beside the patch as pictures of its RecipeId, the names
// the book loads them by (Libs/UI/Textures/Books/{Products,Recipes}/<id>_ui.dds); with
// bookPicturesFrom the clients show the game's own two of that recipe instead.

const { lookupItemClass } = require('../inventory/catalog.cjs');
const { pictureDds, bookPictureDds, pngOf } = require('./picture.cjs');
const workpieces = require('./workpieces.cjs');

const ALCHEMY_ID_MAX = 2147483647;
const SMITHING_ID = /^[A-Za-z0-9_.-]{1,128}$/;
const GUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const ICON = /^[A-Za-z0-9_.-]{1,128}$/;
const LIST_MAX = 64;
const STEPS_MAX = 28;
const TEXT_MAX = 2048;
const PICTURES_MAX_BYTES = 256 * 1024;
// The table patch, as long as one message to the clients carries it (recipe_tables.hpp).
const PATCH_MAX_BYTES = 40 * 1024;
const BASES = ['Water', 'Wine', 'Oil', 'Spiritus'];
const STEP_TYPES = ['Trivial', 'Easy', 'Hard'];
const CATEGORIES = ['ShortSword', 'LongSword', 'Axe', 'Other'];
// The game's item icon square, which a recipe's own icon is fitted onto.
const ICON_SIZE = 64;
// The recipe book's squares, as the game's own pictures are: the product's and the recipe's.
const PRODUCT_SIZE = 256;
const RECIPE_SIZE = 1024;
const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
const ITEM = "'([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})'";
const NUMBER = '(\\d+(?:\\.\\d+)?)';
// The terms of a step's condition the game's own syntax has.
const CONDITION_TERMS = [
  /^PotionBase\(\)$/,
  new RegExp(`^IngredientCount\\(${ITEM}\\) ?(==|>=) ?(\\d+)$`),
  new RegExp(`^IsMilled\\(${ITEM}, ?([01])\\)$`),
  new RegExp(`^WeakBoilingTime\\(${ITEM}, ?${NUMBER}\\)$`),
  new RegExp(`^StrongBoilingTime\\(${ITEM}, ?${NUMBER}\\)$`),
  /^IsBaseDistilled\(([01])\)$/,
  /^IsResultMilled\(([01])\)$/,
];

function fail(message) {
  throw new TypeError(`Crafting.defineRecipe: ${message}`);
}

function isInteger(value, min, max) {
  return Number.isInteger(value) && value >= min && value <= max;
}

function text(value, name, required) {
  if (value === undefined && !required) {
    return undefined;
  }
  if (typeof value !== 'string' || value.length === 0 || value.length > TEXT_MAX) {
    fail(`${name} must be a text of 1 to ${TEXT_MAX} characters`);
  }
  return value;
}

function oneOf(value, values, name) {
  if (!values.includes(value)) {
    fail(`${name} must be one of ${values.join(', ')}`);
  }
  return value;
}

// An item class of the game (or a streamed mod's): its GUID in lower case.
function itemOf(value, name) {
  const id = typeof value === 'string' ? value.toLowerCase() : '';
  const known = GUID.test(id) ? lookupItemClass(id) : undefined;
  if (!known || known.id !== id) {
    fail(`${name}: ${String(value)} is not an item of the game's catalog`);
  }
  return known;
}

// The bytes of a base64 PNG, or a throw.
function pictureBytes(value, name) {
  if (value === undefined) {
    return 0;
  }
  const bytes = typeof value === 'string' ? Buffer.from(value, 'base64') : Buffer.alloc(0);
  if (bytes.length < 24 || !bytes.subarray(0, 8).equals(PNG_SIGNATURE)) {
    fail(`${name} must be a base64 PNG`);
  }
  const width = bytes.readUInt32BE(16);
  const height = bytes.readUInt32BE(20);
  if (width < 1 || height < 1 || width > 4096 || height > 4096) {
    fail(`${name} must be at most 4096x4096 pixels`);
  }
  return bytes.length;
}

function ingredientsOf(list) {
  if (!Array.isArray(list) || list.length < 1 || list.length > LIST_MAX) {
    fail(`ingredients must list 1 to ${LIST_MAX} items`);
  }
  const seen = new Set();
  return list.map((entry, index) => {
    const item = itemOf(entry?.item, `ingredients[${index}].item`);
    if (seen.has(item.id)) {
      fail(`ingredients[${index}]: ${item.id} is listed twice`);
    }
    seen.add(item.id);
    if (!isInteger(entry.amount, 1, 1000)) {
      fail(`ingredients[${index}].amount must be a whole number from 1 to 1000`);
    }
    return { item: item.id, amount: entry.amount };
  });
}

function productsOf(list, kind) {
  if (!Array.isArray(list) || list.length < 1 || list.length > LIST_MAX) {
    fail(`products must list 1 to ${LIST_MAX} items`);
  }
  return list.map((entry, index) => {
    const item = itemOf(entry?.item, `products[${index}].item`);
    if (item.quest) {
      fail(`products[${index}]: ${item.id} is a quest item`);
    }
    if (kind === 'alchemy') {
      if (!isInteger(entry.threshold, 1, 4)) {
        fail(`products[${index}].threshold must be a grade from 1 to 4`);
      }
      const maxYield = entry.maxYield ?? 1;
      if (!isInteger(maxYield, 1, 64)) {
        fail(`products[${index}].maxYield must be a whole number from 1 to 64`);
      }
      return { item: item.id, threshold: entry.threshold, maxYield };
    }
    if (!isInteger(entry.threshold, 0, 100)) {
      fail(`products[${index}].threshold must be a quality percent from 0 to 100`);
    }
    return { item: item.id, threshold: entry.threshold, maxYield: 1 };
  });
}

// A step's condition: terms of the game's syntax joined by AND, item GUIDs in lower case.
function conditionOf(value, index) {
  if (typeof value !== 'string' || value.length === 0 || value.length > TEXT_MAX) {
    fail(`steps[${index}].condition must be a text`);
  }
  for (const term of value.split(' AND ')) {
    const trimmed = term.trim();
    const match = CONDITION_TERMS.map((pattern) => pattern.exec(trimmed)).find(Boolean);
    if (!match) {
      fail(`steps[${index}].condition: '${trimmed}' is not of the game's condition syntax`);
    }
    const item = match.find((part, at) => at > 0 && GUID.test(part ?? ''));
    if (item) {
      itemOf(item, `steps[${index}].condition`);
    }
  }
  return value;
}

function stepsOf(list) {
  if (!Array.isArray(list) || list.length < 1 || list.length > STEPS_MAX) {
    fail(`steps must list 1 to ${STEPS_MAX} steps`);
  }
  const seen = new Set();
  return list.map((step, index) => {
    if (!isInteger(step?.id, 1, STEPS_MAX) || seen.has(step.id)) {
      fail(`steps[${index}].id must be a distinct number from 1 to ${STEPS_MAX}`);
    }
    seen.add(step.id);
    const feedbackPriority = step.feedbackPriority ?? 0;
    if (!isInteger(feedbackPriority, 0, 1000)) {
      fail(`steps[${index}].feedbackPriority must be a whole number from 0 to 1000`);
    }
    return {
      id: step.id,
      type: oneOf(step.type, STEP_TYPES, `steps[${index}].type`),
      condition: conditionOf(step.condition, index),
      feedbackPriority,
      text: text(step.text, `steps[${index}].text`, false),
    };
  });
}

// The definition checked and in a plain form; `used(kind, id)` tells an id the game or another
// recipe uses, `alchemyNumbers` the game's alchemy RecipeIds (bookPicturesFrom).
function normalize(definition, alchemyNumbers) {
  if (!definition || typeof definition !== 'object') {
    fail('takes a recipe definition');
  }
  const kind = oneOf(definition.kind, ['alchemy', 'smithing'], 'kind');
  const alchemy = kind === 'alchemy';
  if (alchemy ? !isInteger(definition.id, 1, ALCHEMY_ID_MAX) : !SMITHING_ID.test(definition.id)) {
    fail(
      alchemy
        ? `id must be a RecipeId from 1 to ${ALCHEMY_ID_MAX}`
        : "id must be 1 to 128 letters, digits, '_', '-' or '.'",
    );
  }
  if (definition.icon !== undefined && !ICON.test(definition.icon)) {
    fail('icon must name one of the game item icons');
  }
  const pictures =
    pictureBytes(definition.iconImage, 'iconImage') +
    pictureBytes(definition.productImage, 'productImage') +
    pictureBytes(definition.recipeImage, 'recipeImage');
  if (pictures > PICTURES_MAX_BYTES) {
    fail('the pictures together must be at most 256 KiB of PNG');
  }
  const dlcId = definition.dlcId ?? 0;
  if (!isInteger(dlcId, 0, 65535)) {
    fail('dlcId must be a whole number from 0 to 65535');
  }
  const recipe = {
    kind,
    id: definition.id,
    name: text(definition.name, 'name', true),
    icon: definition.icon,
    dlcId,
    ingredients: ingredientsOf(definition.ingredients),
    products: productsOf(definition.products, kind),
    pictures: pictures > 0,
    iconImage: definition.iconImage,
  };
  // The recipe's own icon: read now, so a picture the game could not show throws here.
  if (definition.iconImage !== undefined) {
    pictureDds(definition.iconImage, ICON_SIZE, 'iconImage');
  }
  if (alchemy) {
    if (
      definition.bookPicturesFrom !== undefined &&
      !alchemyNumbers.has(definition.bookPicturesFrom)
    ) {
      fail("bookPicturesFrom must be the RecipeId of one of the game's alchemy recipes");
    }
    // The book's own pictures: read now too (bookPicturesFrom shows the game's instead).
    for (const name of ['productImage', 'recipeImage']) {
      if (definition[name] !== undefined) {
        pngOf(definition[name], name);
      }
    }
    const baseXP = definition.baseXP ?? 0;
    if (typeof baseXP !== 'number' || !Number.isFinite(baseXP) || baseXP < 0 || baseXP > 100000) {
      fail('baseXP must be a number from 0 to 100000');
    }
    return {
      ...recipe,
      base: oneOf(definition.base, BASES, 'base'),
      usage: text(definition.usage, 'usage', false),
      baseXP,
      bookPicturesFrom: definition.bookPicturesFrom,
      productImage: definition.productImage,
      recipeImage: definition.recipeImage,
      steps: stepsOf(definition.steps),
    };
  }
  const minSkill = definition.minSkill ?? 0;
  if (!isInteger(minSkill, 0, 30)) {
    fail('minSkill must be a whole number from 0 to 30');
  }
  if (definition.perk !== undefined && !GUID.test(String(definition.perk).toLowerCase())) {
    fail('perk must be a perk GUID');
  }
  return {
    ...recipe,
    minSkill,
    perk: definition.perk === undefined ? undefined : String(definition.perk).toLowerCase(),
    category: oneOf(definition.category, CATEGORIES, 'category'),
    // One of the game's workpieces or one a resource registered (Crafting.defineWorkpiece).
    workpiece: oneOf(definition.workpiece, workpieces.workpieceIds(), 'workpiece'),
    description: text(definition.description, 'description', false),
  };
}

function escape(value) {
  return String(value)
    .replaceAll('&', '&amp;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&apos;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;');
}

function attributes(pairs) {
  return pairs
    .filter(([, value]) => value !== undefined)
    .map(([name, value]) => ` ${name}="${escape(value)}"`)
    .join('');
}

// The name of the item icon a recipe's own iconImage is shown as, or null without one.
function iconNameOf(recipe) {
  return recipe.iconImage === undefined ? null : keyOf(recipe, 'icon');
}

// The texts' keys: KCDS's own, one set per recipe.
function keyOf(recipe, part) {
  const id = String(recipe.id)
    .toLowerCase()
    .replace(/[^a-z0-9_]/g, '_');
  return `kcds_recipe_${recipe.kind}_${id}_${part}`;
}

function smithingPatch(recipe) {
  const lines = [
    '<?xml version="1.0" encoding="utf-8"?>',
    '<database name="barbora">',
    '  <BlacksmithRecipes version="1">',
    `    <BlacksmithRecipe${attributes([
      ['Id', recipe.id],
      ['UIIcon', iconNameOf(recipe) ?? recipe.icon],
      ['Category', recipe.category],
      ['MinSkillLevel', recipe.minSkill],
      ['WorkpieceId', recipe.workpiece],
      ['PerkId', recipe.perk],
      ['DlcId', recipe.dlcId || undefined],
    ])}>`,
    `      <UIName${attributes([
      ['StringName', keyOf(recipe, 'name')],
      ['Text', recipe.name],
    ])} />`,
  ];
  if (recipe.description !== undefined) {
    lines.push(
      `      <UIDesc${attributes([
        ['StringName', keyOf(recipe, 'desc')],
        ['Text', recipe.description],
      ])} />`,
    );
  }
  lines.push('      <Ingredients>');
  for (const { item, amount } of recipe.ingredients) {
    lines.push(
      `        <BlacksmithRecipeIngredient${attributes([
        ['ItemClassId', item],
        ['Amount', amount],
      ])} />`,
    );
  }
  lines.push('      </Ingredients>', '      <Products>');
  for (const { item, threshold } of recipe.products) {
    lines.push(
      `        <BlacksmithRecipeProduct${attributes([
        ['ItemClassId', item],
        ['MinimalQuality', threshold],
      ])} />`,
    );
  }
  lines.push(
    '      </Products>',
    '    </BlacksmithRecipe>',
    '  </BlacksmithRecipes>',
    '</database>',
  );
  return lines.join('\n') + '\n';
}

function alchemyPatch(recipe) {
  // The texts the row names by key: the game reads none of this comment; the client gives the
  // game these texts.
  const texts = [[keyOf(recipe, 'name'), recipe.name]];
  if (recipe.usage !== undefined) {
    texts.push([keyOf(recipe, 'usage'), recipe.usage]);
  }
  for (const step of recipe.steps) {
    if (step.text !== undefined) {
      texts.push([keyOf(recipe, `step_${step.id}`), step.text]);
    }
  }
  const lines = [
    '<?xml version="1.0" encoding="utf-8"?>',
    '<!-- KCDS texts:',
    ...texts.map(
      ([key, value]) =>
        `  <KcdsText${attributes([
          ['StringName', key],
          ['Text', String(value).replaceAll('--', '- -')],
        ])} />`,
    ),
    '-->',
    '<database name="barbora">',
    '  <AlchemyRecipes version="1">',
    `    <AlchemyRecipe${attributes([
      ['RecipeId', recipe.id],
      ['BaseMaterial', recipe.base],
      ['UIUsage', recipe.usage === undefined ? undefined : keyOf(recipe, 'usage')],
      ['UIName', keyOf(recipe, 'name')],
      ['UIIcon', iconNameOf(recipe) ?? recipe.icon],
      ['BaseQualityRewardXP', recipe.baseXP],
    ])}>`,
    '      <Ingredients>',
  ];
  for (const { item, amount } of recipe.ingredients) {
    lines.push(
      `        <AlchemyIngredient${attributes([
        ['IngredientItemId', item],
        ['Quantity', amount],
      ])} />`,
    );
  }
  lines.push('      </Ingredients>', '      <Products>');
  for (const { item, threshold, maxYield } of recipe.products) {
    lines.push(
      `        <AlchemyProduct${attributes([
        ['Quality', threshold],
        ['ProductItemId', item],
        ['MaxYield', maxYield],
      ])} />`,
    );
  }
  lines.push('      </Products>', '      <Steps>');
  for (const step of recipe.steps) {
    lines.push(
      `        <AlchemyRecipeStep${attributes([
        ['StepId', step.id],
        ['StepType', step.type],
        ['FeedbackPriority', step.feedbackPriority || undefined],
        ['UIText', step.text === undefined ? undefined : keyOf(recipe, `step_${step.id}`)],
        ['Condition', step.condition],
      ])} />`,
    );
  }
  lines.push('      </Steps>', '    </AlchemyRecipe>', '  </AlchemyRecipes>', '</database>');
  return lines.join('\n') + '\n';
}

// The patch file of a registered recipe: its name (the game path is Libs/Tables/minigame/<file>)
// and its text.
function patchOf(recipe) {
  const suffix = `kcds_${String(recipe.id)
    .toLowerCase()
    .replace(/[^a-z0-9_.-]/g, '_')}.xml`;
  const table =
    recipe.kind === 'alchemy'
      ? { file: `AlchemyRecipe__${suffix}`, patch: alchemyPatch(recipe) }
      : { file: `BlacksmithRecipes__${suffix}`, patch: smithingPatch(recipe) };
  if (Buffer.byteLength(table.patch, 'utf8') > PATCH_MAX_BYTES) {
    fail('the recipe with its texts must be at most 40 KiB as a table row');
  }
  // Its own icon: a 64x64 .dds the clients mount as the game's item icon of that name.
  const iconName = iconNameOf(recipe);
  if (iconName) {
    table.iconName = iconName;
    table.icon = pictureDds(recipe.iconImage, ICON_SIZE, 'iconImage');
  }
  if (recipe.kind === 'alchemy') {
    table.pictures = bookPicturesOf(recipe);
  }
  return table;
}

// An alchemy recipe's pictures in the recipe book, each { which: 'product' | 'recipe', id, from,
// dds }: the game's own of RecipeId `from` (dds empty), or its own .dds (from 0); none without
// them.
function bookPicturesOf(recipe) {
  const id = recipe.id;
  if (recipe.bookPicturesFrom !== undefined) {
    return ['product', 'recipe'].map((which) => ({
      which,
      id,
      from: recipe.bookPicturesFrom,
      dds: Buffer.alloc(0),
    }));
  }
  const pictures = [];
  if (recipe.productImage !== undefined) {
    const dds = bookPictureDds(recipe.productImage, PRODUCT_SIZE, 'productImage');
    pictures.push({ which: 'product', id, from: 0, dds });
  }
  if (recipe.recipeImage !== undefined) {
    const dds = bookPictureDds(recipe.recipeImage, RECIPE_SIZE, 'recipeImage');
    pictures.push({ which: 'recipe', id, from: 0, dds });
  }
  return pictures;
}

// The recipe's row in the catalog's shape (recipes.cjs smithingRows, alchemyRows).
function rowOf(recipe) {
  if (recipe.kind === 'smithing') {
    return {
      id: recipe.id,
      kind: 'smithing',
      perks: recipe.perk ? [recipe.perk] : [],
      uiName: keyOf(recipe, 'name'),
      uiText: recipe.name,
      category: recipe.category,
      workpiece: recipe.workpiece,
      minSkill: recipe.minSkill,
      dlcId: recipe.dlcId,
      ingredients: recipe.ingredients.map(({ item, amount }) => [item, amount]),
      products: recipe.products.map(({ item, threshold }) => [item, threshold]),
    };
  }
  return {
    id: keyOf(recipe, 'name').slice(0, -'_name'.length),
    kind: 'alchemy',
    number: recipe.id,
    perks: [],
    uiName: keyOf(recipe, 'name'),
    uiText: recipe.name,
    base: recipe.base,
    xp: recipe.baseXP,
    dlcId: recipe.dlcId,
    ingredients: recipe.ingredients.map(({ item, amount }) => [item, amount]),
    products: recipe.products.map(({ item, threshold, maxYield }) => [item, threshold, maxYield]),
  };
}

module.exports = { normalize, patchOf, rowOf };
