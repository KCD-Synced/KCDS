'use strict';

// Rays against the drawn faces of the author's own models (opened buildings, imported models),
// which the game's rays cannot meet: an opened building keeps no collision inside, so its vault
// and inner walls are drawn only (model_mesh.cjs). The MCP server asks the editor which of them
// stand where (list), reads their .cgf files from the assets folder once (kept while the file is
// unchanged), and meets their triangles itself.

const fs = require('node:fs');
const path = require('node:path');

const { readTriangles, rayMeshes } = require('./model_mesh.cjs');

// A model of the author's own: its name in the assets folder.
const OWN_MODEL = /^objects\/kcds\/[a-z0-9_/]+\.cgf$/;
// Collision-only models of an opened building (open_model.cjs <name>_walls): not drawn.
const COLLISION_ONLY = /_walls\.cgf$/;

class DrawnHits {
  // game: the connection to the editor (server.cjs Game), with call(command, args).
  constructor(game) {
    this.game = game;
    this.meshes = new Map();
    this.folder = null;
  }

  // The assets folder (asked once) and the author's own drawn models placed now.
  async placed() {
    if (!this.folder) {
      const assets = await this.game.call('assets', {});
      this.folder = assets.ok ? assets.result.folder : null;
    }
    const listed = await this.game.call('list', { kind: 'prop' });
    if (!this.folder || !listed.ok) {
      return [];
    }
    return listed.result.filter(
      (thing) => OWN_MODEL.test(thing.name) && !COLLISION_ONLY.test(thing.name),
    );
  }

  // A model's triangles, read again only when its file changed; null when it cannot be read.
  meshOf(name) {
    const file = path.join(this.folder, ...name.split('/'));
    let stamp = null;
    try {
      stamp = fs.statSync(file).mtimeMs;
    } catch {
      return null;
    }
    const known = this.meshes.get(file);
    if (known && known.stamp === stamp) {
      return known.mesh;
    }
    let mesh = null;
    try {
      mesh = readTriangles(fs.readFileSync(file));
    } catch {
      mesh = null;
    }
    this.meshes.set(file, { stamp, mesh });
    return mesh;
  }

  // The first drawn face of the author's own models the ray from `from` to `to` meets, or null.
  async ray(from, to) {
    const models = [];
    for (const thing of await this.placed()) {
      const mesh = this.meshOf(thing.name);
      if (mesh && mesh.materials.length > 0) {
        models.push({ mesh, at: thing.at, turn: thing.turn, scale: thing.scale, name: thing.name });
      }
    }
    return models.length > 0 ? rayMeshes(models, from, to) : null;
  }
}

// The nearer of two hits along a ray from `from` (either may be null).
function nearer(from, first, second) {
  const away = (hit) =>
    hit
      ? Math.hypot(hit.position.x - from.x, hit.position.y - from.y, hit.position.z - from.z)
      : Infinity;
  return away(second) < away(first) ? second : first;
}

module.exports = { DrawnHits, nearer, OWN_MODEL };
