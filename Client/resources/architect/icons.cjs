'use strict';

// World Architect's small interface icons, traced from the design canvas's own icons
// (24 by 24 units, centre 12, 12) as line drawings for NativeClip.draw. icon() places one, scaled,
// in a colour.

// A circle as a closed line of points.
function circle(cx, cy, radius, from = 0, to = 360, steps = 20) {
  const points = [];
  for (let index = 0; index <= steps; index += 1) {
    const angle = ((from + ((to - from) * index) / steps) * Math.PI) / 180;
    points.push([cx + Math.cos(angle) * radius, cy + Math.sin(angle) * radius]);
  }
  return points;
}

// Each icon: lines of points; a line whose first point is 'fill' is drawn filled.
const ICONS = Object.freeze({
  select: [
    [
      [5, 3],
      [18, 10.5],
      [12.5, 12],
      [11, 18],
      [5, 3],
    ],
  ],
  move: [
    [
      [12, 3],
      [12, 21],
    ],
    [
      [3, 12],
      [21, 12],
    ],
    [
      [9, 6],
      [12, 3],
      [15, 6],
    ],
    [
      [9, 18],
      [12, 21],
      [15, 18],
    ],
    [
      [6, 9],
      [3, 12],
      [6, 15],
    ],
    [
      [18, 9],
      [21, 12],
      [18, 15],
    ],
  ],
  rotate: [
    circle(12, 12, 8, -40, 270),
    [
      [20, 4],
      [20, 9],
      [15, 9],
    ],
  ],
  scale: [
    [
      [4, 12],
      [12, 12],
      [12, 20],
      [4, 20],
      [4, 12],
    ],
    [
      [13, 11],
      [20, 4],
    ],
    [
      [14, 4],
      [20, 4],
      [20, 10],
    ],
  ],
  house: [
    [
      [3, 11],
      [12, 4],
      [21, 11],
    ],
    [
      [5, 10],
      [5, 20],
      [19, 20],
      [19, 10],
    ],
    [
      [10, 20],
      [10, 14],
      [14, 14],
      [14, 20],
    ],
  ],
  terrain: [
    [
      [2, 19],
      [9, 8],
      [13, 14],
      [16, 10],
      [22, 19],
      [2, 19],
    ],
  ],
  tree: [
    [
      [12, 3],
      [18, 11],
      [14.5, 11],
      [19, 17],
      [5, 17],
      [9.5, 11],
      [6, 11],
      [12, 3],
    ],
    [
      [12, 17],
      [12, 21],
    ],
  ],
  shield: [
    [
      [12, 3],
      [20, 6],
      [20, 12],
      [17, 17.5],
      [12, 21],
      [7, 17.5],
      [4, 12],
      [4, 6],
      [12, 3],
    ],
  ],
  map: [
    [
      [3, 6],
      [9, 4],
      [15, 6],
      [21, 4],
      [21, 18],
      [15, 20],
      [9, 18],
      [3, 20],
      [3, 6],
    ],
    [
      [9, 4],
      [9, 18],
    ],
    [
      [15, 6],
      [15, 20],
    ],
  ],
  undo: [
    [
      [9, 14],
      [4, 9],
      [9, 4],
    ],
    [[4, 9], [15, 9], ...circle(15, 14, 5, -90, 90, 8).slice(1), [12, 19]],
  ],
  redo: [
    [
      [15, 14],
      [20, 9],
      [15, 4],
    ],
    [[20, 9], [9, 9], ...circle(9, 14, 5, 270, 90, 8).slice(1), [12, 19]],
  ],
  play: [['fill', [7, 4], [20, 12], [7, 20], [7, 4]]],
  magnet: [
    [
      [6, 3],
      [6, 11],
      ...circle(12, 11, 6, 180, 0, 8).slice(1),
      [18, 3],
      [14, 3],
      [14, 11],
      ...circle(12, 11, 2, 0, 180, 6).slice(1),
      [10, 3],
      [6, 3],
    ],
  ],
  search: [
    circle(11, 11, 7),
    [
      [16, 16],
      [21, 21],
    ],
  ],
  filter: [
    [
      [3, 5],
      [21, 5],
      [14, 13],
      [14, 19],
      [10, 21],
      [10, 13],
      [3, 5],
    ],
  ],
  down: [
    [
      [6, 9],
      [12, 15],
      [18, 9],
    ],
  ],
  up: [
    [
      [6, 15],
      [12, 9],
      [18, 15],
    ],
  ],
  right: [
    [
      [9, 6],
      [15, 12],
      [9, 18],
    ],
  ],
  folder: [
    [
      [3, 6],
      [9, 6],
      [11, 8],
      [21, 8],
      [21, 19],
      [3, 19],
      [3, 6],
    ],
  ],
  box: [
    [
      [4, 7],
      [12, 3],
      [20, 7],
      [20, 17],
      [12, 21],
      [4, 17],
      [4, 7],
    ],
    [
      [4, 7],
      [12, 11],
      [20, 7],
    ],
    [
      [12, 11],
      [12, 21],
    ],
  ],
  door: [
    [
      [6, 3],
      [18, 3],
      [18, 21],
      [6, 21],
      [6, 3],
    ],
    [
      [14, 12],
      [14.5, 12],
    ],
  ],
});

// The commands of an icon centred on (x, y), `size` pixels across, in `color`.
function icon(name, x, y, size, color, width = 1.5) {
  const lines = ICONS[name];
  if (!lines) {
    return [];
  }
  const scale = size / 24;
  const at = ([px, py]) => [x + (px - 12) * scale, y + (py - 12) * scale];
  const commands = [];
  for (const line of lines) {
    const filled = line[0] === 'fill';
    const points = (filled ? line.slice(1) : line).map(at);
    commands.push(filled ? ['lineStyle'] : ['lineStyle', width, color, 100]);
    if (filled) {
      commands.push(['beginFill', color, 100]);
    }
    commands.push(['moveTo', points[0][0], points[0][1]]);
    for (const [px, py] of points.slice(1)) {
      commands.push(['lineTo', px, py]);
    }
    if (filled) {
      commands.push(['endFill']);
    }
  }
  return commands;
}

module.exports = { icon, ICONS };
