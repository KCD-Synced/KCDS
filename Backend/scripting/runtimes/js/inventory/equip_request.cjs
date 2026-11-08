'use strict';

// What Player.equip and Horse.equip take: one piece, the same way for both. A class by name or
// GUID, or { item, metadata } for a piece with properties of its own (quality, health or
// condition, onEquipBuffs, custom { name, description, data } and transient, as Inventory.add
// takes them).

const { lookupItemClass } = require('./catalog.cjs');
const { propsOf, metadataOf } = require('./inventories.cjs');

// A plain object of any realm: resources run in their own contexts, whose Object differs.
function isPlainObject(value) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    return false;
  }
  const prototype = Object.getPrototypeOf(value);
  return prototype === null || Object.getPrototypeOf(prototype) === null;
}

// The piece a request names: { itemClass, props, metadata }, with the properties normalized and
// the metadata as a row reads it (defaults left out). Throws a TypeError for anything else: a
// request of another shape, a class the catalog does not carry, or metadata Inventory.add refuses.
function equipRequestOf(request, what) {
  let item = request;
  let given;
  if (isPlainObject(request)) {
    for (const key of Object.keys(request)) {
      if (key !== 'item' && key !== 'metadata') {
        throw new TypeError(`${what}: unknown key ${key}, it takes { item, metadata }`);
      }
    }
    item = request.item;
    given = request.metadata;
  } else if (typeof request !== 'string') {
    throw new TypeError(`${what} takes an item class name or GUID, or { item, metadata }`);
  }
  const itemClass = lookupItemClass(item);
  if (!itemClass) {
    throw new TypeError(`${what}: unknown item ${String(item)}`);
  }
  const { props, code } = propsOf(itemClass, given);
  if (code) {
    throw new TypeError(`${what}: ${code} for ${itemClass.name}`);
  }
  return { itemClass, props, metadata: metadataOf(itemClass, props) };
}

module.exports = { equipRequestOf };
