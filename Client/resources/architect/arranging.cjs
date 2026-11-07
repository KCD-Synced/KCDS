'use strict';

// Furniture arranged in a layout: a row (pews, a shelf of jars, torches along a wall), a grid
// (pews either side of an aisle, market stalls) or a ring around a point (chairs around a table,
// candles around an altar). Each layout answers the spots, [{ at: { x, y, z }, turn }], turned so
// a prop's front (its -y side, as the game's props face) looks the way asked.
//
// Pure: the editor's Details > ARRANGE (editor.cjs) and an AI assistant's arrange
// (ai_commands.cjs) place things at them.

const MOST = 64;

function degrees(radians) {
  return Math.round((((((radians * 180) / Math.PI) % 360) + 360) % 360) * 100) / 100;
}

function rounded(value) {
  return Math.round(value * 1000) / 1000;
}

// The turn that faces a prop's front (-y) along (dx, dy).
function turnToward(dx, dy) {
  return degrees(Math.atan2(dx, -dy));
}

function countOf(count) {
  const n = Math.floor(Number(count));
  if (!Number.isFinite(n) || n < 1 || n > MOST) {
    throw new Error(`count is 1 to ${MOST}`);
  }
  return n;
}

// Each spot's turn: `turn` for all, or each facing the point `toward`.
function turned(spots, { turn, toward }) {
  return spots.map((at) => {
    let own = Number.isFinite(turn) ? turn : 0;
    if (toward && Number.isFinite(toward.x) && Number.isFinite(toward.y)) {
      own = turnToward(toward.x - at.x, toward.y - at.y);
    }
    return { at, turn: own };
  });
}

// `count` spots from `from` to `to` (both ends, evenly); or with `spacing` from `from` toward
// `to` (or along `direction`, degrees from +x), `spacing` metres apart.
function rowSpots({ from, to = null, direction = null, count, spacing = null, turn, toward }) {
  const n = countOf(count);
  let dx = 1;
  let dy = 0;
  let step = Number(spacing);
  if (to) {
    dx = to.x - from.x;
    dy = to.y - from.y;
    const length = Math.hypot(dx, dy);
    if (length < 0.01) {
      throw new Error('a row needs from and to apart');
    }
    dx /= length;
    dy /= length;
    if (!Number.isFinite(step) || step <= 0) {
      step = n > 1 ? length / (n - 1) : 0;
    }
  } else {
    const angle = ((Number(direction) || 0) * Math.PI) / 180;
    dx = Math.cos(angle);
    dy = Math.sin(angle);
    if (!Number.isFinite(step) || step <= 0) {
      throw new Error('a row without to needs spacing');
    }
  }
  const spots = [];
  for (let k = 0; k < n; k += 1) {
    spots.push({
      x: rounded(from.x + dx * step * k),
      y: rounded(from.y + dy * step * k),
      ...(Number.isFinite(from.z) ? { z: from.z } : {}),
    });
  }
  return turned(spots, { turn, toward });
}

// `rows` x `columns` spots around `at`, `spacing` metres apart ([across, along] or one number),
// the grid turned `turn` degrees; `aisle` metres kept free down its middle (between the left and
// right halves of its columns). Each spot turned as the grid, unless `toward`.
function gridSpots({ at, rows, columns, spacing = 1.5, turn = 0, aisle = 0, toward }) {
  const r = countOf(rows);
  const c = countOf(columns);
  if (r * c > MOST) {
    throw new Error(`a grid has at most ${MOST} spots`);
  }
  const [across, along] = Array.isArray(spacing) ? spacing : [spacing, spacing];
  if (!(across > 0) || !(along > 0)) {
    throw new Error('spacing is more than 0');
  }
  const gap = Math.max(0, Number(aisle) || 0);
  const width = (c - 1) * across + (c > 1 ? gap : 0);
  const depth = (r - 1) * along;
  const t = ((Number(turn) || 0) * Math.PI) / 180;
  const cos = Math.cos(t);
  const sin = Math.sin(t);
  const spots = [];
  for (let row = 0; row < r; row += 1) {
    for (let column = 0; column < c; column += 1) {
      const pastAisle = gap > 0 && column >= Math.ceil(c / 2) ? gap : 0;
      const x = column * across + pastAisle - width / 2;
      const y = row * along - depth / 2;
      spots.push({
        x: rounded(at.x + x * cos - y * sin),
        y: rounded(at.y + x * sin + y * cos),
        ...(Number.isFinite(at.z) ? { z: at.z } : {}),
      });
    }
  }
  return turned(spots, { turn: Number(turn) || 0, toward });
}

// `count` spots on a ring of `radius` around `at`, from `start` degrees (from +x); each faces
// the middle (face 'in', chairs at a table) or away from it ('out').
function aroundSpots({ at, count, radius, start = 0, face = 'in' }) {
  const n = countOf(count);
  if (!(radius > 0)) {
    throw new Error('radius is more than 0');
  }
  const spots = [];
  for (let k = 0; k < n; k += 1) {
    const angle = ((Number(start) || 0) * Math.PI) / 180 + (2 * Math.PI * k) / n;
    const x = at.x + Math.cos(angle) * radius;
    const y = at.y + Math.sin(angle) * radius;
    const inward = turnToward(at.x - x, at.y - y);
    spots.push({
      at: { x: rounded(x), y: rounded(y), ...(Number.isFinite(at.z) ? { z: at.z } : {}) },
      turn: face === 'out' ? (inward + 180) % 360 : inward,
    });
  }
  return spots;
}

// `count` spots in a block that starts at `at` (the first spot) and runs `columns` wide along
// the thing's right (+x turned by `turn`), its rows behind it (+y), `spacing` metres apart; each
// turned as the thing. Details' copies of a selected prop: one column is a row.
function blockSpots({ at, turn = 0, count, columns = 1, spacing }) {
  const n = countOf(count);
  const wide = Math.max(1, Math.floor(Number(columns) || 1));
  if (!(spacing > 0)) {
    throw new Error('spacing is more than 0');
  }
  const t = ((Number(turn) || 0) * Math.PI) / 180;
  const cos = Math.cos(t);
  const sin = Math.sin(t);
  const spots = [];
  for (let k = 0; k < n; k += 1) {
    const x = (k % wide) * spacing;
    const y = Math.floor(k / wide) * spacing;
    spots.push({
      at: {
        x: rounded(at.x + x * cos - y * sin),
        y: rounded(at.y + x * sin + y * cos),
        ...(Number.isFinite(at.z) ? { z: at.z } : {}),
      },
      turn: Number(turn) || 0,
    });
  }
  return spots;
}

// The spots of a layout: 'row', 'grid' or 'around', with that layout's options.
function layoutSpots(layout, options) {
  if (layout === 'row') {
    return rowSpots(options);
  }
  if (layout === 'grid') {
    return gridSpots(options);
  }
  if (layout === 'around') {
    return aroundSpots(options);
  }
  throw new Error('layout is row, grid or around');
}

module.exports = {
  layoutSpots,
  rowSpots,
  gridSpots,
  aroundSpots,
  blockSpots,
  turnToward,
  MOST,
};
