'use strict';

// The levels' own areas: what Area.all('level') and Area.getAt answer for the map a
// server runs. level_areas.json.gz is generated from a game installation by
// Dev/tools/areas/build_level_areas.py: { <level>: [{ id, name, type, labels, height,
// points: [x, y, z, ...] }, ...] }, world metres. A TriggerArea's prism comes from the level's
// baked TriggerAreas.fubar, a SmartAreaShape's polygon from its own Area points. An AreaUnion (a
// house of several rooms) is { id, name, type, labels, members: [ids] } after them.
//
// The catalog is read once, when the server names its map, and only that level's areas are kept.

const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const { Vector3 } = require('../math.cjs');

const CATALOG = path.join(__dirname, 'level_areas.json.gz');
const LEVEL_TYPES = ['TriggerArea', 'SmartAreaShape', 'AreaUnion'];

// One catalog entry as the fields a level area is built from, or null for one that cannot be.
function entryOf(raw) {
  if (raw === null || typeof raw !== 'object') {
    return null;
  }
  const { id, name, type, labels, height, points, members } = raw;
  if (typeof id !== 'string' || !LEVEL_TYPES.includes(type)) {
    return null;
  }
  const ownLabels = Array.isArray(labels)
    ? labels.filter((label) => typeof label === 'string')
    : [];
  if (type === 'AreaUnion') {
    if (!Array.isArray(members) || !members.length) {
      return null;
    }
    return {
      id: id.toLowerCase(),
      name: typeof name === 'string' ? name : '',
      type,
      labels: ownLabels,
      members: members.filter((member) => typeof member === 'string').map((m) => m.toLowerCase()),
    };
  }
  if (!Array.isArray(points) || points.length < 9 || points.length % 3 !== 0) {
    return null;
  }
  if (!points.every((value) => typeof value === 'number' && Number.isFinite(value))) {
    return null;
  }
  const corners = [];
  for (let i = 0; i < points.length; i += 3) {
    corners.push(new Vector3(points[i], points[i + 1], points[i + 2]));
  }
  return {
    id: id.toLowerCase(),
    name: typeof name === 'string' ? name : '',
    type,
    labels: ownLabels,
    height: typeof height === 'number' && Number.isFinite(height) && height > 0 ? height : 0,
    points: corners,
  };
}

// The areas of one level, or an empty list for a level the catalog does not hold (klaster).
function loadLevelAreas(level, file = CATALOG) {
  let catalog;
  try {
    catalog = JSON.parse(zlib.gunzipSync(fs.readFileSync(file)).toString('utf8'));
  } catch (error) {
    throw new Error(`The level area catalog could not be read: ${error.message}`);
  }
  const raw = catalog[level];
  if (!Array.isArray(raw)) {
    return [];
  }
  const entries = [];
  for (const item of raw) {
    const entry = entryOf(item);
    if (entry) {
      entries.push(entry);
    }
  }
  return entries;
}

module.exports = { loadLevelAreas, entryOf, LEVEL_TYPES };
