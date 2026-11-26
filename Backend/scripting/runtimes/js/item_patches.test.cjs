'use strict';

// A game mod's items: a resource's item and CharacterComponent table patches join the server's
// item catalog (inventory/item_patches.cjs).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { ResourceHost } = require('./host.cjs');
const { Inventory } = require('./inventory/inventories.cjs');
const { itemClasses, lookupItemClass } = require('./inventory/catalog.cjs');
const { addItemTablePatches } = require('./inventory/item_patches.cjs');

const SLEEVES = '5f0c1a6e-0000-4000-8000-00000000a001';
const CLOAK = '5f0c1a6e-0000-4000-8000-00000000a002';
const BLADE = '5f0c1a6e-0000-4000-8000-00000000a003';
const RIBBON = '5f0c1a6e-0000-4000-8000-00000000a004';
const BREAD = '86e4ff24-88db-4024-abe6-46545fa0fbd1';

const ITEMS = `<?xml version="1.0" encoding="utf-8"?>
<database name="barbora">
  <ItemClasses version="2">
    <!-- a mod's sleeves, its cloak, a sword and the game's bread made dearer -->
    <Armor Id="${SLEEVES}" Name="mod_sleeves_m01" UIName="ui_nm_mod_sleeves" Clothing="ModSleeves01"
      MaxQuality="4" Price="120" Weight="3.5" />
    <Armor Id="${CLOAK}" Name="mod_cloak_m01" UIName="ui_nm_mod_cloak" Clothing="ModCloak01"
      Price="80" Weight="2" Model="manmade/mod/cloak_drop" />
    <MeleeWeapon Id="${BLADE}" Name="mod_blade" UIName="ui_nm_mod_blade" Class="4" MaxQuality="4"
      AttackModSlash="1.2" AttackModStab="0.9" AttackModSmash="0" Price="900" Weight="2.2" />
    <MiscItem Id="${RIBBON}" Name="mod_ribbon &amp; bow" UIName="ui_nm_mod_ribbon" Price="3"
      Weight="0.05" IsDivisible="true" />
    <Food Id="${BREAD}" Name="bread" UIName="ui_nm_bread" Price="99" Weight="0.3" />
    <NPCTool Id="5f0c1a6e-0000-4000-8000-00000000a005" Name="mod_npc_broom" />
    <!-- <MiscItem Id="5f0c1a6e-0000-4000-8000-00000000a006" Name="mod_commented_out" /> -->
  </ItemClasses>
</database>`;

// The sleeves derive from the game's brigandine sleeves; the cloak gives its own armour type.
const COMPONENTS = `<database name="barbora"><CharacterComponents version="6">
  <Component Name="Male"><DerivedComponents>
    <Clothing Name="ArmBrigandine"><DerivedComponents>
      <Clothing Name="ModSleeves01" DropModel="manmade/mod/sleeves_drop" />
    </DerivedComponents></Clothing>
    <Clothing Name="ModCloakBase" ArmorType="Coat"><DerivedComponents>
      <Clothing Name="ModCloak01" />
    </DerivedComponents></Clothing>
  </DerivedComponents></Component>
</CharacterComponents></database>`;

test("a mod's item rows join the catalog: slots from its clothing, weapons, stacks and a game row", () => {
  const before = itemClasses().length;
  const { added, replaced } = addItemTablePatches([ITEMS], [COMPONENTS]);
  assert.deepEqual(added, ['mod_sleeves_m01', 'mod_cloak_m01', 'mod_blade', 'mod_ribbon & bow']);
  assert.deepEqual(replaced, ['bread']);
  assert.equal(itemClasses().length, before + 4);

  const sleeves = lookupItemClass('mod_sleeves_m01');
  assert.equal(sleeves.id, SLEEVES);
  assert.equal(sleeves.slot, 'sleeves', "the game's brigandine sleeves' armour type");
  assert.equal(sleeves.model, 'objects/manmade/mod/sleeves_drop.cgf');
  assert.equal(sleeves.maximumQuality, 4);
  assert.equal(sleeves.qualityBands, true);
  assert.equal(sleeves.category, 'armor');

  const cloak = lookupItemClass(CLOAK.toUpperCase());
  assert.equal(cloak.slot, 'body_coat', 'the ArmorType of the clothing it is nested in');
  assert.equal(cloak.model, 'objects/manmade/mod/cloak_drop.cgf', 'its own Model first');
  assert.equal(cloak.qualityBands, false);

  const blade = lookupItemClass(BLADE);
  assert.equal(blade.weaponClass, 'longsword');
  assert.equal(blade.attackModSlash, 1.2);
  assert.equal(blade.slot, null);

  const ribbon = lookupItemClass(RIBBON);
  assert.equal(ribbon.name, 'mod_ribbon & bow');
  assert.equal(ribbon.stackable, true);

  assert.equal(lookupItemClass('bread').price, 99, "the patch's row takes the game's place");
  assert.equal(lookupItemClass('mod_npc_broom'), undefined, 'NPC tools stay out');
  assert.equal(lookupItemClass('mod_commented_out'), undefined, 'a row in a comment is no row');

  const inventory = new Inventory();
  const bladeAdded = inventory.add({ item: 'mod_blade', amount: 1 });
  assert.equal(bladeAdded.result.ok, true);
  assert.equal(inventory.snapshot().items[0].item, BLADE);
});

test("a resource's replace/ item patches join the catalog when it starts, with a log line", async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-item-patches-'));
  const logs = [];
  const host = new ResourceHost(root, { send: () => {}, log: (who, text) => logs.push(text) });
  const dir = path.join(root, 'tailor');
  fs.mkdirSync(path.join(dir, 'replace', 'libs', 'tables', 'item'), { recursive: true });
  fs.mkdirSync(path.join(dir, 'replace', 'Libs', 'Tables', 'Character'), { recursive: true });
  fs.writeFileSync(
    path.join(dir, 'package.json'),
    JSON.stringify({ name: 'tailor', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(dir, 'index.js'), '');
  const hood = '5f0c1a6e-0000-4000-8000-00000000b001';
  fs.writeFileSync(
    path.join(dir, 'replace', 'libs', 'tables', 'item', 'item__tailor.xml'),
    `<database><ItemClasses><Hood Id="${hood}" Name="tailor_hood" Clothing="TailorHood" /></ItemClasses></database>`,
  );
  fs.writeFileSync(
    path.join(dir, 'replace', 'Libs', 'Tables', 'Character', 'CharacterComponent__tailor.xml'),
    '<database><CharacterComponents><Clothing Name="TailorHood" ArmorType="Hood" /></CharacterComponents></database>',
  );
  await host.startAll();
  assert.equal(lookupItemClass('tailor_hood').slot, 'head_hood');
  assert.ok(
    logs.some((line) => line === 'items tailor_hood added from item__tailor.xml'),
    logs.join('\n'),
  );
});
