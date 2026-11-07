'use strict';

// World Architect's review for AI assistants: the MCP server measures each picture's light and
// colour (Dev/tools/architect-mcp/look/picture_review.cjs) and says what a player would notice.
const test = require('node:test');
const assert = require('node:assert/strict');
const zlib = require('node:zlib');

const { measure, verdict } = require('../../../../Dev/tools/architect-mcp/look/picture_review.cjs');

// An RGB PNG of width x height, coloured by colour(x, y) -> [r, g, b] (0 to 255).
function png(width, height, colour) {
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
    for (let x = 0; x < width; x += 1) {
      const [r, g, b] = colour(x, y);
      row[1 + x * 3] = r;
      row[2 + x * 3] = g;
      row[3 + x * 3] = b;
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

// A stone-like texture: warm, lit, with detail in every block.
const warmStone = (x, y) => {
  const grain = ((x * 37 + y * 91) % 23) * 3;
  return [150 + grain, 110 + grain, 80 + grain];
};

test('a dark room lit by the sky is too dark and blue', () => {
  const measured = measure(png(64, 48, (x, y) => [8, 14 + ((x + y) % 5), 30 + ((x * y) % 7)]));
  assert.equal(measured.cast, 'blue');
  assert.ok(measured.brightness < 0.12);
  const said = verdict(measured, { indoors: true }).join(' ');
  assert.match(said, /Too dark/);
  assert.match(said, /blue cast: cold sky or moon light fills the room/);
});

test('a warm, textured, lit room looks plausible', () => {
  const measured = measure(png(64, 48, warmStone));
  assert.equal(measured.cast, 'warm');
  assert.ok(measured.brightness > 0.3 && measured.brightness < 0.7);
  assert.deepEqual(verdict(measured, { indoors: true }), [
    `Light and colour look plausible (brightness ${measured.brightness}, warm).`,
  ]);
});

test('flat black areas and washed-out light are named', () => {
  const holes = measure(png(64, 64, (x, y) => (x < 32 ? [0, 0, 0] : warmStone(x, y))));
  assert.ok(holes.black >= 0.45);
  assert.match(verdict(holes).join(' '), /flat black areas/);
  const white = measure(png(32, 32, (x, y) => [250, 250, 248 - ((x + y) % 3)]));
  assert.match(verdict(white).join(' '), /Washed out/);
});

test('base64 text as the editor answers is measured as the bytes are', () => {
  const picture = png(16, 16, warmStone);
  assert.deepEqual(measure(picture.toString('base64')), measure(picture));
  assert.throws(() => measure('bm90IGEgcG5n'), /not a PNG/);
});
