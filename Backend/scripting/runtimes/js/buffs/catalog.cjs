'use strict';

// The game's status effects, the same on the server and in every client's resource
// runtime: what Buffs.find, classes and tags answer, and what BuffInfo and BuffState carry.
// catalog.json is built from a game installation by Dev/tools/buffs/build_catalog.py.

const catalog = require('./catalog.json');

const BUFFS = catalog.buffs;
const BUFF_BY_NAME = new Map(BUFFS.map((buff) => [buff.name, buff]));
const BUFF_BY_ID = new Map(BUFFS.map((buff) => [buff.id.toLowerCase(), buff]));
const CLASS_BY_NAME = new Map(catalog.classes.map((entry) => [entry.name, entry]));
// The classes a server can claim, in the game's order.
const CLAIMABLE = catalog.classes.filter((entry) => entry.claimable).map((entry) => entry.name);
const TAGS = catalog.tags;
const TAG_BY_NAME = new Map(TAGS.map((tag) => [tag.name, tag]));

function buffInfo(buff) {
  return {
    name: buff.name,
    class: buff.class,
    aiTag: buff.aiTag,
    duration: buff.duration,
    custom: false,
    displayName: null,
    description: null,
  };
}

// The catalog's buff of that exact name or GUID (either case), or undefined.
function lookupBuff(query) {
  if (typeof query !== 'string') {
    return undefined;
  }
  return BUFF_BY_NAME.get(query) ?? BUFF_BY_ID.get(query.toLowerCase());
}

// The class ids of claimable class names; throws for anything else.
function claimableClassIds(names) {
  if (!Array.isArray(names)) {
    throw new TypeError('claim takes an array of buff classes');
  }
  return names.map((name) => {
    const entry = typeof name === 'string' ? CLASS_BY_NAME.get(name) : undefined;
    if (!entry || !entry.claimable) {
      throw new TypeError(`Not a claimable buff class: ${String(name)}`);
    }
    return entry.id;
  });
}

// Buffs' read members, the same on both sides.
const buffCatalog = Object.freeze({
  get classes() {
    return [...CLAIMABLE];
  },
  get tags() {
    return TAGS.map((tag) => tag.name);
  },
  find(query) {
    if (typeof query !== 'string') {
      throw new TypeError('find takes a buff name or GUID');
    }
    const buff = lookupBuff(query);
    return buff ? buffInfo(buff) : null;
  },
});

module.exports = {
  BUFFS,
  CLAIMABLE,
  TAG_BY_NAME,
  buffInfo,
  lookupBuff,
  claimableClassIds,
  buffCatalog,
};
