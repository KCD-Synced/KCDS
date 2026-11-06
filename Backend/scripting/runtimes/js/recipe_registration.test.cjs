'use strict';

// Crafting.defineRecipe: a resource's alchemy and smithing recipes, checked, put in
// the catalog as the script's and sent to every client as a patch of the game's table.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const { ResourceHost } = require('./host.cjs');
const { decodePng } = require('./recipes/picture.cjs');

const SALVIA = 'b9de1d84-a0c1-4b81-9f60-8d7fbb3cb9d4';
const NETTLE = '9b771c16-c0b1-438c-aeda-8c4d3ce28465';
const HAIR_O_DOG = '7a1e8447-4449-473b-aac9-63f7a324fa0b';
const IRON = '94d8f5f1-20a7-4840-98ad-1d198d381389';
const HORSESHOE = '549ab26e-df73-43d6-ac9b-f4f74afec67f';

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-recipes-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'herbalist');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'herbalist', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  return { host, output };
}

const TONIC = {
  kind: 'alchemy',
  id: 910001,
  name: 'Sage & nettle tonic',
  icon: 'potion_hair_o_dog',
  base: 'Water',
  usage: 'Drink it -- slowly.',
  baseXP: 12,
  ingredients: [
    { item: SALVIA, amount: 2 },
    { item: NETTLE, amount: 1 },
  ],
  products: [{ item: HAIR_O_DOG, threshold: 1, maxYield: 2 }],
  steps: [
    {
      id: 1,
      type: 'Trivial',
      condition: `PotionBase() AND IngredientCount('${SALVIA}') >= 1`,
    },
    {
      id: 2,
      type: 'Easy',
      condition: `IsMilled('${SALVIA}', 1) AND WeakBoilingTime('${NETTLE}', 3)`,
      feedbackPriority: 2,
      text: 'Mill the sage.',
    },
  ],
};

const SHOE = {
  kind: 'smithing',
  id: 'kit_shoe_registered',
  name: 'Kit <shoe>',
  icon: 'horseshoeFarmer',
  category: 'Other',
  workpiece: 'horseshoe',
  minSkill: 2,
  description: 'A shoe a script added.',
  ingredients: [{ item: IRON, amount: 2 }],
  products: [{ item: HORSESHOE, threshold: 40 }],
};

test('defineRecipe puts a recipe in the catalog and sends its table patch', async () => {
  const { host, output } = hostWith(`
    kcds.world.Crafting.defineRecipe(${JSON.stringify(TONIC)});
    kcds.world.Crafting.defineRecipe(${JSON.stringify(SHOE)});
    kcds.world.Crafting.defineRecipe(${JSON.stringify(SHOE)}); // the same again: nothing
  `);
  await host.startAll();
  const { Crafting } = host.makeAPI();
  const player = host.updatePlayer({
    id: 5,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const tables = output.filter((e) => e.op === 'recipeTable' && e.to === undefined);
  assert.deepEqual(
    tables.map((e) => e.file),
    ['AlchemyRecipe__kcds_910001.xml', 'BlacksmithRecipes__kcds_kit_shoe_registered.xml'],
  );

  const alchemy = tables[0].patch;
  assert.match(alchemy, /<AlchemyRecipe RecipeId="910001" BaseMaterial="Water"/);
  assert.match(alchemy, /UIName="kcds_recipe_alchemy_910001_name"/);
  assert.match(alchemy, /UIIcon="potion_hair_o_dog" BaseQualityRewardXP="12">/);
  assert.match(alchemy, /<AlchemyIngredient IngredientItemId="b9de1d84[^"]*" Quantity="2" \/>/);
  assert.match(alchemy, /<AlchemyProduct Quality="1" ProductItemId="7a1e8447[^"]*" MaxYield="2"/);
  assert.match(
    alchemy,
    /<AlchemyRecipeStep StepId="2" StepType="Easy" FeedbackPriority="2" UIText="kcds_recipe_alchemy_910001_step_2" Condition="IsMilled\(&apos;b9de1d84/,
  );
  // The texts ride in a comment the game skips; a '--' in them would end it.
  assert.match(
    alchemy,
    /<KcdsText StringName="kcds_recipe_alchemy_910001_name" Text="Sage &amp; nettle tonic" \/>/,
  );
  assert.match(alchemy, /Text="Drink it - - slowly."/);
  assert.equal(alchemy.indexOf('<!--') < alchemy.indexOf('<database'), true);

  const smithing = tables[1].patch;
  assert.match(
    smithing,
    /<BlacksmithRecipe Id="kit_shoe_registered" UIIcon="horseshoeFarmer" Category="Other" MinSkillLevel="2" WorkpieceId="horseshoe">/,
  );
  assert.match(
    smithing,
    /<UIName StringName="kcds_recipe_smithing_kit_shoe_registered_name" Text="Kit &lt;shoe&gt;" \/>/,
  );
  assert.match(
    smithing,
    /<BlacksmithRecipeProduct ItemClassId="549ab26e[^"]*" MinimalQuality="40" \/>/,
  );

  const listed = Crafting.recipes(player);
  const tonic = listed.find((recipe) => recipe.key.kind === 'alchemy' && recipe.key.id === 910001);
  assert.equal(tonic.source, 'script');
  assert.equal(tonic.base, 'Water');
  assert.deepEqual(JSON.parse(JSON.stringify(tonic.products)), [
    { item: HAIR_O_DOG, threshold: 1, maxYield: 2 },
  ]);
  const shoe = listed.find((recipe) => recipe.key.id === 'kit_shoe_registered');
  assert.equal(shoe.source, 'script');
  assert.equal(shoe.minSkill, 2);
  assert.equal(listed.find((recipe) => recipe.key.id === 'r_horseshoe').source, 'game');

  // A player whose world becomes ready is sent every registered recipe.
  assert.deepEqual(
    output.filter((e) => e.op === 'recipeTable' && e.to !== undefined).map((e) => [e.file, e.to]),
    [
      ['AlchemyRecipe__kcds_910001.xml', 5],
      ['BlacksmithRecipes__kcds_kit_shoe_registered.xml', 5],
    ],
  );
});

test('defineRecipe refuses malformed definitions and ids in use', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const { Crafting } = host.makeAPI();
  const refuses = (change, pattern) =>
    assert.throws(() => Crafting.defineRecipe({ ...SHOE, id: 'kit_refused', ...change }), pattern);
  refuses({ kind: 'cooking' }, /kind must be one of/);
  refuses({ id: 'has space' }, /id must be 1 to 128/);
  refuses({ id: 'r_horseshoe' }, /the game or a resource uses smithing recipe r_horseshoe/);
  refuses({ name: '' }, /name must be a text/);
  refuses({ category: 'Bow' }, /category must be one of/);
  refuses({ workpiece: 'bow' }, /workpiece must be one of/);
  refuses({ minSkill: 31 }, /minSkill must be/);
  refuses({ perk: 'not-a-guid' }, /perk must be a perk GUID/);
  refuses({ ingredients: [] }, /ingredients must list 1 to 64/);
  refuses(
    { ingredients: [{ item: 'ironBad', amount: 1 }] },
    /is not an item of the game's catalog/,
  );
  refuses(
    {
      ingredients: [
        { item: IRON, amount: 1 },
        { item: IRON, amount: 2 },
      ],
    },
    /is listed twice/,
  );
  refuses({ products: [{ item: HORSESHOE, threshold: 101 }] }, /quality percent from 0 to 100/);
  refuses({ iconImage: 'bm90IGEgcG5n' }, /iconImage must be a base64 PNG/);

  const tonic = (change) => ({ ...TONIC, id: 910099, ...change });
  assert.throws(() => Crafting.defineRecipe(tonic({ id: 1 })), /uses alchemy recipe 1/);
  assert.throws(() => Crafting.defineRecipe(tonic({ id: 0 })), /RecipeId from 1/);
  assert.throws(() => Crafting.defineRecipe(tonic({ base: undefined })), /base must be one of/);
  assert.throws(() => Crafting.defineRecipe(tonic({ steps: [] })), /steps must list 1 to 28/);
  assert.throws(
    () => Crafting.defineRecipe(tonic({ steps: [{ id: 1, type: 'Easy', condition: 'Drink()' }] })),
    /is not of the game's condition syntax/,
  );
  assert.throws(
    () =>
      Crafting.defineRecipe(
        tonic({
          steps: [
            { id: 1, type: 'Easy', condition: 'PotionBase()' },
            { id: 1, type: 'Easy', condition: 'PotionBase()' },
          ],
        }),
      ),
    /distinct number/,
  );
  assert.throws(
    () => Crafting.defineRecipe(tonic({ products: [{ item: HAIR_O_DOG, threshold: 5 }] })),
    /grade from 1 to 4/,
  );
  assert.throws(
    () => Crafting.defineRecipe(tonic({ bookPicturesFrom: 999999 })),
    /bookPicturesFrom/,
  );
  // 28 steps of 2048 characters do not fit one message to the clients.
  const longSteps = Array.from({ length: 28 }, (_, index) => ({
    id: index + 1,
    type: 'Trivial',
    condition: 'PotionBase()',
    text: 'x'.repeat(2048),
  }));
  assert.throws(
    () => Crafting.defineRecipe(tonic({ steps: longSteps })),
    /at most 40 KiB as a table row/,
  );

  // A different definition under a registered id.
  Crafting.defineRecipe({ ...SHOE, id: 'kit_once' });
  assert.throws(
    () => Crafting.defineRecipe({ ...SHOE, id: 'kit_once', minSkill: 3 }),
    /already registered with another definition/,
  );
});

// A PNG of `width` x `height` RGBA pixels (each [r, g, b, a]), filter 0 on every line.
function png(width, height, pixel) {
  const crcTable = Array.from({ length: 256 }, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    return c >>> 0;
  });
  const crc = (bytes) => {
    let c = 0xffffffff;
    for (const byte of bytes) {
      c = crcTable[(c ^ byte) & 0xff] ^ (c >>> 8);
    }
    return (c ^ 0xffffffff) >>> 0;
  };
  const chunk = (type, body) => {
    const head = Buffer.alloc(8);
    head.writeUInt32BE(body.length, 0);
    head.write(type, 4, 'latin1');
    const tail = Buffer.alloc(4);
    tail.writeUInt32BE(crc(Buffer.concat([head.subarray(4), body])), 0);
    return Buffer.concat([head, body, tail]);
  };
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0);
  header.writeUInt32BE(height, 4);
  header[8] = 8;
  header[9] = 6;
  const lines = [];
  for (let y = 0; y < height; y++) {
    lines.push(Buffer.from([0]));
    for (let x = 0; x < width; x++) {
      lines.push(Buffer.from(pixel(x, y)));
    }
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', header),
    chunk('IDAT', zlib.deflateSync(Buffer.concat(lines))),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

test("a recipe's own icon is fitted onto the item icon square and sent with its patch", async () => {
  // 128 x 64, red on the left half, blue on the right.
  const picture = png(128, 64, (x) => (x < 64 ? [255, 0, 0, 255] : [0, 0, 255, 255]));
  const decoded = decodePng(picture, 'iconImage');
  assert.deepEqual([decoded.width, decoded.height], [128, 64]);
  const definition = { ...SHOE, id: 'kit_icon_shoe', iconImage: picture.toString('base64') };
  const { host, output } = hostWith(
    `kcds.world.Crafting.defineRecipe(${JSON.stringify(definition)});`,
  );
  await host.startAll();
  const [table] = output.filter((e) => e.op === 'recipeTable' && e.file.includes('kit_icon_shoe'));
  assert.equal(table.iconName, 'kcds_recipe_smithing_kit_icon_shoe_icon');
  assert.match(table.patch, /UIIcon="kcds_recipe_smithing_kit_icon_shoe_icon"/);
  // A 64 x 64 B8G8R8A8 .dds: the picture fitted at 64 x 32, centred, transparent above and below.
  assert.equal(table.icon.length, 128 + 64 * 64 * 4);
  assert.equal(table.icon.toString('latin1', 0, 4), 'DDS ');
  const at = (x, y) => [...table.icon.subarray(128 + (y * 64 + x) * 4, 128 + (y * 64 + x) * 4 + 4)];
  assert.deepEqual(at(10, 0), [0, 0, 0, 0]);
  assert.deepEqual(at(10, 32), [0, 0, 255, 255]); // red as B, G, R, A
  assert.deepEqual(at(50, 32), [255, 0, 0, 255]); // blue
  assert.throws(
    () => decodePng(Buffer.from('not a png'), 'iconImage'),
    /iconImage must be a base64 PNG/,
  );
});

// The colour of one pixel of a DXT5 .dds, as RGBA.
function dxt5Pixel(dds, size, x, y) {
  const block = 128 + ((Math.floor(y / 4) * size) / 4 + Math.floor(x / 4)) * 16;
  const index = (y % 4) * 4 + (x % 4);
  const a0 = dds[block];
  const a1 = dds[block + 1];
  const alphaBits = dds.readUIntLE(block + 2 + (index < 8 ? 0 : 3), 3);
  const alphaIndex = (alphaBits >> ((index % 8) * 3)) & 7;
  const alphas = [a0, a1];
  for (let step = 1; step <= 6; step++) {
    alphas.push(Math.round(((7 - step) * a0 + step * a1) / 7));
  }
  const colour = (value) => [
    (((value >> 11) & 31) * 255) / 31,
    (((value >> 5) & 63) * 255) / 63,
    ((value & 31) * 255) / 31,
  ];
  const p0 = colour(dds.readUInt16LE(block + 8));
  const p1 = colour(dds.readUInt16LE(block + 10));
  const palette = [
    p0,
    p1,
    p0.map((value, channel) => (2 * value + p1[channel]) / 3),
    p0.map((value, channel) => (value + 2 * p1[channel]) / 3),
  ];
  const colourIndex = (dds.readUInt32LE(block + 12) >>> (index * 2)) & 3;
  return [...palette[colourIndex].map(Math.round), alphas[alphaIndex] ?? a0];
}

test("an alchemy recipe's book pictures travel beside its patch, as DXT5", async () => {
  // 64 x 32: green on the left half, red on the right.
  const picture = png(64, 32, (x) => (x < 32 ? [0, 255, 0, 255] : [255, 0, 0, 255]));
  const own = {
    ...TONIC,
    id: 910010,
    productImage: picture.toString('base64'),
    recipeImage: picture.toString('base64'),
  };
  const borrowed = { ...TONIC, id: 910011, bookPicturesFrom: 1 };
  const { host, output } = hostWith(`
    kcds.world.Crafting.defineRecipe(${JSON.stringify(own)});
    kcds.world.Crafting.defineRecipe(${JSON.stringify(borrowed)});
  `);
  await host.startAll();
  const tables = output.filter((e) => e.op === 'recipeTable');
  assert.ok(tables.some((e) => e.file === 'AlchemyRecipe__kcds_910010.xml'));
  assert.ok(tables.every((e) => e.pictures === undefined));
  // Each picture is an op of its own, after its recipe's table.
  const pictures = (id) => output.filter((e) => e.op === 'recipePicture' && e.id === id);
  const [product, recipe] = pictures(910010);
  assert.ok(output.indexOf(product) > output.findIndex((e) => e.file?.includes('910010')));
  assert.deepEqual(
    [product.which, product.id, recipe.which, recipe.id],
    ['product', 910010, 'recipe', 910010],
  );
  assert.equal(product.dds.length, 128 + 256 * 256);
  assert.equal(recipe.dds.length, 128 + 1024 * 1024);
  assert.equal(product.dds.toString('latin1', 84, 88), 'DXT5');
  // Fitted at 256 x 128, centred: transparent above, green and red beside each other.
  assert.equal(dxt5Pixel(product.dds, 256, 100, 10)[3], 0);
  assert.deepEqual(dxt5Pixel(product.dds, 256, 60, 128), [0, 255, 0, 255]);
  assert.deepEqual(dxt5Pixel(product.dds, 256, 200, 128), [255, 0, 0, 255]);
  assert.deepEqual(dxt5Pixel(recipe.dds, 1024, 800, 512), [255, 0, 0, 255]);
  // The game's own two pictures of recipe 1, no .dds.
  assert.deepEqual(
    pictures(910011).map(({ which, from, dds }) => [which, from, dds.length]),
    [
      ['product', 1, 0],
      ['recipe', 1, 0],
    ],
  );
  // A player whose world becomes ready gets each table, then its pictures.
  host.updatePlayer({
    id: 6,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  // (Recipes of the tests before are registered in this process too.)
  const ours = (e) => e.id === 910010 || e.id === 910011 || /_91001[01]\.xml$/.test(e.file ?? '');
  const arrival = output.filter((e) => e.to === 6 && e.op.startsWith('recipe') && ours(e));
  assert.deepEqual(
    arrival.map((e) => [e.op, e.file ?? `${e.which} ${e.id}`]),
    [
      ['recipeTable', 'AlchemyRecipe__kcds_910010.xml'],
      ['recipePicture', 'product 910010'],
      ['recipePicture', 'recipe 910010'],
      ['recipeTable', 'AlchemyRecipe__kcds_910011.xml'],
      ['recipePicture', 'product 910011'],
      ['recipePicture', 'recipe 910011'],
    ],
  );
  // A smithing recipe has no book.
  const { host: smith, output: smithOutput } = hostWith(
    `kcds.world.Crafting.defineRecipe(${JSON.stringify({ ...SHOE, id: 'kit_bookless_shoe' })});`,
  );
  await smith.startAll();
  assert.ok(smithOutput.some((e) => e.op === 'recipeTable'));
  assert.ok(!smithOutput.some((e) => e.op === 'recipePicture'));
});
