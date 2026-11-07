'use strict';

// The MCP server's open_model: a building of the game opened up (open_core.cjs) on the author's
// PC, its files read from the game's packs and written into the author's assets folder. World
// Architect in the game opens one the same way with the game's own files
// (Client/resources/architect/opening.cjs).

const fs = require('node:fs');
const path = require('node:path');

const { zipEntries, zipRead, findGame } = require('../custom_map.cjs');
const core = require('./open_core.cjs');

// The game's packs that hold models, searched in order.
function modelPacks(game) {
  const data = path.join(game, 'Data');
  return fs
    .readdirSync(data)
    .filter((name) => /^(IPL_)?Objects.*\.pak$/i.test(name))
    .map((name) => path.join(data, name));
}

// The bytes of a game file (a path inside its packs), or null.
function gameFile(game, file) {
  const wanted = file.replace(/\\/g, '/').toLowerCase();
  for (const pack of modelPacks(game)) {
    const entries = zipEntries(pack);
    if (entries.has(wanted)) {
      return zipRead(pack, entries, wanted);
    }
  }
  return null;
}

// Reading the game's packs in `game` and writing into `assetsFolder`, for open_core.cjs.
function folderIo(game, assetsFolder) {
  const write = (file, data) => {
    const target = path.join(assetsFolder, ...file.split('/'));
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, data);
  };
  return {
    read: (file) => gameFile(game, file),
    text: (file) => {
      const bytes = gameFile(game, file);
      return bytes ? bytes.toString('utf8') : null;
    },
    write,
    split: (from, base, material) => {
      const streamed = gameFile(game, from);
      if (!streamed) {
        return null;
      }
      return core.drawnPieces(streamed, material).map((piece, index) => {
        const file = `${base}_${index}.cgf`;
        write(file, piece);
        return file;
      });
    },
  };
}

// Opens `mesh` (a game model's path) into `assetsFolder` as objects/kcds/opened/<name>.cgf
// (options as open_core.cjs openWith takes them).
function openModel(options, assetsFolder, game) {
  return core.openWith(folderIo(game ?? findGame(), assetsFolder), options ?? {});
}

module.exports = {
  openModel,
  gameFile,
  openBytes: core.openBytes,
  boxProxies: core.boxProxies,
  swapSubMaterials: core.swapSubMaterials,
  renameMaterial: core.renameMaterial,
};
