'use strict';

// How fast items wear: the game works out the wear of a blow, a block or a fall, and
// a server scales it. A player's game reports an item's condition dropping (reason `wear`); the
// server takes the drop times the item's rate and gives the item that condition, in the player's
// game too. A repair is never scaled.
//
// Inventory.setWearRates({ default, <category>, <item> }): a rate per item (its table name or
// GUID), else per category (meleeWeapon, missileWeapon, armor, helmet, hood, ammo, ...), else the
// default; 1 is the game's own wear, 0 none, 2 twice as fast. Keys left out keep the game's rate.

const { itemClasses, lookupItemClass } = require('./catalog.cjs');

const MAX_RATE = 100;
const MAX_KEYS = 4096;

function categories() {
  return new Set(itemClasses().map((itemClass) => itemClass.category));
}

class WearRates {
  constructor() {
    this.fallback = 1;
    // category -> rate
    this.byCategory = new Map();
    // class GUID -> rate
    this.byItem = new Map();
  }

  // Replaces the rates; throws on an unknown key or a rate out of range. The rates now in use.
  set(table) {
    if (table === null || typeof table !== 'object' || Array.isArray(table)) {
      throw new TypeError('Inventory.setWearRates takes an object of rates');
    }
    const entries = Object.entries(table);
    if (entries.length > MAX_KEYS) {
      throw new RangeError('Inventory.setWearRates: too many rates');
    }
    const known = categories();
    let fallback = 1;
    const byCategory = new Map();
    const byItem = new Map();
    for (const [key, rate] of entries) {
      if (typeof rate !== 'number' || !Number.isFinite(rate) || rate < 0 || rate > MAX_RATE) {
        throw new RangeError(`Inventory.setWearRates: ${key} must be a number from 0 to 100`);
      }
      if (key === 'default') {
        fallback = rate;
      } else if (known.has(key)) {
        byCategory.set(key, rate);
      } else {
        const itemClass = lookupItemClass(key);
        if (!itemClass) {
          throw new TypeError(`Inventory.setWearRates: no item or category ${key}`);
        }
        byItem.set(itemClass.id, rate);
      }
    }
    this.fallback = fallback;
    this.byCategory = byCategory;
    this.byItem = byItem;
    return this.get();
  }

  // The rates in use: { default, <category>..., <item name>... }.
  get() {
    const table = { default: this.fallback };
    for (const [category, rate] of this.byCategory) {
      table[category] = rate;
    }
    for (const [id, rate] of this.byItem) {
      table[lookupItemClass(id).name] = rate;
    }
    return table;
  }

  // The rate of a class.
  of(itemClass) {
    if (this.byItem.has(itemClass.id)) {
      return this.byItem.get(itemClass.id);
    }
    if (this.byCategory.has(itemClass.category)) {
      return this.byCategory.get(itemClass.category);
    }
    return this.fallback;
  }
}

module.exports = { WearRates };
