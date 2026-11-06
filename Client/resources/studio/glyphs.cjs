'use strict';

// The Asset/Animation Studio's content tiles' pictures: line drawings centred on
// (0, 0), about 70 pixels across, as NativeClip.draw commands, in the style of World Architect's
// (Client/resources/architect/glyphs.cjs), whose pictures the studio also uses.

const { GLYPHS: ARCHITECT_GLYPHS } = require('../architect/glyphs.cjs');

const LINE = 0x8d949c;

function path(points) {
  const commands = [['moveTo', points[0][0], points[0][1]]];
  for (const [x, y] of points.slice(1)) {
    commands.push(['lineTo', x, y]);
  }
  return commands;
}

function glyph(...paths) {
  return [['clear'], ['lineStyle', 2, LINE, 100], ...paths.flat()];
}

// A wave from (x0, y0) to (x1, y1), `turns` times across, `height` pixels high.
function wave(x0, y0, x1, y1, turns, height) {
  const points = [];
  const steps = 24;
  for (let index = 0; index <= steps; index += 1) {
    const share = index / steps;
    const lift = Math.sin(share * turns * Math.PI * 2) * height;
    points.push([x0 + (x1 - x0) * share, y0 + (y1 - y0) * share + lift]);
  }
  return path(points);
}

// A chain's links: small rings along a line.
function links(count) {
  const commands = [];
  for (let index = 0; index < count; index += 1) {
    const x = -30 + index * 12;
    const y = -24 + index * 9;
    commands.push(
      ...path([
        [x - 5, y - 3],
        [x + 5, y - 3],
        [x + 5, y + 3],
        [x - 5, y + 3],
        [x - 5, y - 3],
      ]),
    );
  }
  return commands;
}

// An animation key: a diamond on a time line.
function key() {
  return [
    ...path([
      [-32, 0],
      [32, 0],
    ]),
    ...path([
      [0, -18],
      [16, 0],
      [0, 18],
      [-16, 0],
      [0, -18],
    ]),
  ];
}

// "Add a key": a plus.
function plus() {
  return [
    ...path([
      [-18, 0],
      [18, 0],
    ]),
    ...path([
      [0, -18],
      [0, 18],
    ]),
  ];
}

const GLYPHS = Object.freeze({
  ...ARCHITECT_GLYPHS,
  key: glyph(key()),
  addKey: glyph(plus()),
  rope: glyph(wave(-30, -28, 30, 28, 1.5, 6)),
  whip: glyph(
    path([
      [-32, 26],
      [-20, 14],
    ]),
    wave(-20, 14, 32, -26, 1, 8),
  ),
  chain: glyph(links(6)),
});

module.exports = { GLYPHS };
