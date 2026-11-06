'use strict';

// The content tiles' icons (World Architect): line drawings in the style of the
// design's icons, centred on (0, 0) and about 70 pixels across, as NativeClip.draw commands.

const LINE = 0x8d949c;

function path(points, closed = false) {
  const commands = [['moveTo', points[0][0], points[0][1]]];
  for (const [x, y] of points.slice(1)) {
    commands.push(['lineTo', x, y]);
  }
  if (closed) {
    commands.push(['lineTo', points[0][0], points[0][1]]);
  }
  return commands;
}

function glyph(...paths) {
  return [['clear'], ['lineStyle', 2, LINE, 100], ...paths.flat()];
}

const GLYPHS = Object.freeze({
  house: glyph(
    path([
      [-34, -4],
      [0, -32],
      [34, -4],
    ]),
    path(
      [
        [-26, -10],
        [-26, 30],
        [26, 30],
        [26, -10],
      ],
      false,
    ),
    path([
      [-6, 30],
      [-6, 8],
      [6, 8],
      [6, 30],
    ]),
  ),
  structure: glyph(
    path(
      [
        [-32, 30],
        [-32, -14],
        [32, -14],
        [32, 30],
      ],
      true,
    ),
    path([
      [-32, 8],
      [32, 8],
    ]),
    path([
      [-10, -14],
      [-10, 8],
    ]),
    path([
      [12, 8],
      [12, 30],
    ]),
  ),
  door: glyph(
    path(
      [
        [-18, -32],
        [18, -32],
        [18, 32],
        [-18, 32],
      ],
      true,
    ),
    path([
      [8, 2],
      [10, 2],
    ]),
  ),
  tree: glyph(
    path(
      [
        [0, -34],
        [22, -6],
        [10, -6],
        [26, 18],
        [-26, 18],
        [-10, -6],
        [-22, -6],
      ],
      true,
    ),
    path([
      [0, 18],
      [0, 34],
    ]),
  ),
  bush: glyph(
    path(
      [
        [-30, 24],
        [-30, 6],
        [-16, -10],
        [0, -2],
        [14, -14],
        [30, 4],
        [30, 24],
      ],
      true,
    ),
  ),
  raise: glyph(
    path([
      [-34, 28],
      [-6, -18],
      [8, 4],
      [16, -6],
      [34, 28],
    ]),
    path([
      [0, -22],
      [0, -36],
    ]),
    path([
      [-6, -30],
      [0, -36],
      [6, -30],
    ]),
  ),
  lower: glyph(
    path([
      [-34, -10],
      [-12, -10],
      [0, 20],
      [12, -10],
      [34, -10],
    ]),
    path([
      [0, -36],
      [0, -18],
    ]),
    path([
      [-6, -24],
      [0, -18],
      [6, -24],
    ]),
  ),
  flatten: glyph(
    path([
      [-34, 20],
      [-20, 4],
      [20, 4],
      [34, 20],
    ]),
    path([
      [-20, -6],
      [20, -6],
    ]),
  ),
  clear: glyph(
    path(
      [
        [-30, 30],
        [-30, 12],
        [-16, -4],
        [0, 4],
        [14, -8],
        [30, 10],
        [30, 30],
      ],
      true,
    ),
    path([
      [14, -36],
      [34, -16],
    ]),
    path([
      [34, -36],
      [14, -16],
    ]),
  ),
  nature: glyph(
    path(
      [
        [-30, 30],
        [-30, 12],
        [-16, -4],
        [0, 4],
        [14, -8],
        [30, 10],
        [30, 30],
      ],
      true,
    ),
    path([
      [24, -38],
      [24, -14],
    ]),
    path([
      [12, -26],
      [36, -26],
    ]),
  ),
  wall: glyph(
    path(
      [
        [-32, -20],
        [32, -20],
        [32, 20],
        [-32, 20],
      ],
      true,
    ),
    path([
      [-32, 0],
      [32, 0],
    ]),
    path([
      [-8, -20],
      [-8, 0],
    ]),
    path([
      [12, 0],
      [12, 20],
    ]),
  ),
  floor: glyph(
    path(
      [
        [-34, 18],
        [-14, -6],
        [34, -6],
        [14, 18],
      ],
      true,
    ),
  ),
  ramp: glyph(
    path(
      [
        [-32, 24],
        [32, -18],
        [32, 24],
      ],
      true,
    ),
  ),
  box: glyph(
    path(
      [
        [-26, -12],
        [0, -26],
        [26, -12],
        [26, 18],
        [0, 32],
        [-26, 18],
      ],
      true,
    ),
    path([
      [-26, -12],
      [0, 2],
      [26, -12],
    ]),
    path([
      [0, 2],
      [0, 32],
    ]),
  ),
});

module.exports = { GLYPHS };
