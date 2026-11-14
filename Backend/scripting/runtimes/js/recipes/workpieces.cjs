'use strict';

// The smithing workpieces resources register: Crafting.defineWorkpiece(definition). A workpiece is
// what a smithing recipe is forged on (its WorkpieceId): a row of the game's
// BlacksmithWorkpieces.xml, the model laid on the anvil and the map of zones its strikes land in.
// The game has four (sword, longsword, axe, horseshoe, in the recipe catalog). A definition may
// start from one of them (`like`) and change any column; it is written as a patch of that table
// (BlacksmithWorkpieces__kcds_<id>.xml), which every client loads into the game's own workpiece
// database (Client/runtime/crafting/recipe_tables.hpp) before the recipes that name it.

const catalog = require('./catalog.json');

const ID = /^[A-Za-z0-9_.-]{1,64}$/;
// A model the game loads by path: its own (objects/...) or a resource's streamed one.
const MODEL = /^objects\/[a-z0-9_./-]{1,240}\.(cdf|chr|cgf)$/;
const NAME = /^[A-Za-z0-9_]{1,64}$/;
const AUDIO_SWITCHES = ['sword', 'axe', 'horseshoe'];
const POINT_LIMIT = 10;
// The table's columns: the definition's field, the row's attribute, the kind of value.
const COLUMNS = [
  ['model', 'Model', 'model'],
  ['tag', 'Tag', 'name'],
  ['audioSwitch', 'AudioSwitch', 'audio'],
  ['zones', 'Zones', 'zones'],
  ['useTongs', 'UseTongs', 'boolean'],
  ['raycastAttachment', 'RaycastAttachment', 'name'],
  ['initialPosition', 'InitialPosition', 'point'],
  ['mapSize', 'MapSize', 'point'],
  ['mapOffset', 'MapOffset', 'point'],
  ['mapRotation', 'MapRotation', 'angles'],
  ['startMinPosition', 'StartMinPosition', 'point'],
  ['endMinPosition', 'EndMinPosition', 'point'],
  ['startMaxPosition', 'StartMaxPosition', 'point'],
  ['endMaxPosition', 'EndMaxPosition', 'point'],
];

// The game's workpieces by id, each row's attributes as the table writes them.
const GAME = new Map(catalog.workpieces.map((row) => [row.Id, row]));
// The registered ones by id: { owner, key, table }.
const REGISTERED = new Map();

function fail(message) {
  throw new TypeError(`Crafting.defineWorkpiece: ${message}`);
}

// A list of `count` finite numbers within the limit, as the table writes it ("0.5,0.2").
function numbersOf(value, count, field) {
  if (
    !Array.isArray(value) ||
    value.length !== count ||
    !value.every(
      (number) =>
        typeof number === 'number' && Number.isFinite(number) && Math.abs(number) <= POINT_LIMIT,
    )
  ) {
    fail(`${field} must be ${count} numbers from -${POINT_LIMIT} to ${POINT_LIMIT}`);
  }
  return value.map((number) => String(Number(number.toFixed(4)))).join(',');
}

// A definition field as the table's attribute text; throws for a value the column cannot take.
function columnOf(value, kind, field) {
  if (kind === 'model') {
    const model = typeof value === 'string' ? value.replace(/\\/g, '/').toLowerCase() : '';
    if (!MODEL.test(model)) {
      fail(`${field} must be a model path such as 'objects/.../name.cdf' (.cdf, .chr or .cgf)`);
    }
    return model;
  }
  if (kind === 'name') {
    if (typeof value !== 'string' || !NAME.test(value)) {
      fail(`${field} must be a name of letters, digits and '_'`);
    }
    return value;
  }
  if (kind === 'audio') {
    if (!AUDIO_SWITCHES.includes(value)) {
      fail(`${field} must be one of ${AUDIO_SWITCHES.join(', ')}`);
    }
    return value;
  }
  if (kind === 'zones') {
    if (!Number.isInteger(value) || value < 1 || value > 8) {
      fail(`${field} must be a whole number from 1 to 8`);
    }
    return String(value);
  }
  if (kind === 'boolean') {
    if (typeof value !== 'boolean') {
      fail(`${field} must be true or false`);
    }
    return value ? 'true' : 'false';
  }
  return numbersOf(value, kind === 'angles' ? 3 : 2, field);
}

// The definition checked: { id, row } with the row's attributes, the `like` workpiece's first.
function normalize(definition) {
  if (!definition || typeof definition !== 'object') {
    fail('takes a workpiece definition');
  }
  const id = definition.id;
  if (typeof id !== 'string' || !ID.test(id)) {
    fail("id must be 1 to 64 letters, digits, '_', '-' or '.'");
  }
  if (GAME.has(id)) {
    fail(`the game has a workpiece ${id}`);
  }
  let base = {};
  if (definition.like !== undefined) {
    if (!GAME.has(definition.like)) {
      fail(`like must be one of the game's workpieces: ${[...GAME.keys()].join(', ')}`);
    }
    base = { ...GAME.get(definition.like) };
  }
  const row = { ...base, Id: id };
  for (const [field, attribute, kind] of COLUMNS) {
    if (definition[field] !== undefined) {
      row[attribute] = columnOf(definition[field], kind, field);
    }
  }
  if (!row.Model) {
    fail('needs a model, or like one of the game workpieces');
  }
  return { id, row };
}

function escape(value) {
  return String(value)
    .replaceAll('&', '&amp;')
    .replaceAll('"', '&quot;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;');
}

// The patch file: its name (the game path is Libs/Tables/minigame/<file>) and its text.
function patchOf({ id, row }) {
  const attributes = Object.entries(row)
    .map(([name, value]) => ` ${name}="${escape(value)}"`)
    .join('');
  const patch = [
    '<?xml version="1.0" encoding="utf-8"?>',
    '<database name="barbora">',
    '  <BlacksmithWorkpieces version="1">',
    `    <BlacksmithWorkpiece${attributes} />`,
    '  </BlacksmithWorkpieces>',
    '</database>',
    '',
  ].join('\n');
  const suffix = id.toLowerCase().replace(/[^a-z0-9_.-]/g, '_');
  return { file: `BlacksmithWorkpieces__kcds_${suffix}.xml`, patch };
}

// Registers a workpiece until the server stops: its table patch { file, patch } for the clients,
// or null when the same definition was registered before. Throws for an invalid definition, an id
// of the game's, or another definition under an id already registered.
function defineWorkpiece(owner, definition) {
  const workpiece = normalize(definition);
  const key = JSON.stringify(workpiece.row);
  const before = REGISTERED.get(workpiece.id);
  if (before) {
    if (before.key === key) {
      return null;
    }
    fail(`workpiece ${workpiece.id} is already registered with another definition`);
  }
  const table = patchOf(workpiece);
  REGISTERED.set(workpiece.id, { owner, key, table, row: workpiece.row });
  return table;
}

// Every workpiece id a recipe may name.
function workpieceIds() {
  return [...GAME.keys(), ...REGISTERED.keys()];
}

// The registered workpieces' table patches, oldest first: for a client whose world became ready,
// before the recipes that name them.
function registeredTables() {
  return [...REGISTERED.values()].map(({ table }) => table);
}

module.exports = { defineWorkpiece, workpieceIds, registeredTables };
