'use strict';

// World Architect's custom maps made by the MCP server (Dev/tools/architect-mcp/custom_map.cjs,
// its new_map tool): the packs it writes read back, and bad names, sizes and grounds refused
// before anything is written. The map itself is compared with Dev/tools/architect/make_custom_map.py
// on a PC with the game (byte for byte).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const {
  makeCustomMap,
  zipEntries,
  zipRead,
  zipWrite,
  crc32,
} = require('../../../../Dev/tools/architect-mcp/custom_map.cjs');

test('a pack written reads back, deflated, with each file whole', () => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-map-'));
  const file = path.join(folder, 'level.pak');
  const big = Buffer.alloc(200000, 7);
  zipWrite(file, { 'levelinfo.xml': '<Level />', 'terrain/terrain.dat': big });
  const entries = zipEntries(file);
  assert.deepEqual([...entries.keys()], ['levelinfo.xml', 'terrain/terrain.dat']);
  assert.equal(entries.get('terrain/terrain.dat').method, 8);
  assert.equal(zipRead(file, entries, 'levelinfo.xml').toString(), '<Level />');
  assert.deepEqual(zipRead(file, entries, 'TERRAIN/terrain.dat'), big);
  assert.equal(zipRead(file, entries, 'terrain/terrain.dat', 1000).length, 1000);
  if (zlib.crc32) {
    assert.equal(crc32(big), zlib.crc32(big));
  }
  fs.rmSync(folder, { recursive: true });
});

test('bad names, sizes, heights and grounds are refused', () => {
  const game = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-game-'));
  assert.throws(() => makeCustomMap({ name: 'Prague!', game }), /lower-case/);
  assert.throws(() => makeCustomMap({ name: 'praha', size: 500, game }), /size/);
  assert.throws(() => makeCustomMap({ name: 'praha', height: -1, game }), /height/);
  assert.throws(() => makeCustomMap({ name: 'praha', ground: 'lava', game }), /ground/);
  fs.rmSync(game, { recursive: true });
});

test('a map name already on this PC is refused, before the game files are read', () => {
  const maps = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-maps-'));
  fs.mkdirSync(path.join(maps, 'kcds_map_praha'));
  fs.writeFileSync(path.join(maps, 'kcds_map_praha', 'level.pak'), 'old');
  assert.throws(() => makeCustomMap({ name: 'praha', game: maps, out: maps }), /already exists/);
  fs.rmSync(maps, { recursive: true });
});
