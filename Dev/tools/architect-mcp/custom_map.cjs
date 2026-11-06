'use strict';

// A World Architect custom map from scratch, made by the MCP server on the author's PC (the
// new_map tool): the same flat map as Dev/tools/architect/make_custom_map.py writes (see its notes
// for the level's files as the game reads them), from the player's own game files, into
// %LOCALAPPDATA%\KCDS\maps\kcds_map_<name>\ (level.pak, recast.pak, map.json). The game folder is
// never written. Node's own zlib only: the game's packs are zip files (deflate or stored).

const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');

const NAME = /^[a-z0-9_]{1,32}$/;
const SIZES = Object.freeze([128, 256, 512, 1024, 2048]);
const SECTOR_SIZE = 64;
const LOD_LEVELS = 6;
const TEXTURE_SECTOR_SIZE = 256;
const TEXTURE_LAYER_FORMAT = 0x16;
const GROUND_NORMAL = [128, 255, 128];
const FLAT_STEP = 0.05;
// The ground materials every sector carries (klaster's surface types), the first covering the map.
const GROUND_MATERIALS = Object.freeze({
  grass: 18,
  meadow: 8,
  pasture: 29,
  trampled: 16,
  soil: 14,
  mud: 6,
  stony_mud: 34,
  sand: 10,
  gravel: 3,
  forest: 4,
  forest_leaves: 12,
  cobblestones: 37,
  paving: 22,
  cobblestones_grass: 38,
  river_shore: 15,
});
const FAR_COLOURS = Object.freeze({
  grass: [76, 88, 52],
  meadow: [80, 90, 54],
  pasture: [82, 88, 56],
  forest: [64, 62, 46],
  forest_leaves: [70, 62, 44],
  sand: [128, 116, 92],
});
const FAR_COLOUR_STONE = [104, 100, 92];
// terrain.dat's model tables (CTerrain::Load_T).
const VEGETATION_RECORD = 360;
const MATERIAL_AT = 336;
const BRUSH_RECORD = 256;
const HEAD_BYTES = 8 * 1024 * 1024;
// The navmesh (Warhorse's Detour version 14).
const NAV_HEAD = 60;
const NAV_MAGIC = 0x444e4156;
const NAV_VERSION = 14;
const NAV_TILE = 16;
const NAV_CELLS = 4;
const NAV_WALK = 1;
const NAV_BORDER = 0x8000;
const NAV_SIDE_PLUS_X = 0;
const NAV_SIDE_PLUS_Y = 2;
const NAV_SIDE_MINUS_X = 4;
const NAV_SIDE_MINUS_Y = 6;
const NAV_WALKABLE_HEIGHT = 1.76;
const NAV_WALKABLE_RADIUS = 0.32;
const NAV_WALKABLE_CLIMB = 0.32;
const NAV_FOURTH = 0.8;
const NAV_QUANT = 12.5;
const NAV_BOX_MARGIN = 0.5;
const STEAM_GAME = 'KingdomComeDeliverance2';

// ---- zip ----------------------------------------------------------------------------------------

// The entries of a zip file: { name (forward slashes, lower case), method, offset, compressed,
// size }.
function zipEntries(file) {
  const fd = fs.openSync(file, 'r');
  try {
    const length = fs.fstatSync(fd).size;
    const tail = Buffer.alloc(Math.min(length, 65557));
    fs.readSync(fd, tail, 0, tail.length, length - tail.length);
    const end = tail.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
    if (end < 0) {
      throw new Error(`${file} is not a zip file`);
    }
    let count = tail.readUInt16LE(end + 10);
    let directorySize = tail.readUInt32LE(end + 12);
    let directoryAt = tail.readUInt32LE(end + 16);
    // Zip64 (a pack over 4 GB or with many files): its own end record before this one.
    if (directoryAt === 0xffffffff || count === 0xffff) {
      const locator = tail.lastIndexOf(Buffer.from([0x50, 0x4b, 0x06, 0x07]), end);
      const recordAt = Number(tail.readBigUInt64LE(locator + 8));
      const record = Buffer.alloc(56);
      fs.readSync(fd, record, 0, 56, recordAt);
      count = Number(record.readBigUInt64LE(32));
      directorySize = Number(record.readBigUInt64LE(40));
      directoryAt = Number(record.readBigUInt64LE(48));
    }
    const directory = Buffer.alloc(directorySize);
    fs.readSync(fd, directory, 0, directorySize, directoryAt);
    const entries = new Map();
    let at = 0;
    for (let index = 0; index < count; index += 1) {
      const method = directory.readUInt16LE(at + 10);
      let compressed = directory.readUInt32LE(at + 20);
      let size = directory.readUInt32LE(at + 24);
      const nameLength = directory.readUInt16LE(at + 28);
      const extraLength = directory.readUInt16LE(at + 30);
      const commentLength = directory.readUInt16LE(at + 32);
      let offset = directory.readUInt32LE(at + 42);
      const name = directory.toString('latin1', at + 46, at + 46 + nameLength);
      // Zip64 sizes and offset in the extra field.
      let extra = at + 46 + nameLength;
      const extraEnd = extra + extraLength;
      while (extra + 4 <= extraEnd) {
        const tag = directory.readUInt16LE(extra);
        const dataLength = directory.readUInt16LE(extra + 2);
        if (tag === 0x0001) {
          let field = extra + 4;
          if (size === 0xffffffff) {
            size = Number(directory.readBigUInt64LE(field));
            field += 8;
          }
          if (compressed === 0xffffffff) {
            compressed = Number(directory.readBigUInt64LE(field));
            field += 8;
          }
          if (offset === 0xffffffff) {
            offset = Number(directory.readBigUInt64LE(field));
          }
        }
        extra += 4 + dataLength;
      }
      entries.set(name.replace(/\\/g, '/').toLowerCase(), {
        name,
        method,
        offset,
        compressed,
        size,
      });
      at += 46 + nameLength + extraLength + commentLength;
    }
    return entries;
  } finally {
    fs.closeSync(fd);
  }
}

// An entry's bytes, its first `most` only when given.
function zipRead(file, entries, name, most = -1) {
  const entry = entries.get(name.toLowerCase());
  if (!entry) {
    throw new Error(`${path.basename(file)} has no ${name}`);
  }
  const fd = fs.openSync(file, 'r');
  try {
    const head = Buffer.alloc(30);
    fs.readSync(fd, head, 0, 30, entry.offset);
    const dataAt = entry.offset + 30 + head.readUInt16LE(26) + head.readUInt16LE(28);
    // Only the head when `most` is given: deflate never makes data much smaller than a block, so
    // `most` compressed bytes and a little more hold at least `most` inflated ones.
    const wanted = most >= 0 ? Math.min(entry.compressed, most + 65536) : entry.compressed;
    const data = Buffer.alloc(wanted);
    fs.readSync(fd, data, 0, wanted, dataAt);
    if (entry.method === 0) {
      return most >= 0 ? data.subarray(0, most) : data;
    }
    if (entry.method !== 8) {
      throw new Error(`${name}: zip method ${entry.method} is not read`);
    }
    if (most < 0) {
      return zlib.inflateRawSync(data);
    }
    const inflated = zlib.inflateRawSync(data, { finishFlush: zlib.constants.Z_SYNC_FLUSH });
    return inflated.subarray(0, most);
  } finally {
    fs.closeSync(fd);
  }
}

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n += 1) {
    let c = n;
    for (let k = 0; k < 8; k += 1) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(data) {
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; i += 1) {
    crc = CRC_TABLE[(crc ^ data[i]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// A zip file of the files (name -> Buffer or string), deflated as CryPak reads them.
function zipWrite(file, files) {
  const locals = [];
  const central = [];
  let offset = 0;
  for (const [name, content] of Object.entries(files)) {
    const data = Buffer.isBuffer(content) ? content : Buffer.from(content, 'utf8');
    const packed = zlib.deflateRawSync(data, { level: 9 });
    const nameBytes = Buffer.from(name, 'utf8');
    const crc = crc32(data);
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0, 6);
    local.writeUInt16LE(8, 8);
    local.writeUInt32LE(0, 10);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(packed.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(nameBytes.length, 26);
    local.writeUInt16LE(0, 28);
    const entry = Buffer.alloc(46);
    entry.writeUInt32LE(0x02014b50, 0);
    entry.writeUInt16LE(20, 4);
    entry.writeUInt16LE(20, 6);
    entry.writeUInt16LE(0, 8);
    entry.writeUInt16LE(8, 10);
    entry.writeUInt32LE(0, 12);
    entry.writeUInt32LE(crc, 16);
    entry.writeUInt32LE(packed.length, 20);
    entry.writeUInt32LE(data.length, 24);
    entry.writeUInt16LE(nameBytes.length, 28);
    entry.writeUInt32LE(offset, 42);
    locals.push(local, nameBytes, packed);
    central.push(entry, nameBytes);
    offset += 30 + nameBytes.length + packed.length;
  }
  const directory = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(central.length / 2, 8);
  end.writeUInt16LE(central.length / 2, 10);
  end.writeUInt32LE(directory.length, 12);
  end.writeUInt32LE(offset, 16);
  fs.writeFileSync(file, Buffer.concat([...locals, directory, end]));
}

// ---- the map ------------------------------------------------------------------------------------

// klaster's level, level data, action data and mission files, renamed and emptied.
function levelDescription(pack, entries, level) {
  const text = (name) => zipRead(pack, entries, name).toString('utf8');
  const files = {};
  files['levelinfo.xml'] = text('levelinfo.xml').replace(
    'Name="data/levels/klaster"',
    `Name="data/levels/${level}"`,
  );
  files['leveldata.xml'] = text('leveldata.xml')
    .replace('Name="klaster"', `Name="${level}"`)
    .replace(/<Layers>[\s\S]*?<\/Layers>/, '<Layers />')
    .replace(/<Vegetation>[\s\S]*?<\/Vegetation>/, '<Vegetation />');
  files['leveldataaction.xml'] = text('leveldataaction.xml')
    .replace(/CGFCount="\d+"/g, 'CGFCount="0"')
    .replace(/ProgressBarRange="\d+"/g, 'ProgressBarRange="1"');
  files['mission_mission0.xml'] = text('mission_mission0.xml').replace(
    /CGFCount="\d+"/g,
    'CGFCount="0"',
  );
  files['objects_mission0.xml'] = '<Objects>\n</Objects>\n';
  for (const name of [
    'levelprototypes.xml',
    'serialize.xml',
    'lensflarelist.xml',
    'dyntexsrclayeract.xml',
    'shaderslist.txt',
  ]) {
    files[name] = zipRead(pack, entries, name);
  }
  files['brushlist.txt'] = '';
  for (const [key, entry] of entries) {
    if (key.startsWith('tables/')) {
      files[entry.name.replace(/\\/g, '/')] = zipRead(pack, entries, key);
    }
  }
  return files;
}

// The sectors' surface palette, `first` covering the map, padded to whole words.
function palette(first) {
  const ids = [
    GROUND_MATERIALS[first],
    ...Object.entries(GROUND_MATERIALS)
      .filter(([name]) => name !== first)
      .map(([, id]) => id),
  ];
  const data = Buffer.alloc(Math.ceil(ids.length / 4) * 4);
  ids.forEach((id, index) => data.writeUInt8(id, index));
  return { data, length: ids.length };
}

// One terrain node and its children, flat at `height` (CTerrainNode load).
function terrainNode(x, y, size, height, ground) {
  const leaf = size === SECTOR_SIZE;
  const parts = [];
  let words = Buffer.alloc(0);
  let base = 0;
  let step = 0;
  let side = 0;
  if (leaf) {
    side = SECTOR_SIZE + 1;
    words = Buffer.alloc(4 * side * side);
    base = height;
    step = FLAT_STEP;
  } else {
    const half = size / 2;
    for (const [cx, cy] of [
      [x, y],
      [x + half, y],
      [x, y + half],
      [x + half, y + half],
    ]) {
      parts.push(terrainNode(cx, cy, half, height, ground));
    }
  }
  const head = Buffer.alloc(48);
  head.writeInt16LE(8, 0);
  head.writeUInt8(0, 2);
  [x, y, height, x + size, y + size, height].forEach((value, index) =>
    head.writeFloatLE(value, 4 + 4 * index),
  );
  head.writeFloatLE(base, 28);
  head.writeFloatLE(step, 32);
  head.writeInt16LE(side, 36);
  head.writeInt32LE(ground.palette.length, 40);
  const errors = Buffer.alloc(4 * LOD_LEVELS);
  return Buffer.concat([head.subarray(0, 44), words, errors, ground.palette.data, ...parts]);
}

// klaster's plant kinds as terrain.dat's model tables (CTerrain::Load_T reads them first).
function plantKinds(game) {
  const pack = path.join(game, 'Data', 'Levels', 'klaster', 'terrain.pak');
  const entries = zipEntries(pack);
  const head = zipRead(pack, entries, 'terrain/terrain.dat', HEAD_BYTES);
  let at = 32;
  const kinds = head.readInt32LE(at);
  at += 4;
  const records = head.subarray(at, at + VEGETATION_RECORD * kinds);
  at += VEGETATION_RECORD * kinds;
  const brushes = head.readInt32LE(at);
  at += 4 + BRUSH_RECORD * brushes;
  const materials = head.readInt32LE(at);
  at += 4;
  let used = 0;
  for (let kind = 0; kind < kinds; kind += 1) {
    used = Math.max(used, 1 + records.readInt32LE(VEGETATION_RECORD * kind + MATERIAL_AT));
  }
  used = Math.max(0, Math.min(used, materials));
  const counts = Buffer.alloc(12);
  counts.writeInt32LE(kinds, 0);
  return Buffer.concat([
    counts.subarray(0, 4),
    records,
    Buffer.alloc(4),
    (() => {
      const count = Buffer.alloc(4);
      count.writeInt32LE(used, 0);
      return count;
    })(),
    head.subarray(at, at + BRUSH_RECORD * used),
  ]);
}

// terrain\terrain.dat (CTerrain::Load_T).
function terrain(size, height, kinds, ground) {
  const tail = Buffer.alloc(32);
  tail.writeInt16LE(8, 0);
  tail.writeInt16LE(0, 2);
  [0, 0, 0, size, size, size].forEach((value, index) => tail.writeFloatLE(value, 4 + 4 * index));
  tail.writeInt32LE(0, 28);
  const body = Buffer.concat([kinds, terrainNode(0, 0, size, height, ground), tail]);
  const header = Buffer.alloc(32);
  header.writeUInt8(29, 0);
  header.writeUInt8(0, 1);
  header.writeUInt8(6, 2);
  header.writeUInt8(1, 3);
  header.writeInt32LE(32 + body.length, 4);
  header.writeInt32LE(size, 8);
  header.writeFloatLE(1.0, 12);
  header.writeInt32LE(SECTOR_SIZE, 16);
  header.writeInt32LE(size / SECTOR_SIZE, 20);
  header.writeFloatLE(0.003814697265625, 24);
  header.writeFloatLE(0.0, 28);
  return Buffer.concat([header, body]);
}

function nodeIndices(size, counter) {
  const index = counter.next;
  counter.next += 1;
  if (size === SECTOR_SIZE) {
    return [index, -1, -1, -1, -1];
  }
  let out = [index];
  for (let child = 0; child < 4; child += 1) {
    out = out.concat(nodeIndices(size / 2, counter));
  }
  return out;
}

function bc1Solid([red, green, blue], size) {
  const packed =
    (Math.floor((red * 31) / 255) << 11) |
    (Math.floor((green * 63) / 255) << 5) |
    Math.floor((blue * 31) / 255);
  const block = Buffer.alloc(8);
  block.writeUInt16LE(packed, 0);
  block.writeUInt16LE(packed, 2);
  return Buffer.concat(Array.from({ length: (size / 4) * (size / 4) }, () => block));
}

// terrain\cover.ctc: the ground's colour from afar and its normal, one sector per node.
function groundTexture(size, colour) {
  const layer = Buffer.alloc(12);
  layer.writeUInt16LE(TEXTURE_SECTOR_SIZE, 0);
  layer.writeUInt16LE(0, 2);
  layer.writeUInt8(TEXTURE_LAYER_FORMAT, 4);
  layer.writeInt32LE((TEXTURE_SECTOR_SIZE * TEXTURE_SECTOR_SIZE) / 2, 8);
  const head = Buffer.alloc(16);
  head.write('CRY\0', 0, 'latin1');
  head.writeUInt8(100, 4);
  head.writeUInt8(0, 5);
  head.writeUInt16LE(9, 6);
  head.writeUInt16LE(2, 8);
  head.writeUInt16LE(0, 10);
  head.writeFloatLE(1.0, 12);
  const counter = { next: 0 };
  const indices = nodeIndices(size, counter);
  const list = Buffer.alloc(2 + 2 * indices.length);
  list.writeUInt16LE(indices.length, 0);
  indices.forEach((value, index) => list.writeInt16LE(value, 2 + 2 * index));
  const sector = Buffer.concat([
    bc1Solid(colour, TEXTURE_SECTOR_SIZE),
    bc1Solid(GROUND_NORMAL, TEXTURE_SECTOR_SIZE),
  ]);
  return Buffer.concat([
    head,
    layer,
    layer,
    list,
    ...Array.from({ length: counter.next }, () => sector),
  ]);
}

// One 16 m navmesh tile of NAV_CELLS x NAV_CELLS walkable quads at the ground's height.
function navTile(tileX, tileY, height) {
  const cells = NAV_CELLS;
  const step = NAV_TILE / cells;
  const left = tileX * NAV_TILE;
  const bottom = tileY * NAV_TILE;
  const vertices = [];
  for (let j = 0; j <= cells; j += 1) {
    for (let i = 0; i <= cells; i += 1) {
      vertices.push([left + i * step, bottom + j * step, height]);
    }
  }
  const polys = [];
  for (let j = 0; j < cells; j += 1) {
    for (let i = 0; i < cells; i += 1) {
      const corner = j * (cells + 1) + i;
      const ring = [corner, corner + 1, corner + 1 + cells + 1, corner + cells + 1];
      const neighbours = [
        j > 0 ? (j - 1) * cells + i + 1 : NAV_BORDER | NAV_SIDE_MINUS_Y,
        i < cells - 1 ? j * cells + i + 2 : NAV_BORDER | NAV_SIDE_PLUS_X,
        j < cells - 1 ? (j + 1) * cells + i + 1 : NAV_BORDER | NAV_SIDE_PLUS_Y,
        i > 0 ? j * cells + i : NAV_BORDER | NAV_SIDE_MINUS_X,
      ];
      const poly = Buffer.alloc(28);
      [...ring, 0, 0, ...neighbours, 0, 0].forEach((value, index) =>
        poly.writeUInt16LE(value, 2 * index),
      );
      poly.writeUInt8(NAV_WALK, 24);
      poly.writeUInt8(4, 25);
      poly.writeUInt8(0, 26);
      poly.writeUInt8(1, 27);
      polys.push(poly);
    }
  }
  const polyCount = cells * cells;
  const header = Buffer.alloc(112);
  [
    NAV_MAGIC,
    NAV_VERSION,
    tileX,
    tileY,
    0,
    0,
    polyCount,
    vertices.length,
    0,
    polyCount * 4 * 2,
    polyCount,
    0,
    0,
    0,
    0,
    0,
    polyCount,
  ].forEach((value, index) => header.writeInt32LE(value, 4 * index));
  [
    NAV_WALKABLE_HEIGHT,
    NAV_WALKABLE_RADIUS,
    NAV_WALKABLE_CLIMB,
    NAV_FOURTH,
    left,
    bottom,
    height - NAV_BOX_MARGIN,
    left + NAV_TILE,
    bottom + NAV_TILE,
    height + NAV_BOX_MARGIN,
    NAV_QUANT,
  ].forEach((value, index) => header.writeFloatLE(value, 68 + 4 * index));
  const body = Buffer.alloc(12 * vertices.length);
  vertices.forEach((vertex, index) => {
    vertex.forEach((value, axis) => body.writeFloatLE(value, 12 * index + 4 * axis));
  });
  return Buffer.concat([header, body, ...polys]);
}

// recast/rdnavmesh_1.hdr and .nav for the whole map.
function navmesh(size, height, game) {
  const pack = path.join(game, 'Data', 'Levels', 'klaster', 'recast.pak');
  const entries = zipEntries(pack);
  const config = zipRead(pack, entries, 'recast/rdnavmesh_1.nav', NAV_HEAD).subarray(
    0,
    NAV_HEAD - 4,
  );
  const tiles = size / NAV_TILE;
  const count = Buffer.alloc(4);
  count.writeInt32LE(tiles * tiles, 0);
  const nav = [config, count];
  let length = config.length + 4;
  const index = [];
  for (let tileY = 0; tileY < tiles; tileY += 1) {
    for (let tileX = 0; tileX < tiles; tileX += 1) {
      const tile = navTile(tileX, tileY, height);
      const record = Buffer.alloc(20);
      [tileX, tileY, length, 1, tile.length].forEach((value, i) =>
        record.writeInt32LE(value, 4 * i),
      );
      index.push(record);
      nav.push(tile);
      length += tile.length;
    }
  }
  return { index: Buffer.concat(index), nav: Buffer.concat(nav) };
}

// The game's folder: KCDS_GAME_FOLDER, else Steam's libraries, else Steam's default one.
function findGame() {
  const candidates = [];
  if (process.env.KCDS_GAME_FOLDER) {
    candidates.push(process.env.KCDS_GAME_FOLDER);
  }
  const steam = path.join(process.env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)', 'Steam');
  try {
    const libraries = fs.readFileSync(path.join(steam, 'steamapps', 'libraryfolders.vdf'), 'utf8');
    for (const match of libraries.matchAll(/"path"\s+"([^"]+)"/g)) {
      candidates.push(
        path.join(match[1].replace(/\\\\/g, '\\'), 'steamapps', 'common', STEAM_GAME),
      );
    }
  } catch {
    // No Steam library list: the default folder only.
  }
  candidates.push(path.join(steam, 'steamapps', 'common', STEAM_GAME));
  const found = candidates.find((folder) =>
    fs.existsSync(path.join(folder, 'Data', 'Levels', 'klaster', 'level.pak')),
  );
  if (!found) {
    throw new Error('the game folder was not found (set KCDS_GAME_FOLDER)');
  }
  return found;
}

function mapsFolder() {
  const local =
    process.env.LOCALAPPDATA ?? path.join(process.env.USERPROFILE ?? '', 'AppData', 'Local');
  return path.join(local, 'KCDS', 'maps');
}

// Makes the map; answers { level, size, height, ground, folder, bytes }.
function makeCustomMap({
  name,
  size = 1024,
  height = 32,
  ground = 'grass',
  navmesh: walkable = true,
  game,
  out,
} = {}) {
  if (typeof name !== 'string' || !NAME.test(name)) {
    throw new Error('the name is 1 to 32 lower-case letters, digits or _');
  }
  if (!SIZES.includes(size)) {
    throw new Error(`the size is ${SIZES.join(', ')} metres`);
  }
  if (typeof height !== 'number' || !Number.isFinite(height) || height < 0 || height > 1000) {
    throw new Error('the height is 0 to 1000 metres');
  }
  if (!Object.hasOwn(GROUND_MATERIALS, ground)) {
    throw new Error(`the ground is one of ${Object.keys(GROUND_MATERIALS).join(', ')}`);
  }
  const level = `kcds_map_${name}`;
  const folder = path.join(out ?? mapsFolder(), level);
  // A map the game has listed is not made again under it (switching to one remade while the game
  // ran never finished loading): a new name, or its folder deleted first.
  if (fs.existsSync(path.join(folder, 'level.pak'))) {
    throw new Error(`${level} already exists on this PC: choose another name`);
  }
  const gameFolder = game ?? findGame();
  const pack = path.join(gameFolder, 'Data', 'Levels', 'klaster', 'level.pak');
  const files = levelDescription(pack, zipEntries(pack), level);
  const groundInfo = { palette: palette(ground) };
  files['terrain/cover.ctc'] = groundTexture(size, FAR_COLOURS[ground] ?? FAR_COLOUR_STONE);
  files['terrain/terrain.dat'] = terrain(size, height, plantKinds(gameFolder), groundInfo);
  const indoor = Buffer.alloc(20);
  [6, 20, 0, 0, 0].forEach((value, index) => indoor.writeInt32LE(value, 4 * index));
  files['terrain/indoor.dat'] = indoor;
  files['terrain/hlods.xml'] =
    '<HLod DataOffset="4" DataSize="4" ProxyIndex="-1" Type="Cluster" Radius="8000" ' +
    'NearestObserverDistance="0" Name="root" Hash="214082742" />\n';
  const hlods = Buffer.alloc(8);
  hlods.writeInt32LE(2, 0);
  files['terrain/hlods.dat'] = hlods;
  fs.mkdirSync(folder, { recursive: true });
  const levelPack = path.join(folder, 'level.pak');
  zipWrite(levelPack, files);
  const recast = path.join(folder, 'recast.pak');
  if (walkable) {
    const navigation = navmesh(size, height, gameFolder);
    zipWrite(recast, {
      'Recast/RDNavmesh_1.hdr': navigation.index,
      'Recast/RDNavmesh_1.nav': navigation.nav,
    });
  } else if (fs.existsSync(recast)) {
    fs.rmSync(recast);
  }
  fs.writeFileSync(
    path.join(folder, 'map.json'),
    `{"name": "${level}", "size": ${size}, "height": ${height}, "ground": "${ground}"}\n`,
  );
  return { level, size, height, ground, folder, bytes: fs.statSync(levelPack).size };
}

module.exports = {
  makeCustomMap,
  GROUND_MATERIALS,
  SIZES,
  zipEntries,
  zipRead,
  zipWrite,
  crc32,
  findGame,
};
