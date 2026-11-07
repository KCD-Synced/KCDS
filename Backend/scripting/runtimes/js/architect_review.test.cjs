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

test("a room lit by the sky alone is dim and blue, and told the game's room light", () => {
  const measured = measure(png(64, 48, (x, y) => [8, 14 + ((x + y) % 5), 30 + ((x * y) % 7)]));
  assert.equal(measured.cast, 'blue');
  assert.ok(measured.brightness < 0.1);
  const said = verdict(measured, { indoors: true }).join(' ');
  assert.match(said, /^Dim/);
  assert.match(said, /blue cast: cold sky light fills the room/);
  assert.match(said, /room light \(light preset room, ambient: true/);
});

test('a warm, textured, lit room looks plausible', () => {
  const measured = measure(png(64, 48, warmStone));
  assert.equal(measured.cast, 'warm');
  assert.ok(measured.brightness > 0.3 && measured.brightness < 0.7);
  assert.deepEqual(verdict(measured, { indoors: true }), [
    `Light and colour look plausible (brightness ${measured.brightness}, warm).`,
  ]);
});

test("a dark, warm, textured room as the game's own is only called dim; a black one too dark", () => {
  // The game's rooms read about 0.03 to 0.25 (calibrated on 52 views of 13 of Kuttenberg's).
  const dim = (x, y) => {
    const [r, g, b] = warmStone(x, y);
    return [Math.round(r * 0.18), Math.round(g * 0.18), Math.round(b * 0.18)];
  };
  const room = measure(png(64, 64, (x, y) => (y < 20 && x < 20 ? [0, 0, 0] : dim(x, y))));
  const said = verdict(room, { indoors: true });
  assert.equal(said.length, 1, said.join(' | '));
  assert.match(said[0], /^Dim/);
  const black = measure(png(32, 32, (x, y) => [3, 3, 4 + ((x + y) % 2)]));
  assert.match(verdict(black, { indoors: true })[0], /^Too dark/);
});

test('a dim candle-lit plaster room seen from above is only called dim; a lit even one is not', () => {
  // A smooth warm gradient, as the game's own plaster wall under a candle reads (even blocks).
  const plaster = (x, y) => [34 + Math.floor((x + y) / 16), 18 + Math.floor(x / 24), 8];
  const room = measure(png(96, 64, plaster));
  assert.ok(room.brightness < 0.1);
  assert.ok(room.flat > 0.55 && room.upper.flat > 0.3);
  // The view from above looks down across the room: its upper part is no wall.
  const fromAbove = verdict(room, { indoors: true, walls: false });
  assert.equal(fromAbove.length, 1, fromAbove.join(' | '));
  assert.match(fromAbove[0], /^Dim/);
  // A lit room as even as that is still told it lacks texture.
  const lit = measure(png(96, 64, (x, y) => [150 + Math.floor((x + y) / 16), 140, 120]));
  assert.match(verdict(lit, { indoors: true, walls: false }).join(' '), /even, without texture/);
  assert.match(verdict(lit, { indoors: true }).join(' '), /walls and ceiling look plain/);
});

test('textured walls under warm light are not called plain; bare faces are', () => {
  // Walls as measured live at Kuttenberg (detail over brightness): the game's own plaster in an
  // opened house 0.087, a made plaster 0.063, both 0 % even; an opened shell's bare inside 0.025.
  const view = (detail, flat) => ({
    brightness: 0.4,
    dark: 0,
    bright: 0,
    contrast: 0.2,
    detail: 0.05,
    color: { r: 0.5, g: 0.4, b: 0.3 },
    cast: 'warm',
    black: 0,
    flat,
    upper: {
      brightness: 0.37,
      dark: 0,
      bright: 0,
      contrast: 0.2,
      detail: detail * 0.37,
      color: { r: 0.5, g: 0.4, b: 0.3 },
      cast: 'warm',
      black: 0,
      flat,
    },
  });
  const plain = (m) => verdict(m, { indoors: true }).some((line) => /look plain/.test(line));
  assert.equal(plain(view(0.087, 0)), false, "the game's plaster");
  assert.equal(plain(view(0.063, 0)), false, 'a made plaster');
  assert.equal(plain(view(0.025, 0.1)), true, 'a bare face');
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
