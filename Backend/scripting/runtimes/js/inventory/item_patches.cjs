'use strict';

// A game mod's items on the server: the item rows of a resource's table patches
// (replace/Libs/Tables/item/item__<name>.xml) join the item catalog, so inventories, stashes,
// shops and the ground take them like the game's own. Players' games load the same patches with
// the game's tables at their next start (Client/runtime/startup_tables), so the classes exist
// there too.
//
// A worn class finds its slot through its clothing: the clothing a CharacterComponent patch of the
// same resource defines (replace/Libs/Tables/Character/CharacterComponent__<name>.xml), with the
// nearest ArmorType of itself or of the clothing it is nested in, else the game's clothing of that
// name. The rows are read as Dev/tools/items/build_catalog.py reads the game's.

const {
  addPatchedClass,
  gameClothing,
  slotOfArmorType,
  weaponClassName,
} = require('./catalog.cjs');

// The table elements the catalog leaves out, and those whose units share one item.
const SKIPPED_ELEMENTS = new Set(['NPCTool']);
const STACKABLE_ELEMENTS = new Set(['Ammo', 'Money']);
const WORN_ELEMENTS = new Set(['Armor', 'Helmet', 'Hood', 'QuickSlotContainer']);
const WEAPON_ELEMENTS = new Set(['MeleeWeapon', 'MissileWeapon']);
// The armour archetypes of plate legs that cover the feet.
const FEET_ARCHETYPES = new Set(['57', '58']);
const GUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

function decodeEntities(text) {
  return text
    .replace(/&quot;/g, '"')
    .replace(/&apos;/g, "'")
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&amp;/g, '&');
}

function attributesOf(source) {
  const attributes = {};
  for (const match of source.matchAll(/([A-Za-z_][\w.:-]*)\s*=\s*("([^"]*)"|'([^']*)')/g)) {
    attributes[match[1]] = decodeEntities(match[3] ?? match[4] ?? '');
  }
  return attributes;
}

// Every element of an XML text in document order: { tag, attributes, parent }, the parent being
// the element it is nested in (null at the top). A comment is read as one token and skipped, so
// tags inside it count for nothing; declarations are skipped too.
const TOKEN = /<!--[\s\S]*?(?:-->|$)|<(\/?)([A-Za-z_][\w.:-]*)([^>]*?)(\/?)>/g;

function elementsOf(text) {
  const elements = [];
  const open = [];
  for (const match of String(text).matchAll(TOKEN)) {
    const [whole, closing, tag, rest, selfClosing] = match;
    if (whole.startsWith('<!--')) {
      continue;
    }
    if (closing) {
      while (open.length > 0 && open.pop().tag !== tag) {
        // An element left open closes with its parent.
      }
      continue;
    }
    const element = {
      tag,
      attributes: attributesOf(rest),
      parent: open.length > 0 ? open[open.length - 1] : null,
    };
    elements.push(element);
    if (!selfClosing) {
      open.push(element);
    }
  }
  return elements;
}

function number(text, fallback = 0) {
  const value = Number(text);
  return text === undefined || text === '' || !Number.isFinite(value) ? fallback : value;
}

function camel(tag) {
  return tag[0].toLowerCase() + tag.slice(1);
}

// A table's mesh path as the game opens it: under objects/, forward slashes, .cgf when the
// table leaves the extension out.
function meshPath(path) {
  let mesh = String(path).replace(/\\/g, '/').replace(/^\/+/, '');
  if (!mesh.split('/').pop().includes('.')) {
    mesh += '.cgf';
  }
  return mesh.toLowerCase().startsWith('objects/') ? mesh : `objects/${mesh}`;
}

// The clothing of CharacterComponent patches by name: { armorType, coversFeet, dropModel }, each
// the nearest of itself, the clothing it is nested in, or the game's clothing of that name.
function clothingOfPatches(texts) {
  const found = new Map();
  for (const text of texts) {
    const resolved = new Map();
    for (const element of elementsOf(text)) {
      const name = element.attributes.Name;
      const own = element.attributes;
      const game = name ? gameClothing(name) : undefined;
      const parent = element.parent ? resolved.get(element.parent) : undefined;
      const archetype = own.ArmorArchetypeId;
      const entry = {
        armorType: own.ArmorType ?? game?.armorType ?? parent?.armorType ?? null,
        coversFeet:
          archetype !== undefined
            ? FEET_ARCHETYPES.has(archetype)
            : game?.coversFeet ?? parent?.coversFeet ?? false,
        dropModel: own.DropModel
          ? meshPath(own.DropModel)
          : game?.dropModel ?? parent?.dropModel ?? '',
      };
      resolved.set(element, entry);
      if (element.tag === 'Clothing' && name) {
        found.set(name, Object.freeze(entry));
      }
    }
  }
  return found;
}

// The catalog entry of one item row, or null for a row the catalog leaves out.
function classOfRow(element, clothing) {
  const attributes = element.attributes;
  const id = String(attributes.Id ?? '').toLowerCase();
  if (SKIPPED_ELEMENTS.has(element.tag) || !GUID.test(id) || !attributes.Name) {
    return null;
  }
  const worn = WORN_ELEMENTS.has(element.tag);
  const weapon = WEAPON_ELEMENTS.has(element.tag);
  const wears = worn && attributes.Clothing ? attributes.Clothing : null;
  const clothingEntry = wears ? clothing.get(wears) ?? gameClothing(wears) : undefined;
  const slot = clothingEntry?.armorType ? slotOfArmorType(clothingEntry.armorType) : null;
  let model = attributes.Model ? meshPath(attributes.Model) : '';
  if (!model && clothingEntry?.dropModel) {
    model = clothingEntry.dropModel;
  }
  return Object.freeze({
    id,
    name: attributes.Name,
    nameKey: attributes.UIName ?? '',
    category: camel(element.tag),
    maximumQuality: Math.trunc(number(attributes.MaxQuality, 1)) || 1,
    quest: attributes.IsQuestItem === 'true',
    price: number(attributes.Price),
    weight: number(attributes.Weight),
    stackable: attributes.IsDivisible === 'true' || STACKABLE_ELEMENTS.has(element.tag),
    qualityBands: attributes.MaxQuality !== undefined,
    slot,
    coversFeet: Boolean(slot) && Boolean(clothingEntry?.coversFeet),
    model,
    weaponClass: weapon ? weaponClassName(attributes.Class) : null,
    attackModSlash: weapon ? number(attributes.AttackModSlash) : 0,
    attackModStab: weapon ? number(attributes.AttackModStab) : 0,
    attackModSmash: weapon ? number(attributes.AttackModSmash) : 0,
  });
}

// The rows of one resource's item patches (texts of item__<name>.xml) join the catalog, with the
// clothing of its CharacterComponent patches. Answers { added, replaced }: the names of the new
// classes and of the game's classes a row took the place of.
function addItemTablePatches(itemTexts, componentTexts = []) {
  const clothing = clothingOfPatches(componentTexts);
  const added = [];
  const replaced = [];
  for (const text of itemTexts) {
    for (const element of elementsOf(text)) {
      const entry = classOfRow(element, clothing);
      if (!entry) {
        continue;
      }
      const fresh = addPatchedClass(entry);
      if (fresh === true) {
        added.push(entry.name);
      } else if (fresh === false) {
        replaced.push(entry.name);
      }
    }
  }
  return { added, replaced };
}

module.exports = { addItemTablePatches, clothingOfPatches };
