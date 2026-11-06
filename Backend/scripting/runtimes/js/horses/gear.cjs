'use strict';

// A horse's tack: what it wears in its four slots, from the game's own tables. The
// item catalog (inventory/catalog.json, Dev/tools/items/build_catalog.py) gives every horse item
// its slot (horse_saddle, horse_torso, horse_head, horse_shoe), and the game's clothing presets
// named horse_* are its sets of tack. A caparison or harness (torso) lies on the saddle, so it
// needs one.
//
// Gear is { saddle, head, torso, shoe }, item class names or null for an empty slot. It goes to
// every client as the horse's `gear` field (the outfit field form "KO2", per piece its class bytes,
// its quality and its health), and every body of the horse wears it.

const {
  classBytesHex,
  itemClasses,
  lookupItemClass,
  lookupOutfit,
  outfitNames,
  qualityBand,
} = require('../inventory/catalog.cjs');

// Script slot -> the game's equipment slot, saddles first.
const SLOTS = Object.freeze({
  saddle: 'horse_saddle',
  head: 'horse_head',
  torso: 'horse_torso',
  shoe: 'horse_shoe',
});
const SLOT_OF_GAME = new Map(Object.entries(SLOTS).map(([slot, game]) => [game, slot]));
// The tack a horse wears when a script names none: the game's two generic sets.
const DEFAULT_PRESETS = Object.freeze(['horse_common01', 'horse_common02']);
const PRESET_PREFIX = 'horse_';

function bare() {
  return { saddle: null, head: null, torso: null, shoe: null };
}

// The script slot of a class, or null for a class that is no horse gear.
function slotOf(itemClass) {
  return itemClass ? SLOT_OF_GAME.get(itemClass.slot) ?? null : null;
}

// The script slot of a class by its name or GUID, or null.
function slotOfItem(item) {
  return slotOf(lookupItemClass(item));
}

// Every horse gear class name, of one slot or of all of them (saddles first).
function gearList(slot) {
  if (slot !== undefined && !Object.hasOwn(SLOTS, slot)) {
    throw new TypeError("Horse.gearList takes 'saddle', 'head', 'torso', 'shoe' or nothing");
  }
  const wanted = slot === undefined ? Object.keys(SLOTS) : [slot];
  const found = [];
  for (const name of wanted) {
    for (const entry of itemClasses()) {
      if (entry.slot === SLOTS[name]) {
        found.push(entry.name);
      }
    }
  }
  return found;
}

// The tack of a preset by slot, or null when it is no set of horse gear (an empty preset, or one
// with a piece no horse wears).
function presetGear(name) {
  const outfit = lookupOutfit(name);
  if (!outfit || outfit.items.length === 0) {
    return null;
  }
  const gear = bare();
  for (const item of outfit.items) {
    const slot = slotOf(item);
    if (slot === null || gear[slot] !== null) {
      return null;
    }
    gear[slot] = item.name;
  }
  return gear;
}

// The game's sets of horse tack, alphabetical.
function gearSets() {
  return outfitNames()
    .filter((name) => name.startsWith(PRESET_PREFIX) && presetGear(name) !== null)
    .sort();
}

// One class as gear of a slot; throws for a class that is no horse gear or one keyed under
// another slot.
function pieceOf(value, slot, what) {
  const itemClass = lookupItemClass(value);
  const own = slotOf(itemClass);
  if (own === null) {
    throw new TypeError(`${what}: ${String(value)} is no horse gear`);
  }
  if (slot !== undefined && own !== slot) {
    throw new TypeError(`${what}: ${itemClass.name} is ${own} gear, not ${slot}`);
  }
  return itemClass.name;
}

// What setTack takes as gear by slot: a preset name, or an object of slots (a missing key or
// null leaves the slot empty). Throws for anything a horse cannot wear.
function gearOf(request, what = 'Horse.setTack') {
  let gear;
  if (typeof request === 'string') {
    gear = presetGear(request);
    if (gear === null) {
      throw new TypeError(`${what}: ${request} is no horse gear preset`);
    }
  } else if (request !== null && typeof request === 'object' && !Array.isArray(request)) {
    gear = bare();
    for (const [slot, value] of Object.entries(request)) {
      if (!Object.hasOwn(SLOTS, slot)) {
        throw new TypeError(`${what}: unknown slot ${slot}`);
      }
      gear[slot] = value === null || value === undefined ? null : pieceOf(value, slot, what);
    }
  } else {
    throw new TypeError(`${what} takes a preset name or { saddle, head, torso, shoe }`);
  }
  checkTorso(gear, what);
  return gear;
}

function checkTorso(gear, what) {
  if (gear.torso !== null && gear.saddle === null) {
    throw new RangeError(`${what}: a caparison lies on a saddle, and there is none`);
  }
}

// The tack a horse wears when spawned without any: one of the game's generic sets.
function defaultGear(random = Math.random) {
  const name = DEFAULT_PRESETS[Math.floor(random() * DEFAULT_PRESETS.length)];
  return presetGear(name) ?? bare();
}

// The pieces as the native server takes them (the outfit field: class bytes, quality, health).
function piecesOf(gear) {
  const pieces = [];
  for (const slot of Object.keys(SLOTS)) {
    if (gear[slot] !== null) {
      // As the game's presets dress a body: the lowest quality, whole.
      const itemClass = lookupItemClass(gear[slot]);
      pieces.push({
        itemClass: classBytesHex(itemClass),
        quality: 1,
        health: qualityBand(itemClass, 1).high,
      });
    }
  }
  return pieces;
}

module.exports = {
  SLOTS,
  bare,
  checkTorso,
  defaultGear,
  gearList,
  gearOf,
  gearSets,
  pieceOf,
  piecesOf,
  presetGear,
  slotOf,
  slotOfItem,
};
