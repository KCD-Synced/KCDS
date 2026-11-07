'use strict';

// How a picture of the world looks, measured, so an AI assistant judges light and colour by
// numbers and not only by eye: the editor's pictures (screenshot, review) are PNGs; this reads
// their pixels (textures.cjs decodePng) and says what a player would notice.
//
//   measure(png)   the whole picture's { brightness, dark, bright, contrast, detail, color, cast,
//                  black, flat }, and the same for its upper part (walls, ceiling, sky) and its
//                  lower part (the floor, the ground) in `upper` and `lower`
//   verdict(m)     plain sentences: too dark, washed out, cold walls, untextured walls, holes ...

const { decodePng } = require('../textures.cjs');

// Every SAMPLE_STEP-th pixel across and down is measured (a 1600 px picture in a few ms).
const SAMPLE_STEP = 2;
// The blocks the black and flat areas are found in (pixels a side).
const BLOCK = 16;
// The upper part ends and the lower part starts this far down the picture (shares of its height).
const UPPER_END = 0.45;
const LOWER_START = 0.6;
// Brightness (0 to 1, as the screen shows it): near black, near white.
const DARK = 0.07;
const BRIGHT = 0.96;
// A block this dark and this even is a flat black area (a hole, a face drawn without material);
// one this even but lit is a flat area (a stretched or missing texture, or the sky).
const BLACK_MEAN = 0.04;
const BLACK_SPREAD = 0.015;
const FLAT_SPREAD = 0.008;
// What the verdict calls too dark, dim, washed out, low in contrast, a colour cast, plain walls
// and holes, measured against the game's own rooms: 52 views of 13 rooms of Kuttenberg's houses,
// courts, sheds and its bastion at noon (brightness median 0.08, many under 0.03; walls' blue over
// red at most 0.03, median -0.03; walls' even share median 0.05, detail over brightness at least
// 0.1 in nine views of ten), and an opened church lit by the sky (walls blue over red 0.04,
// even 0.4, detail over brightness 0.085). The game's rooms are dark: dimness is said, not
// blamed, and what is measured on walls is only measured where they are lit enough to see.
const TOO_DARK = 0.03;
const DIM = 0.1;
const WASHED = 0.7;
const BRIGHT_SHARE = 0.2;
const LOW_CONTRAST = 0.06;
const CAST = 0.04;
// Walls lit by the sky alone lean blue more gently than a whole picture does; no wall of the
// game's rooms leaned more than this.
const WALLS_CAST = 0.03;
// Holes: flat black areas in a picture that is lit elsewhere (a dark room is black in corners).
const BLACK_SHARE = 0.08;
const BLACK_LIT = 0.15;
const FLAT_SHARE = 0.35;
// Indoors the walls' own check names plain walls; the whole picture's evenness is said above this.
const FLAT_SHARE_INDOORS = 0.55;
const WALLS_FLAT = 0.3;
// Detail over brightness: a texture's grain shows less in the dark.
const LITTLE_DETAIL = 0.09;
// Walls this lit are measured for flatness and detail; contrast is judged above LIT.
const WALLS_LIT = 0.06;
const LIT = 0.15;
const DARK_WALLS = 0.03;

const round = (value) => Math.round(value * 1000) / 1000;

// The light and colour of the rows from `top` to `bottom` (pixels), 0 to 1 each, shares of the
// part's area.
function partOf(pixels, width, top, bottom) {
  let count = 0;
  let sum = 0;
  let squares = 0;
  let dark = 0;
  let bright = 0;
  let red = 0;
  let green = 0;
  let blue = 0;
  let detail = 0;
  const luma = (x, y) => {
    const at = (y * width + x) * 4;
    return (0.2126 * pixels[at] + 0.7152 * pixels[at + 1] + 0.0722 * pixels[at + 2]) / 255;
  };
  const across = Math.ceil(width / BLOCK);
  const down = Math.max(1, Math.ceil((bottom - top) / BLOCK));
  const blocks = Array.from({ length: across * down }, () => ({ n: 0, sum: 0, squares: 0 }));
  for (let y = top; y < bottom; y += SAMPLE_STEP) {
    for (let x = 0; x < width; x += SAMPLE_STEP) {
      const at = (y * width + x) * 4;
      const value = luma(x, y);
      count += 1;
      sum += value;
      squares += value * value;
      red += pixels[at] / 255;
      green += pixels[at + 1] / 255;
      blue += pixels[at + 2] / 255;
      if (value < DARK) {
        dark += 1;
      } else if (value > BRIGHT) {
        bright += 1;
      }
      // Detail: how much the brightness changes to the next pixel across and down (a texture's
      // grain, edges); a plain or stretched surface has little.
      const right = x + 1 < width ? luma(x + 1, y) : value;
      const below = y + 1 < bottom ? luma(x, y + 1) : value;
      detail += Math.abs(right - value) + Math.abs(below - value);
      const block = blocks[Math.floor((y - top) / BLOCK) * across + Math.floor(x / BLOCK)];
      block.n += 1;
      block.sum += value;
      block.squares += value * value;
    }
  }
  const spread = (n, total, totalSquares) =>
    Math.sqrt(Math.max(0, totalSquares / n - (total / n) ** 2));
  let black = 0;
  let flat = 0;
  let used = 0;
  for (const block of blocks) {
    if (block.n === 0) {
      continue;
    }
    used += 1;
    const blockMean = block.sum / block.n;
    const blockSpread = spread(block.n, block.sum, block.squares);
    if (blockMean < BLACK_MEAN && blockSpread < BLACK_SPREAD) {
      black += 1;
    } else if (blockSpread < FLAT_SPREAD) {
      flat += 1;
    }
  }
  const color = { r: red / count, g: green / count, b: blue / count };
  return {
    brightness: round(sum / count),
    dark: round(dark / count),
    bright: round(bright / count),
    contrast: round(spread(count, sum, squares)),
    detail: round(detail / count),
    color: { r: round(color.r), g: round(color.g), b: round(color.b) },
    cast: castOf(color),
    black: round(black / Math.max(1, used)),
    flat: round(flat / Math.max(1, used)),
  };
}

// The picture's light and colour, whole and in its upper and lower parts. `png`: the file's
// bytes, or base64 text as the editor answers it.
function measure(png) {
  const bytes = Buffer.isBuffer(png) ? png : Buffer.from(String(png), 'base64');
  const { width, height, rgba } = decodePng(bytes);
  return {
    width,
    height,
    ...partOf(rgba, width, 0, height),
    upper: partOf(rgba, width, 0, Math.max(1, Math.floor(height * UPPER_END))),
    lower: partOf(rgba, width, Math.min(height - 1, Math.floor(height * LOWER_START)), height),
  };
}

// The colour a part leans to: 'blue', 'green', 'warm', or 'neutral'.
function castOf({ r, g, b }) {
  if (b > r + CAST && b >= g) {
    return 'blue';
  }
  if (g > r + CAST && g > b + CAST) {
    return 'green';
  }
  if (r > b + 3 * CAST) {
    return 'warm';
  }
  return 'neutral';
}

// What a player would notice, as sentences an assistant can act on. `indoors` (review's answer)
// tells whether the picture is of a room; `walls` whether its upper part is then walls and ceiling
// (an eye-level view; one looking down across the room shows the far floor there), which the
// walls' own colour and texture checks are made on.
function verdict(m, { indoors = false, walls = indoors } = {}) {
  const lines = [];
  const percent = (share) => `${Math.round(share * 100)} %`;
  if (m.brightness < TOO_DARK) {
    lines.push(
      `Too dark (brightness ${m.brightness}): a player sees almost nothing. Add lights or raise ` +
        "their intensity; the game's own rooms read at about 0.03 to 0.25, a lit hall or shop " +
        'at 0.15 to 0.4.',
    );
  } else if (m.brightness < DIM) {
    lines.push(
      `Dim (brightness ${m.brightness}), as many of the game's own rooms are: fine for a house, ` +
        'a cellar or the night; a hall, a shop or a church wants more light.',
    );
  }
  if (m.brightness > WASHED || m.bright > BRIGHT_SHARE) {
    lines.push(
      `Washed out (brightness ${m.brightness}, ${percent(m.bright)} near white): lower the ` +
        "lights' intensity or radius.",
    );
  }
  if (m.cast === 'blue') {
    lines.push(
      indoors
        ? 'A blue cast: cold sky light fills the room, as in an opened building. The game lights ' +
            'its rooms with a room light (light preset room, ambient: true: it sets the ambient ' +
            'light within its radius): one in the middle, radius to the walls, warm, intensity ' +
            'about 1 to 2.5 (lower darkens the room); then warm lights at the lamps and windows.'
        : 'A blue cast: cold light (dusk, moon, fog). Fine outdoors at night; by day set a ' +
            'clearer weather or a warmer hour.',
    );
  } else if (m.cast === 'green') {
    lines.push('A green cast: an odd light colour or material. Look at the lights you set.');
  }
  const upper = m.upper;
  if (walls && upper) {
    const { r, b } = upper.color;
    if (b > r + WALLS_CAST && m.cast !== 'blue') {
      lines.push(
        'The walls and ceiling are cold blue while the floor is not: the walls are lit by the ' +
          'sky. A room light (light preset room) over the room takes some of it out; then light ' +
          'the walls warm from lamps near them, one every 10 m or so. A building open to the sky ' +
          "(no roof, St James's nave) keeps some of it: the game's light inside comes from the " +
          "level's own building, which lets the sky in there.",
      );
    }
    if (upper.brightness < DARK_WALLS && m.brightness >= DIM) {
      lines.push(
        `The walls and ceiling are near black (brightness ${upper.brightness}) over a lit floor: ` +
          'the lights reach the floor only. Put lights higher and give them a radius that reaches ' +
          'the walls.',
      );
    }
    // Only walls lit enough to show their texture are judged for it.
    const detail = upper.detail / Math.max(upper.brightness, 0.03);
    if (upper.brightness >= WALLS_LIT && (upper.flat > WALLS_FLAT || detail < LITTLE_DETAIL)) {
      lines.push(
        `The walls and ceiling look plain (${percent(upper.flat)} even, detail ` +
          `${Math.round(detail * 1000) / 1000} of their brightness; the game's own rooms show ` +
          'at least 0.1): no texture, or one stretched over a large face. Give them a stone, ' +
          'plaster or wood material (make_texture and set_slot, or set_material), or stand the ' +
          "game's wall pieces and decorations against them.",
      );
    }
  }
  if (m.contrast < LOW_CONTRAST && m.brightness >= LIT) {
    lines.push(
      `Flat, low contrast (${m.contrast}): light from fewer, stronger sources with shadows on ` +
        'gives depth.',
    );
  }
  // A dark room is black in its corners: flat black areas are holes only in a lit picture.
  if (m.black > BLACK_SHARE && m.brightness >= BLACK_LIT) {
    lines.push(
      `${percent(m.black)} of the picture is flat black areas: holes, faces drawn without ` +
        'material (an opened building\'s "void"), or the inside of a wall. Look at them.',
    );
  }
  // In a dim picture a texture's grain is too faint to tell from an even face (a candle-lit
  // plaster room of the game's measures 70 % even): even areas are judged in a lit one only.
  const flatShare = indoors ? FLAT_SHARE_INDOORS : FLAT_SHARE;
  const evenJudged = m.brightness >= DIM;
  if (evenJudged && m.flat > flatShare && !(walls && upper && upper.flat > WALLS_FLAT)) {
    lines.push(
      `${percent(m.flat)} of the picture is even, without texture: the sky, or surfaces with a ` +
        'stretched or missing texture. Indoors, give walls and floors a material.',
    );
  }
  if (lines.length === 0) {
    lines.push(`Light and colour look plausible (brightness ${m.brightness}, ${m.cast}).`);
  }
  return lines;
}

module.exports = { measure, verdict, castOf };
