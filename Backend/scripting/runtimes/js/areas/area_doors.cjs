'use strict';

// The doors the levels link to their areas: area.doors, Door.linkedTo and door.areas.
// Built by Dev/tools/areas/build_area_doors.py: { level: { area id: [door, ...] } }, a door being
// { guid, name, position, link }. Read once, when it is first asked for.

const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');

const CATALOG = path.join(__dirname, 'area_doors.json.gz');

let catalog = null;

function loaded() {
  if (catalog === null) {
    catalog = JSON.parse(zlib.gunzipSync(fs.readFileSync(CATALOG)).toString('utf8'));
  }
  return catalog;
}

function copyOf(door) {
  return { ...door, position: { ...door.position } };
}

// The doors a level links to one of its areas; an unknown or unconnected area gives an empty list.
function doorsOfArea(level, areaId) {
  const rows = loaded()[level]?.[String(areaId).toLowerCase()];
  return rows ? rows.map(copyOf) : [];
}

// The ids of the areas a level links a door to, by the door's GUID.
function areasOfDoor(level, guid) {
  const ids = [];
  const areas = loaded()[level] ?? {};
  for (const [areaId, rows] of Object.entries(areas)) {
    if (rows.some((door) => door.guid === guid)) {
      ids.push(areaId);
    }
  }
  return ids;
}

module.exports = { doorsOfArea, areasOfDoor };
