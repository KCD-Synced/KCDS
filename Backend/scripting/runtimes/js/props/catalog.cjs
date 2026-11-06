'use strict';
// The game's prop catalog: every static mesh under objects/, by its lower-case path, built from a
// game installation by Dev/tools/props/build_catalog.py. Player.setDisguise takes a mesh from it
const catalog = require('./catalog.json');
// How each mesh collides (Dev/tools/props/build_collision.py): the meshes that collide
// as drawn ("mesh") and those nothing collides with ("none"); every other one collides only with
// its simplified shapes ("proxy").
const collision = require('./collision.json');

const paths = new Set();
// file stem -> the first path in catalog order with that stem
const stems = new Map();
for (const [folder, files] of Object.entries(catalog.folders)) {
  for (const file of files) {
    const path = `${folder}/${file}`;
    paths.add(path);
    const stem = file.slice(0, -'.cgf'.length);
    if (!stems.has(stem)) {
      stems.set(stem, path);
    }
  }
}

// The catalog path of a mesh named by its path (with or without objects/ and .cgf, as the game's
// animation props name them) or file stem (any case), or null for one the catalog
// does not carry.
function resolveMesh(model) {
  const name = model.replace(/\\/g, '/').toLowerCase();
  const forms = [name, `objects/${name}`, `${name}.cgf`, `objects/${name}.cgf`];
  for (const form of forms) {
    if (paths.has(form)) {
      return form;
    }
  }
  return stems.get(name) ?? null;
}

const kinds = new Map();
for (const kind of ['mesh', 'none']) {
  for (const [folder, files] of Object.entries(collision[kind])) {
    for (const file of files) {
      kinds.set(`${folder}/${file}`, kind);
    }
  }
}

// How a catalog mesh collides: 'mesh' (as it is drawn), 'proxy' (only with its simplified shapes:
// a box, cylinder, capsule or a rough mesh around it) or 'none'; null for a mesh the catalog does
// not carry.
function collisionOf(model) {
  const path = typeof model === 'string' ? resolveMesh(model) : null;
  if (!path) {
    return null;
  }
  return kinds.get(path) ?? 'proxy';
}

// The game's prefabs (Dev/tools/props/build_prefabs.py): the templates the game can
// build at run time, by their lower-case path under Prefabs/ without .xml, with their GUIDs, and
// those it cannot, with why.
const prefabCatalog = require('./prefabs.json');

// file stem -> the first prefab path in catalog order with that stem (built or blocked)
const prefabStems = new Map();
for (const path of [...Object.keys(prefabCatalog.prefabs), ...Object.keys(prefabCatalog.blocked)]) {
  const stem = path.slice(path.lastIndexOf('/') + 1);
  if (!prefabStems.has(stem)) {
    prefabStems.set(stem, path);
  }
}

// A prefab named by its path under Prefabs/ (with or without prefabs/ and .xml) or file stem (any
// case): { path, guid } for one the game builds, { path, blocked } (why) for one it does not, null
// for one the catalog does not carry.
function resolvePrefab(name) {
  let path = name.replace(/\\/g, '/').toLowerCase();
  path = path.replace(/^prefabs\//, '').replace(/\.xml$/, '');
  if (!(path in prefabCatalog.prefabs) && !(path in prefabCatalog.blocked)) {
    path = prefabStems.get(path) ?? null;
  }
  if (path === null) {
    return null;
  }
  if (path in prefabCatalog.blocked) {
    return { path, blocked: prefabCatalog.blocked[path] };
  }
  return { path, guid: prefabCatalog.prefabs[path] };
}

// The paths of the prefabs the game can build, in catalog order.
function prefabNames() {
  return Object.keys(prefabCatalog.prefabs);
}

// Every mesh path of the catalog, in catalog order (World Architect's Props content).
function meshPaths() {
  return [...paths];
}

module.exports = { resolveMesh, collisionOf, resolvePrefab, prefabNames, meshPaths };
