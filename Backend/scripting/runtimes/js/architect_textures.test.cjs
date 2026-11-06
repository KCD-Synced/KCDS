'use strict';

// World Architect's MCP texture tool (Dev/tools/architect-mcp/textures.cjs): a picture into a
// game texture (DDS with its mip levels) and a material, in the author's assets folder.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');

const textures = require('../../../../Dev/tools/architect-mcp/textures.cjs');

// A PNG of width x height, RGB, each row with the given filter (0 none, 1 left, 2 up).
function png(width, height, colour, filter = 0) {
  const chunk = (type, body) => {
    const head = Buffer.alloc(8);
    head.writeUInt32BE(body.length, 0);
    head.write(type, 4, 'latin1');
    return Buffer.concat([head, body, Buffer.alloc(4)]);
  };
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0);
  header.writeUInt32BE(height, 4);
  header[8] = 8;
  header[9] = 2;
  const rows = [];
  for (let y = 0; y < height; y += 1) {
    const row = Buffer.alloc(1 + width * 3);
    row[0] = filter;
    for (let x = 0; x < width; x += 1) {
      const [r, g, b] = colour(x, y);
      const left = filter === 1 && x > 0 ? colour(x - 1, y) : [0, 0, 0];
      const up = filter === 2 && y > 0 ? colour(x, y - 1) : [0, 0, 0];
      row[1 + x * 3] = (r - left[0] - up[0]) & 0xff;
      row[2 + x * 3] = (g - left[1] - up[1]) & 0xff;
      row[3 + x * 3] = (b - left[2] - up[2]) & 0xff;
    }
    rows.push(row);
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', header),
    chunk('IDAT', zlib.deflateSync(Buffer.concat(rows))),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

test('a PNG is read with its row filters undone', () => {
  const colour = (x, y) => [x * 10, y * 20, 200];
  for (const filter of [0, 1, 2]) {
    const picture = textures.decodePng(png(5, 4, colour, filter));
    assert.equal(picture.width, 5);
    assert.equal(picture.height, 4);
    const at = (3 * 5 + 4) * 4;
    assert.deepEqual([...picture.rgba.subarray(at, at + 4)], [40, 60, 200, 255]);
  }
  assert.throws(() => textures.decodePng(Buffer.from('not a png')), /not a PNG/);
});

test('a picture becomes a power-of-two texture with every mip level and a material drawing it', () => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-textures-'));
  const image = path.join(folder, 'wall.png');
  fs.writeFileSync(
    image,
    png(300, 140, (x, y) => [x % 256, y, 50]),
  );
  const assets = path.join(folder, 'assets');
  const made = textures.makeTexture(
    { image, name: 'old_wall', folder: 'praha', surface: 'plaster', tile: 2 },
    assets,
  );
  assert.equal(made.material, 'objects/kcds/praha/old_wall');
  assert.equal(made.texture, 'objects/kcds/praha/textures/old_wall_diff.dds');
  assert.deepEqual(made.size, { width: 256, height: 128 });
  assert.deepEqual(made.from, { width: 300, height: 140 });
  const dds = fs.readFileSync(path.join(assets, made.texture));
  assert.equal(dds.toString('latin1', 0, 4), 'DDS ');
  assert.equal(dds.readUInt32LE(12), 128);
  assert.equal(dds.readUInt32LE(16), 256);
  // 256 x 128 down to 1 x 1: nine levels, four bytes a pixel.
  assert.equal(dds.readUInt32LE(28), 9);
  let pixels = 0;
  for (let level = 0; level < 9; level += 1) {
    pixels += Math.max(1, 256 >> level) * Math.max(1, 128 >> level);
  }
  assert.equal(dds.length, 148 + pixels * 4);
  // sRGB as the game's own textures: a DX10 header (B8G8R8A8_UNORM_SRGB), the game's mark.
  assert.equal(dds.toString('latin1', 84, 88), 'DX10');
  assert.equal(dds.readUInt32LE(128), 91);
  assert.equal(dds.toString('latin1', 124, 128), 'FYRC');
  assert.equal(dds.readUInt32LE(36), 0x800);
  const mtl = fs.readFileSync(path.join(assets, `${made.material}.mtl`), 'utf8');
  assert.match(mtl, /Shader="Illum"/);
  assert.match(mtl, /SurfaceType="mat_stone"/);
  assert.match(mtl, /File="objects\/kcds\/praha\/textures\/old_wall_diff.dds"/);
  assert.match(mtl, /TileU="2" TileV="2"/);
  fs.rmSync(folder, { recursive: true, force: true });
});

test('a bad name, a missing picture or an unknown surface is refused before anything is written', () => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-textures-'));
  const image = path.join(folder, 'wall.png');
  fs.writeFileSync(
    image,
    png(8, 8, () => [1, 2, 3]),
  );
  assert.throws(() => textures.makeTexture({ image, name: 'Bad Name' }, folder), /lower-case/);
  assert.throws(
    () => textures.makeTexture({ image: 'C:/nope.png', name: 'a' }, folder),
    /no picture/,
  );
  assert.throws(
    () => textures.makeTexture({ image, name: 'a', surface: 'glass' }, folder),
    /surface is one of/,
  );
  assert.equal(fs.existsSync(path.join(folder, 'objects')), false);
  fs.rmSync(folder, { recursive: true, force: true });
});
