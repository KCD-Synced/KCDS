'use strict';

// The game's item classes: what Inventory.classes answers and what every inventory
// operation is checked against. catalog.json is built from a game installation by
// Dev/tools/items/build_catalog.py.
//
// Quality lives in an item's health. The game splits health (0 to 1) into bands by the class's
// maximum quality M: quality q reaches up to q(q+1)/(M(M+1)) and down to (q-1)q/(M(M+1)) times
// 0.75, a quarter into the band below (WHGame 1.5.5; the
// quarter is the game's RPG parameter). Condition is where an item's health sits in its
// quality's band, 0 to 1. A class without qualities has one band, 0 to 1.

const catalog = require('./catalog.json');

const COLUMN = Object.fromEntries(catalog.columns.map((name, index) => [name, index]));
// The share of the band below that a quality reaches down into.
const BAND_OVERLAP = 0.25;

function classOf(row) {
  return Object.freeze({
    id: row[COLUMN.id],
    name: row[COLUMN.name],
    nameKey: row[COLUMN.nameKey],
    category: row[COLUMN.category],
    maximumQuality: row[COLUMN.maximumQuality],
    quest: row[COLUMN.quest],
    price: row[COLUMN.price],
    weight: row[COLUMN.weight],
    stackable: row[COLUMN.stackable],
    qualityBands: row[COLUMN.qualityBands],
    slot: row[COLUMN.slot],
    coversFeet: row[COLUMN.coversFeet],
    // The mesh it lies on the ground as, '' when the tables give none.
    model: row[COLUMN.model] ?? '',
    // A weapon's class and attack modifiers: null and 0 for anything else.
    weaponClass: row[COLUMN.weaponClass] ?? null,
    attackModSlash: row[COLUMN.attackModSlash] ?? 0,
    attackModStab: row[COLUMN.attackModStab] ?? 0,
    attackModSmash: row[COLUMN.attackModSmash] ?? 0,
  });
}

const CLASSES = catalog.classes.map(classOf);
const CLASS_BY_ID = new Map(CLASSES.map((entry) => [entry.id, entry]));
const CLASS_BY_NAME = new Map(CLASSES.map((entry) => [entry.name, entry]));
// Types resources registered (Items.register), in registration order.
const CUSTOM_CLASSES = [];

// A registered type joins the catalog under its GUID and its id.
function registerCustomClass(entry) {
  const known = CUSTOM_CLASSES.findIndex((custom) => custom.id === entry.id);
  if (known >= 0) {
    CUSTOM_CLASSES[known] = entry;
  } else {
    CUSTOM_CLASSES.push(entry);
  }
  CLASS_BY_ID.set(entry.id, entry);
  CLASS_BY_NAME.set(entry.name, entry);
}
// The categories a body can wear or hold (Inventory.set `equipped`).
const GEAR = new Set([
  'armor',
  'helmet',
  'hood',
  'meleeWeapon',
  'missileWeapon',
  'ammo',
  'quickSlotContainer',
]);

// The game's equipment slots, from Libs/Tables/item/equipment_slot.xml: where on the
// body a worn class goes. Slots with the same equipment part and body layer are one place on the
// body, which holds one piece. A slot may need another slot filled (requiresFilled) or empty
// (requiresEmpty) before a piece is worn in it.
const SLOT_COLUMN = Object.fromEntries(catalog.slotColumns.map((name, index) => [name, index]));
const SLOTS = new Map(
  catalog.slots.map((row) => [
    row[SLOT_COLUMN.name],
    Object.freeze({
      name: row[SLOT_COLUMN.name],
      place: `${row[SLOT_COLUMN.equipmentPart]}:${row[SLOT_COLUMN.bodyLayer]}`,
      requiresFilled: row[SLOT_COLUMN.requiresFilled],
      requiresEmpty: row[SLOT_COLUMN.requiresEmpty],
    }),
  ]),
);

// The slot of a slot name ({ name, place, requiresFilled, requiresEmpty }), or undefined.
function lookupSlot(name) {
  return SLOTS.get(name);
}

// The class of a GUID (any case) or of an exact table name, or undefined.
function lookupItemClass(query) {
  if (typeof query !== 'string') {
    return undefined;
  }
  return CLASS_BY_ID.get(query.toLowerCase()) ?? CLASS_BY_NAME.get(query);
}

// Inventory.classes: every class, as the API lists them.
function itemClasses() {
  return [...CLASSES, ...CUSTOM_CLASSES].map((entry) => ({
    id: entry.id,
    name: entry.name,
    nameKey: entry.nameKey,
    category: entry.category,
    maximumQuality: entry.maximumQuality,
    quest: entry.quest,
    slot: entry.slot ?? null,
    model: entry.model ?? '',
  }));
}

// The categories that are worn on the body: a class of them without a model of its own and
// without a drop model on its clothing lies on the ground as nothing (the DLC badges
// that only pin to a cap).
const WORN = new Set(['armor', 'helmet', 'hood']);

// Whether the game draws nothing for a stack of the class on the ground.
function invisibleOnGround(itemClass) {
  return WORN.has(itemClass.category) && !itemClass.model;
}

// The health range of one quality of a class: { low, high }.
function qualityBand(itemClass, quality) {
  if (!itemClass.qualityBands) {
    return { low: 0, high: 1 };
  }
  const scale = itemClass.maximumQuality * (itemClass.maximumQuality + 1);
  return {
    low: (((quality - 1) * quality) / scale) * (1 - BAND_OVERLAP),
    high: (quality * (quality + 1)) / scale,
  };
}

function isGear(itemClass) {
  return GEAR.has(itemClass.category);
}

// The categories an outfit is made of: what setOutfit takes off and puts on.
const CLOTHING = new Set(['armor', 'helmet', 'hood']);

function isClothing(itemClass) {
  return CLOTHING.has(itemClass.category);
}

// The keyring holds the keys a player has; their own game never lists it in the inventory screen,
// so nobody else sees or takes it either (a knocked-out body's container, a thief's loot screen).
function isKeyring(itemClass) {
  return itemClass.category === 'keyRing';
}

// The game's own outfits: { id, name, gender ('male', 'female' or 'any'), items }.
const OUTFIT_COLUMN = Object.fromEntries(catalog.outfitColumns.map((name, index) => [name, index]));
const OUTFITS = catalog.outfits.map((row) =>
  Object.freeze({
    id: row[OUTFIT_COLUMN.id],
    name: row[OUTFIT_COLUMN.name],
    gender: row[OUTFIT_COLUMN.gender],
    items: Object.freeze(row[OUTFIT_COLUMN.items].map((index) => CLASSES[index])),
  }),
);
const OUTFIT_BY_ID = new Map(OUTFITS.map((outfit) => [outfit.id, outfit]));
const OUTFIT_BY_NAME = new Map(OUTFITS.map((outfit) => [outfit.name, outfit]));

// Player.outfitSets: the outfits' names, those of one gender when it is given.
function outfitNames(gender) {
  return OUTFITS.filter(
    (outfit) => gender === undefined || outfit.gender === gender || outfit.gender === 'any',
  ).map((outfit) => outfit.name);
}

// The outfit of an exact name or of a GUID (any case), or undefined.
function lookupOutfit(query) {
  if (typeof query !== 'string') {
    return undefined;
  }
  return OUTFIT_BY_NAME.get(query) ?? OUTFIT_BY_ID.get(query.toLowerCase());
}

// What eating or drinking a food class does by itself: its kind ('potion' for the
// game's food type 1, else 'food') and its base effects (Consumption.baseEffects): the buff, and
// health, energy, nourishment and alcohol before condition, spoilage, perks and the soul.
const FOOD_COLUMN = Object.fromEntries(catalog.foodColumns.map((name, index) => [name, index]));
const POTION_TYPE = 1;
const FOODS = new Map(
  catalog.foods.map((row) => [
    CLASSES[row[FOOD_COLUMN.item]].id,
    Object.freeze({
      kind: row[FOOD_COLUMN.type] === POTION_TYPE ? 'potion' : 'food',
      baseEffects: Object.freeze({
        buff: row[FOOD_COLUMN.buff],
        health: row[FOOD_COLUMN.health],
        energy: row[FOOD_COLUMN.energy],
        nourishment: row[FOOD_COLUMN.nourishment],
        alcohol: row[FOOD_COLUMN.alcohol],
      }),
    }),
  ]),
);

// { kind, baseEffects } of a food class, or undefined for any other class.
function foodOf(itemClass) {
  return FOODS.get(itemClass.id);
}

// A class as scripts compare it (Player.equipment, rightHandItem, GroundItem.itemClass): 32
// lower-case hex digits, the GUID's first three fields reversed as one number, then its last eight
// bytes reversed (its 16 bytes read as two little-endian 64-bit numbers).
function classBytesHex(itemClass) {
  const hex = itemClass.id.replace(/-/g, '').toLowerCase();
  const tail = hex.slice(16).match(/../g).reverse().join('');
  return hex.slice(12, 16) + hex.slice(8, 12) + hex.slice(0, 8) + tail;
}

let classesByHex = null;

// The class of a key's 32 hex digits (as classBytesHex writes them), or undefined.
function lookupClassHex(hex) {
  if (!classesByHex) {
    classesByHex = new Map(itemClasses().map((itemClass) => [classBytesHex(itemClass), itemClass]));
  }
  return classesByHex.get(hex);
}

// Every class whose display name is this localization key: renaming one renames them all.
function classesNamed(nameKey) {
  return CLASSES.filter((entry) => entry.nameKey === nameKey);
}

module.exports = {
  invisibleOnGround,
  classBytesHex,
  classesNamed,
  foodOf,
  itemClasses,
  isClothing,
  isGear,
  isKeyring,
  lookupItemClass,
  lookupClassHex,
  lookupSlot,
  registerCustomClass,
  lookupOutfit,
  outfitNames,
  qualityBand,
};
