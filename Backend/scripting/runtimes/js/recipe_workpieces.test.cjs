'use strict';

// Crafting.defineWorkpiece: a resource's smithing workpiece, checked, written as a patch of the
// game's BlacksmithWorkpieces table and sent to every client before the recipes that name it.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');

const IRON = '94d8f5f1-20a7-4840-98ad-1d198d381389';
const HORSESHOE = '549ab26e-df73-43d6-ac9b-f4f74afec67f';

function hostWith(code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-workpieces-'));
  const output = [];
  const host = new ResourceHost(root, { send: (e) => output.push(e), log: () => {} });
  const dir = path.join(root, 'armourer');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'armourer', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), code);
  return { host, output };
}

const PLATE = {
  id: 'kit_plate',
  like: 'axe',
  model: 'Objects\\Manmade\\Task_Specific_Props\\Metal_Industry\\Smithing\\Semifinished_Helm.cgf',
  zones: 2,
  mapSize: [0.25, 0.25],
};

const PLATE_RECIPE = {
  kind: 'smithing',
  id: 'kit_plate_shoe',
  name: 'Plate shoe',
  icon: 'horseshoeFarmer',
  category: 'Other',
  workpiece: 'kit_plate',
  ingredients: [{ item: IRON, amount: 1 }],
  products: [{ item: HORSESHOE, threshold: 0 }],
};

test("defineWorkpiece writes a row of the game's workpiece table, from a game one", async () => {
  const { host, output } = hostWith(`
    kcds.world.Crafting.defineWorkpiece(${JSON.stringify(PLATE)});
    kcds.world.Crafting.defineWorkpiece(${JSON.stringify(PLATE)}); // the same again: nothing
    kcds.world.Crafting.defineRecipe(${JSON.stringify(PLATE_RECIPE)});
  `);
  await host.startAll();
  const tables = output.filter((e) => e.op === 'recipeTable' && e.to === undefined);
  assert.deepEqual(
    tables.map((e) => e.file),
    ['BlacksmithWorkpieces__kcds_kit_plate.xml', 'BlacksmithRecipes__kcds_kit_plate_shoe.xml'],
  );
  const row = /<BlacksmithWorkpiece ([^>]*)\/>/.exec(tables[0].patch)[1];
  // The axe's row, with the definition's columns over it.
  assert.match(row, /^Id="kit_plate" /);
  assert.match(
    row,
    /Model="objects\/manmade\/task_specific_props\/metal_industry\/smithing\/semifinished_helm.cgf"/,
  );
  assert.match(row, /UseTongs="true" RaycastAttachment="Axe" AudioSwitch="axe"/);
  assert.match(row, /MapSize="0.25,0.25" MapOffset="0,-0.15" MapRotation="0,1.57,1.57"/);
  assert.match(row, /Zones="2"/);
  assert.match(tables[1].patch, /WorkpieceId="kit_plate"/);

  // A player whose world becomes ready gets the workpiece before the recipe.
  host.updatePlayer({
    id: 6,
    ready: true,
    position: { x: 0, y: 0, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
  });
  const sent = output.filter((e) => e.op === 'recipeTable' && e.to === 6).map((e) => e.file);
  assert.ok(
    sent.indexOf('BlacksmithWorkpieces__kcds_kit_plate.xml') <
      sent.indexOf('BlacksmithRecipes__kcds_kit_plate_shoe.xml'),
  );
});

test('defineWorkpiece refuses malformed definitions and ids in use', async () => {
  const { host } = hostWith('');
  await host.startAll();
  const { Crafting } = host.makeAPI();
  const refused = [
    [{ id: 'sword', like: 'axe' }, /the game has a workpiece sword/],
    [{ id: 'kit bad', like: 'axe' }, /id must be/],
    [{ id: 'kit_no_model' }, /needs a model/],
    [{ id: 'kit_like', like: 'hammer' }, /like must be one of/],
    [{ id: 'kit_model', model: 'models/x.cdf' }, /model path/],
    [{ id: 'kit_model2', model: 'objects/x.dds' }, /model path/],
    [{ id: 'kit_zones', like: 'sword', zones: 0 }, /zones must be/],
    [{ id: 'kit_size', like: 'sword', mapSize: [1] }, /mapSize must be 2 numbers/],
    [{ id: 'kit_turn', like: 'axe', mapRotation: [0, 1] }, /mapRotation must be 3 numbers/],
    [{ id: 'kit_far', like: 'axe', mapOffset: [0, 50] }, /mapOffset must be 2 numbers/],
    [{ id: 'kit_tongs', like: 'axe', useTongs: 'yes' }, /useTongs must be true or false/],
    [{ id: 'kit_audio', like: 'axe', audioSwitch: 'bell' }, /audioSwitch must be one of/],
    [{ id: 'kit_ray', like: 'axe', raycastAttachment: 'a b' }, /raycastAttachment must be/],
  ];
  for (const [definition, pattern] of refused) {
    assert.throws(() => Crafting.defineWorkpiece(definition), pattern, JSON.stringify(definition));
  }
  Crafting.defineWorkpiece({ id: 'kit_once', like: 'horseshoe' });
  assert.throws(
    () => Crafting.defineWorkpiece({ id: 'kit_once', like: 'axe' }),
    /already registered with another definition/,
  );
  // A recipe names only a workpiece the game or a resource has.
  assert.throws(
    () => Crafting.defineRecipe({ ...PLATE_RECIPE, id: 'kit_nowhere', workpiece: 'kit_nowhere' }),
    /workpiece must be one of/,
  );
});
