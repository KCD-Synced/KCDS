'use strict';

// What kind of natural object a mesh is, by the game's folders of natural objects: a ray's hit
// `category` on the server (world/traces.cjs) and in client scripts (Client/resources/world.cjs).

const CATEGORIES = [
  ['objects/natural/vegetation/trees/', 'tree'],
  ['objects/natural/vegetation/bushes/', 'bush'],
  ['objects/natural/vegetation/', 'plant'],
  ['objects/natural/rocks/', 'rock'],
  ['objects/natural/stones/', 'rock'],
];

// A mesh path in one form: lower case with '/', as resolveMesh and the props catalog name meshes
// (the game reports the path as the level spelled it, 'Objects/...' or 'objects/...').
function meshPathOf(model) {
  return typeof model === 'string' && model !== ''
    ? model.toLowerCase().replaceAll('\\', '/')
    : null;
}

// 'tree', 'bush', 'plant' or 'rock', or null.
function categoryOf(model) {
  const path = meshPathOf(model);
  if (!path) {
    return null;
  }
  for (const [prefix, category] of CATEGORIES) {
    if (path.startsWith(prefix)) {
      return category;
    }
  }
  return null;
}

module.exports = { categoryOf, meshPathOf };
