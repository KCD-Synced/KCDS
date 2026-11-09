'use strict';

// The game's loot of an animal's soul: what its inventory preset holds (Dev/tools/animals
// build_animal_loot.py writes animal_loot.json). Npc.lootOf lists it, and a server without its own
// harvest table (Npc.setHarvestTable) gives it as a carcass's yield.

const { lookupItemClass } = require('../inventory/catalog.cjs');

const ANIMAL_LOOT = require('./animal_loot.json');
const FOOD_CATEGORY = 'food';
// The game's trophies and special parts, which only a skilled hunter gets.
const HARVESTER_PREFIX = 'special_';

// The soul's loot rows, or null for a soul without a preset of the game's.
function animalLootOf(soul) {
  const entry = typeof soul === 'string' ? ANIMAL_LOOT[soul.toLowerCase()] : undefined;
  if (!entry) {
    return null;
  }
  return entry.items.map((row) => {
    const itemClass = lookupItemClass(row.name);
    return {
      item: itemClass ? itemClass.id : row.name,
      name: row.name,
      amount: row.amount,
      fraction: row.fraction,
      variation: row.variation,
      food: Boolean(itemClass) && itemClass.category === FOOD_CATEGORY,
      harvester: row.name.startsWith(HARVESTER_PREFIX),
    };
  });
}

// The game's own soul of each animal class, by the soul's name, for an animal made without a soul
// (Npc.create with only a class). The hen, the raven and the wild dog have no preset of the game's.
const CLASS_SOULS = Object.freeze({
  Boar: 'animal_boar',
  CattleBull: 'animal_cattle_bull',
  CattleCow: 'animal_cattle_cow',
  Dog: 'animal_dog',
  Hare: 'animal_hare',
  Pig: 'animal_pig',
  RedDeerDoe: 'animal_red_deer_doe',
  RedDeerStag: 'animal_red_deer_stag',
  RoeDeerBuck: 'animal_roe_deer_buck',
  RoeDeerHind: 'animal_roe_deer_hind',
  SheepEwe: 'animal_sheep_ewe',
  SheepRam: 'animal_sheep_ram',
  Wolf: 'animal_wolf_trosecko',
});

// The loot rows of an animal: its own soul's, or else its class's game soul's; null for none.
function animalLootOfBody(soul, actorClass) {
  const own = animalLootOf(soul);
  if (own) {
    return own;
  }
  const name = CLASS_SOULS[actorClass];
  const guid = name
    ? Object.keys(ANIMAL_LOOT).find((key) => ANIMAL_LOOT[key].soul === name)
    : undefined;
  return guid ? animalLootOf(guid) : null;
}

module.exports = { animalLootOf, animalLootOfBody };
