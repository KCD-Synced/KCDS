'use strict';

// Item types a resource registers (the server API's Items): miscellaneous items that
// borrow an existing item's look. Each gets a stable GUID from its id, joins the catalog so every
// inventory, stash and ground-item operation takes it, and goes to every player's game, which
// makes it a class of its own (Client/runtime/inventory/item_types). Its name and description
// travel as the texts of its own localization keys (ITEM_NAMES), as Inventory.renameItem's do.
//
// A usable type offers its use action in the inventory screen, labelled with its useLabel. Choosing
// it asks the server, which checks the row is the player's, the revision is the stock's and the
// type is usable, then raises playerCustomItemUse with the row (frozen) and the revision; the game
// does nothing itself. A client Inventory.whenUsed handler for the item takes the action instead.
//
// A row of a miscellaneous item, crafting material, document or registered type with a name or a
// description of its own (metadata.custom)
// is shown to its player as a view type: a type made the same way, with that name and description,
// that only the shown stock and containers use (the row keeps its own class on the server).
//
// A row of any other category (a weapon, armour, clothing, food, ...) keeps its class in the game
// too, as equipping, combat, wear and the outfit read it: its name and description go to
// its player alone under the row's own keys (kcds_nm_r_<row id>, kcds_in_r_<row id>), and their
// game gives the row's inventory entry and item panel those keys (Client/runtime/inventory/
// row_names).

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const { lookupItemClass, registerCustomClass } = require('./catalog.cjs');
const { clientFileSet } = require('../manifest.cjs');
const { resolveMesh } = require('../props/catalog.cjs');

const ID_PATTERN = /^[A-Za-z0-9:_.-]+$/;
const ID_BYTES = 128;
const NAME_BYTES = 256;
const DESCRIPTION_BYTES = 4096;
const USE_LABEL_BYTES = 64;
const MAX_WEIGHT = 1000000;
const MAX_PRICE = 2147483583;
const MAX_TYPES = 4096;
// The categories a type may look like: the game's miscellaneous items, crafting materials and
// documents.
const VISUALS = new Set(['miscItem', 'craftingMaterial', 'document']);
const ICON_FILE_BYTES = 512;
// A model's path (kMaxItemIconFileBytes, the wire's limit for a path).
const MODEL_BYTES = 512;
// Every icon of the game's items is 64 x 64 and the inventory draws a picture at its own size:
// a .dds of that size is used as it is, an uncompressed 32-bit one of another size (up to 4096
// on a side) is scaled to it by each client (kcd2s/integration/item_icon.hpp).
const ICON_SIZE = 64;
const MAX_ICON_SOURCE = 4096;

// Whether clients can show a .dds as an item icon (see ICON_SIZE).
function iconFits(bytes) {
  if (bytes.length < 128 || bytes.toString('latin1', 0, 4) !== 'DDS ') {
    return false;
  }
  const height = bytes.readUInt32LE(12);
  const width = bytes.readUInt32LE(16);
  if (width === ICON_SIZE && height === ICON_SIZE) {
    return true;
  }
  if (width === 0 || height === 0 || width > MAX_ICON_SOURCE || height > MAX_ICON_SOURCE) {
    return false;
  }
  const flags = bytes.readUInt32LE(80);
  const fourCc = bytes.toString('latin1', 84, 88);
  if (flags & 0x4 && fourCc === 'DX10') {
    return bytes.length >= 148 && [28, 29, 87, 88, 91, 93].includes(bytes.readUInt32LE(128));
  }
  const byte = (mask) => [0xff, 0xff00, 0xff0000, 0xff000000].includes(mask);
  return (
    (flags & 0x40) !== 0 &&
    bytes.readUInt32LE(88) === 32 &&
    byte(bytes.readUInt32LE(92)) &&
    byte(bytes.readUInt32LE(96)) &&
    byte(bytes.readUInt32LE(100))
  );
}

// The icon id of a resource's icon file, as the game client names the .dds it mounts for it
// (Client/runtime/native_ui: kcds_<resource>_<path without .dds>, lower case, any other
// character '_'): the inventory loads Libs/UI/Textures/Icons/Items/<id>_icon.dds.
function itemIconName(resource, path) {
  const stem = `kcds_${resource}_${path.replace(/\.dds$/i, '')}`;
  return stem.toLowerCase().replace(/[^a-z0-9_]/g, '_');
}

// A resource's icon path: relative, inside the resource, a .dds.
function iconPathOf(icon) {
  if (icon === undefined) {
    return '';
  }
  const valid =
    typeof icon === 'string' &&
    /\.dds$/i.test(icon) &&
    !icon.startsWith('/') &&
    !icon.includes('\\') &&
    !icon.split('/').some((part) => part === '' || part === '.' || part === '..') &&
    Buffer.byteLength(icon) <= ICON_FILE_BYTES - ID_BYTES;
  if (!valid) {
    throw new TypeError('Items.register icon must be a .dds file the resource ships, by its path');
  }
  return icon;
}
const CONTROL = /[\x00-\x1f\x7f]/;

// A GUID that only the id decides: the SHA-1 of the id in our namespace, as a name-based (version
// 5) GUID.
function guidOf(id) {
  const hash = crypto.createHash('sha1').update(`kcds:item:${id}`).digest();
  hash[6] = (hash[6] & 0x0f) | 0x50;
  hash[8] = (hash[8] & 0x3f) | 0x80;
  const hex = hash.subarray(0, 16).toString('hex');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(
    16,
    20,
  )}-${hex.slice(20)}`;
}

function text(value, field, limit, { required = false } = {}) {
  if (value === undefined && !required) {
    return '';
  }
  if (typeof value !== 'string' || (required && value.length === 0)) {
    throw new TypeError(`Items.register ${field} must be a${required ? ' non-empty' : ''} string`);
  }
  if (Buffer.byteLength(value) > limit || CONTROL.test(value)) {
    throw new RangeError(`Items.register ${field}: at most ${limit} bytes, no control characters`);
  }
  return value;
}

// The definition as stored and compared: every field with its default.
function definitionOf(definition) {
  if (definition === null || typeof definition !== 'object' || Array.isArray(definition)) {
    throw new TypeError('Items.register definition must be an object');
  }
  const visual = lookupItemClass(definition.visual);
  if (!visual || visual.custom || !VISUALS.has(visual.category)) {
    throw new TypeError(
      'Items.register visual must be a miscellaneous item, crafting material or document',
    );
  }
  const { weight } = definition;
  if (typeof weight !== 'number' || !Number.isFinite(weight) || weight < 0 || weight > MAX_WEIGHT) {
    throw new RangeError(`Items.register weight must be 0 to ${MAX_WEIGHT}`);
  }
  const price = definition.price ?? 0;
  if (!Number.isInteger(price) || price < 0 || price > MAX_PRICE) {
    throw new RangeError(`Items.register price must be a whole number 0 to ${MAX_PRICE}`);
  }
  for (const flag of ['stackable', 'usable']) {
    if (definition[flag] !== undefined && typeof definition[flag] !== 'boolean') {
      throw new TypeError(`Items.register ${flag} must be a boolean`);
    }
  }
  return Object.freeze({
    name: text(definition.name, 'name', NAME_BYTES, { required: true }),
    description: text(definition.description, 'description', DESCRIPTION_BYTES),
    visual: visual.id,
    model: modelOf(definition.model),
    weight,
    price,
    stackable: definition.stackable ?? true,
    usable: definition.usable ?? false,
    useLabel: text(definition.useLabel ?? 'Use', 'useLabel', USE_LABEL_BYTES, { required: true }),
    icon: iconPathOf(definition.icon),
  });
}

// The mesh a type lies on the ground as, in shops and in Items.model: a prop
// catalogue path as Prop.spawn takes it ('manmade/common_furniture/bucket/bucket_a_a', with or
// without objects/ and .cgf, or a file name), as the catalogue writes it; '' for the visual's.
function modelOf(model) {
  if (model === undefined) {
    return '';
  }
  const mesh = typeof model === 'string' && model.length > 0 ? resolveMesh(model) : null;
  if (!mesh || mesh.length > MODEL_BYTES) {
    throw new TypeError(`Items.register model: not a mesh of the prop catalogue: ${String(model)}`);
  }
  return mesh;
}

function sameDefinition(a, b) {
  return Object.keys(a).every((key) => a[key] === b[key]);
}

class CustomItems {
  // `host`: the resource host (bridge, playerInventories for the names).
  constructor(host) {
    this.host = host;
    // id -> { guid, index, definition, itemClass }
    this.types = new Map();
    // The view types of rows with a name or description of their own: key -> the same shape.
    this.views = new Map();
  }

  // The class a row is shown as: its own, or a view type with its own name and description.
  shownClassOf(row) {
    const custom = row.props.custom;
    if (!custom || (custom.name === undefined && custom.description === undefined)) {
      return row.itemClass.id;
    }
    const base = row.itemClass;
    const visual = base.custom ? base.visual : base.id;
    if (!base.custom && !VISUALS.has(base.category)) {
      return base.id;
    }
    const key = `${base.id}|${custom.name ?? ''}|${custom.description ?? ''}`;
    const known = this.views.get(key);
    if (known) {
      return known.guid;
    }
    if (this.views.size >= MAX_TYPES) {
      return base.id;
    }
    const index = this.views.size + 1;
    const guid = guidOf(`row:${key}`);
    const definition = Object.freeze({
      visual,
      weight: base.weight ?? 0,
      price: base.price ?? 0,
      stackable: base.stackable === true,
      usable: base.usable === true,
      useLabel: base.useLabel ?? 'Use',
    });
    // A part the row does not name itself keeps the class's own key, which the game translates.
    const ownDescriptionKey = base.descriptionKey ?? base.nameKey.replace(/^ui_nm_/, 'ui_in_');
    const itemClass = Object.freeze({
      name: `${base.name}:row:${index}`,
      nameKey: custom.name !== undefined ? `kcds_nm_row_${index}` : base.nameKey,
      descriptionKey: custom.description !== undefined ? `kcds_in_row_${index}` : ownDescriptionKey,
      useLabelKey: base.useLabelKey ?? 'ui_hud_use',
    });
    const view = { guid, index, definition, itemClass, icon: null };
    this.views.set(key, view);
    this.send(0, false, [view]);
    const names = [];
    if (custom.name !== undefined) {
      names.push([itemClass.nameKey, custom.name]);
    }
    if (custom.description !== undefined) {
      names.push([itemClass.descriptionKey, custom.description]);
    }
    for (const [nameKey, text] of names) {
      this.host.playerInventories.itemNames.set(nameKey, text);
    }
    this.host.playerInventories.sendNames(0, false, names);
    return guid;
  }

  // The names the rows of an inventory show under keys of their own: key -> text, for the
  // rows of the categories a view type does not show (see shownClassOf).
  rowNamesOf(inventory) {
    const names = new Map();
    for (const row of inventory.rows.values()) {
      const custom = row.props.custom;
      const base = row.itemClass;
      if (!custom || base.custom || VISUALS.has(base.category)) {
        continue;
      }
      if (custom.name) {
        names.set(`kcds_nm_r_${row.id}`, custom.name);
      }
      if (custom.description) {
        names.set(`kcds_in_r_${row.id}`, custom.description);
      }
    }
    return names;
  }

  // Items.register: the type's GUID. An identical repeat answers the same; a different
  // definition under a known id throws.
  register(id, definition) {
    if (typeof id !== 'string' || !ID_PATTERN.test(id) || Buffer.byteLength(id) > ID_BYTES) {
      throw new TypeError('Items.register id: letters, digits and :_-. only, at most 128 bytes');
    }
    const wanted = definitionOf(definition);
    const icon = wanted.icon ? this.iconOf(wanted.icon) : null;
    const known = this.types.get(id);
    if (known) {
      if (!sameDefinition(known.definition, wanted)) {
        throw new Error(`Items.register: ${id} is registered with another definition`);
      }
      return known.guid;
    }
    if (this.types.size >= MAX_TYPES) {
      throw new RangeError(`Items.register: at most ${MAX_TYPES} types`);
    }
    const guid = guidOf(id);
    const index = this.types.size + 1;
    const visual = lookupItemClass(wanted.visual);
    const itemClass = Object.freeze({
      id: guid,
      name: id,
      nameKey: `kcds_nm_item_${index}`,
      descriptionKey: `kcds_in_item_${index}`,
      useLabelKey: `kcds_use_item_${index}`,
      category: 'miscItem',
      maximumQuality: 1,
      quest: false,
      price: wanted.price,
      weight: wanted.weight,
      stackable: wanted.stackable,
      qualityBands: false,
      // The equipment slot of the class it looks like, if it has one (a visual is a miscellaneous
      // item, material or document today, so none does).
      slot: visual.slot,
      coversFeet: visual.coversFeet,
      custom: true,
      visual: visual.id,
      // Its own mesh, else the one of the class it looks like.
      model: wanted.model || visual.model || '',
      usable: wanted.usable,
      useLabel: wanted.useLabel,
    });
    registerCustomClass(itemClass);
    const type = { guid, index, definition: wanted, itemClass, icon };
    this.types.set(id, type);
    // Every game makes the type before it shows one, then names it.
    this.send(0, false, [type]);
    const names = this.host.playerInventories.itemNames;
    names.set(itemClass.nameKey, wanted.name);
    names.set(itemClass.descriptionKey, wanted.description || ' ');
    names.set(itemClass.useLabelKey, wanted.useLabel);
    this.host.playerInventories.sendNames(0, false, [
      [itemClass.nameKey, wanted.name],
      [itemClass.descriptionKey, wanted.description || ' '],
      [itemClass.useLabelKey, wanted.useLabel],
    ]);
    return guid;
  }

  // An icon the registering resource ships to clients: { name, file } (the icon id and
  // "<resource>/<path>"). Throws outside a resource or for a file clients do not get.
  iconOf(file) {
    const owner = this.host.context.getStore();
    const definition = owner ? this.host.manifests?.get(owner.name) : null;
    if (!definition) {
      throw new Error('Items.register icon: only a resource can ship an icon');
    }
    if (!clientFileSet(definition.folder, definition.manifest).has(file)) {
      throw new TypeError(`Items.register icon: ${owner.name} does not ship ${file} to clients`);
    }
    if (!iconFits(fs.readFileSync(path.join(definition.folder, file)))) {
      throw new TypeError(
        'Items.register icon: a .dds of 64 x 64, or an uncompressed 32-bit one clients scale to it',
      );
    }
    return Object.freeze({ name: itemIconName(owner.name, file), file: `${owner.name}/${file}` });
  }

  // [guid, visual, id, nameKey, descriptionKey, useLabelKey, weight, price, stackable, usable,
  // icon, iconFile, model] per type; model '' for the visual's.
  send(to, full, types) {
    this.host.bridge.send({
      op: 'itemTypes',
      id: to,
      full,
      types: types.map(({ guid, definition, itemClass, icon }) => [
        guid,
        definition.visual,
        itemClass.name,
        itemClass.nameKey,
        itemClass.descriptionKey,
        itemClass.useLabelKey,
        definition.weight,
        definition.price,
        definition.stackable,
        definition.usable,
        icon ? icon.name : '',
        icon ? icon.file : '',
        definition.model ?? '',
      ]),
    });
  }

  // A player who came into the world hears every type, before their inventory.
  sendAll(player) {
    if (this.types.size > 0 || this.views.size > 0) {
      this.send(player.id, true, [...this.types.values(), ...this.views.values()]);
    }
  }

  // A player's game chose the use action on a row: playerCustomItemUse when the row is theirs, at
  // the revision their game showed, of a usable type. Whether it was raised.
  use(player, row, revision) {
    const entry = player ? this.host.playerInventories.entryOf(player) : undefined;
    const held = entry?.inventory.rows.get(row);
    if (!held || !held.itemClass.custom || !held.itemClass.usable) {
      return false;
    }
    if (revision !== entry.inventory.revision) {
      return false;
    }
    const view = Object.freeze({ ...entry.inventory.rowView(held) });
    this.host.emit('playerCustomItemUse', player, view, revision);
    // A custom recipe's sketch teaches it.
    this.host.customCrafting?.itemUsed(player, view);
    return true;
  }

  api() {
    return Object.freeze({
      register: (id, definition) => this.register(id, definition),
      // The mesh a class lies on the ground as: under objects/, '' when the tables
      // give none. Prop.spawn takes most of them, to show an item on its own.
      model: (item) => {
        const itemClass = lookupItemClass(item);
        if (!itemClass) {
          throw new TypeError(`Items.model: unknown item class: ${String(item)}`);
        }
        return itemClass.model ?? '';
      },
      // What kind of item a class is: its category and, for a weapon, its class and
      // attack modifiers from the game's tables; null for an unknown class.
      info: (item) => {
        const itemClass = lookupItemClass(item);
        if (!itemClass) {
          return null;
        }
        return {
          id: itemClass.id,
          name: itemClass.name,
          category: itemClass.category,
          weaponClass: itemClass.weaponClass ?? null,
          attackModSlash: itemClass.attackModSlash ?? 0,
          attackModStab: itemClass.attackModStab ?? 0,
          attackModSmash: itemClass.attackModSmash ?? 0,
        };
      },
    });
  }
}

module.exports = { CustomItems, guidOf, iconFits, itemIconName };
