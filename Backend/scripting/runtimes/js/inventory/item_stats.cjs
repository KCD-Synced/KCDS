'use strict';

// The stats of the game's own items a server may override (item-config.json and
// Items.override): each by the item categories that carry it, its range, whether the game keeps
// it as a whole number, and its id on the wire (kcd2s/integration/item_overrides.hpp; the client
// knows where each one lives in an item class).

const GEAR = ['armor', 'helmet', 'hood', 'meleeWeapon', 'missileWeapon'];
const ARMOR = ['armor', 'helmet', 'hood'];
const MELEE = ['meleeWeapon'];
const WEAPONS = ['meleeWeapon', 'missileWeapon'];

function stat(name, id, categories, min, max, whole = false) {
  return Object.freeze({ name, id, categories: Object.freeze(categories), min, max, whole });
}

const STATS = Object.freeze([
  stat('visibility', 1, GEAR, -100, 100),
  stat('conspicuousness', 2, GEAR, -100, 100),
  stat('charisma', 3, GEAR, -1000, 1000),
  stat('defenseStab', 10, ARMOR, 0, 10000),
  stat('defenseSlash', 11, ARMOR, 0, 10000),
  stat('defenseSmash', 12, ARMOR, 0, 10000),
  stat('strengthRequired', 13, ARMOR, 0, 1000, true),
  stat('noise', 14, ARMOR, 0, 100),
  stat('durability', 15, ARMOR, 1, 1000000, true),
  stat('attack', 20, MELEE, 0, 10000),
  stat('power', 20, ['missileWeapon'], 0, 10000),
  stat('attackModStab', 21, MELEE, 0, 100),
  stat('attackModSlash', 22, MELEE, 0, 100),
  stat('attackModSmash', 23, MELEE, 0, 100),
  stat('defense', 24, WEAPONS, 0, 10000),
  stat('durability', 25, WEAPONS, 1, 1000000, true),
  stat('strengthRequired', 26, WEAPONS, 0, 1000, true),
  stat('agilityRequired', 27, WEAPONS, 0, 1000, true),
  stat('health', 30, ['food'], -10000, 10000),
  stat('refresh', 31, ['food'], -10000, 10000),
  stat('nutrition', 32, ['food'], -10000, 10000),
  stat('alcohol', 33, ['food'], 0, 1000),
  stat('decayTime', 34, ['food'], 0, 1000000),
  stat('shortTermNutrition', 35, ['food'], 0, 10),
  stat('attackStab', 40, ['ammo'], 0, 10000),
  stat('powerMod', 41, ['ammo'], 0, 100),
  stat('attackSlash', 42, ['ammo'], 0, 10000),
  stat('attackSmash', 43, ['ammo'], 0, 10000),
]);

// The stat of a name for a category, or undefined.
function statOf(name, category) {
  return STATS.find((entry) => entry.name === name && entry.categories.includes(category));
}

// The stat names a category carries.
function statNames(category) {
  return [
    ...new Set(STATS.filter((entry) => entry.categories.includes(category)).map((s) => s.name)),
  ];
}

module.exports = { statOf, statNames };
