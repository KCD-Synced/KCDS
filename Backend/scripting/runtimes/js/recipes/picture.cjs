'use strict';

// A recipe's picture: a script's base64 PNG decoded and fitted onto a square of the
// size the game shows it at, as an uncompressed B8G8R8A8 .dds the game client mounts as it is.
// The aspect is kept: the picture is scaled to fit the square and centred on a transparent one.
// PNGs of 8 bits per channel, without interlacing, in any of the PNG colour types.

const zlib = require('node:zlib');

const SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
// Channels by colour type: grey, -, RGB, palette, grey + alpha, -, RGBA.
const CHANNELS = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 };
const MAX_SIDE = 4096;

function fail(message) {
  throw new TypeError(`Crafting.defineRecipe: ${message}`);
}

function paeth(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  if (pa <= pb && pa <= pc) {
    return a;
  }
  return pb <= pc ? b : c;
}

// The PNG's pixels as RGBA, { width, height, rgba }; throws for one this cannot read.
function decodePng(bytes, name) {
  if (bytes.length < 33 || !bytes.subarray(0, 8).equals(SIGNATURE)) {
    fail(`${name} must be a base64 PNG`);
  }
  let offset = 8;
  let header = null;
  let palette = null;
  let transparency = null;
  const data = [];
  while (offset + 8 <= bytes.length) {
    const length = bytes.readUInt32BE(offset);
    const type = bytes.toString('latin1', offset + 4, offset + 8);
    const body = bytes.subarray(offset + 8, offset + 8 + length);
    offset += 12 + length;
    if (type === 'IHDR') {
      header = {
        width: body.readUInt32BE(0),
        height: body.readUInt32BE(4),
        depth: body[8],
        colour: body[9],
        interlace: body[12],
      };
    } else if (type === 'PLTE') {
      palette = body;
    } else if (type === 'tRNS') {
      transparency = body;
    } else if (type === 'IDAT') {
      data.push(body);
    } else if (type === 'IEND') {
      break;
    }
  }
  const channels = header ? CHANNELS[header.colour] : undefined;
  if (!header || channels === undefined || header.depth !== 8 || header.interlace !== 0) {
    fail(`${name} must be a PNG of 8 bits per channel without interlacing`);
  }
  const { width, height } = header;
  if (width < 1 || height < 1 || width > MAX_SIDE || height > MAX_SIDE) {
    fail(`${name} must be at most ${MAX_SIDE}x${MAX_SIDE} pixels`);
  }
  if (header.colour === 3 && !palette) {
    fail(`${name} has no palette`);
  }
  let raw;
  try {
    raw = zlib.inflateSync(Buffer.concat(data));
  } catch {
    fail(`${name} is not a readable PNG`);
  }
  const stride = width * channels;
  if (raw.length < height * (stride + 1)) {
    fail(`${name} is not a readable PNG`);
  }
  const pixels = Buffer.alloc(height * stride);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x++) {
      const left = x >= channels ? pixels[y * stride + x - channels] : 0;
      const up = y > 0 ? pixels[(y - 1) * stride + x] : 0;
      const corner = x >= channels && y > 0 ? pixels[(y - 1) * stride + x - channels] : 0;
      let value = line[x];
      if (filter === 1) {
        value += left;
      } else if (filter === 2) {
        value += up;
      } else if (filter === 3) {
        value += (left + up) >> 1;
      } else if (filter === 4) {
        value += paeth(left, up, corner);
      } else if (filter !== 0) {
        fail(`${name} is not a readable PNG`);
      }
      pixels[y * stride + x] = value & 0xff;
    }
  }
  const rgba = Buffer.alloc(width * height * 4);
  for (let index = 0; index < width * height; index++) {
    const at = index * channels;
    let r;
    let g;
    let b;
    let a = 255;
    if (header.colour === 3) {
      const entry = pixels[at];
      r = palette[entry * 3] ?? 0;
      g = palette[entry * 3 + 1] ?? 0;
      b = palette[entry * 3 + 2] ?? 0;
      a = transparency && entry < transparency.length ? transparency[entry] : 255;
    } else if (header.colour === 0 || header.colour === 4) {
      r = g = b = pixels[at];
      a = header.colour === 4 ? pixels[at + 1] : 255;
    } else {
      r = pixels[at];
      g = pixels[at + 1];
      b = pixels[at + 2];
      a = header.colour === 6 ? pixels[at + 3] : 255;
    }
    rgba.set([r, g, b, a], index * 4);
  }
  return { width, height, rgba };
}

// The picture fitted onto a `size` x `size` square: scaled to fit (each pixel the average of the
// area it covers), centred, the rest transparent; RGBA.
function fitSquare({ width, height, rgba }, size) {
  const scale = Math.min(size / width, size / height);
  const fitWidth = Math.max(1, Math.round(width * scale));
  const fitHeight = Math.max(1, Math.round(height * scale));
  const left = Math.floor((size - fitWidth) / 2);
  const top = Math.floor((size - fitHeight) / 2);
  const out = Buffer.alloc(size * size * 4);
  for (let y = 0; y < fitHeight; y++) {
    const y0 = Math.floor((y * height) / fitHeight);
    const y1 = Math.max(y0 + 1, Math.floor(((y + 1) * height) / fitHeight));
    for (let x = 0; x < fitWidth; x++) {
      const x0 = Math.floor((x * width) / fitWidth);
      const x1 = Math.max(x0 + 1, Math.floor(((x + 1) * width) / fitWidth));
      const sum = [0, 0, 0, 0];
      for (let sy = y0; sy < y1; sy++) {
        for (let sx = x0; sx < x1; sx++) {
          const at = (sy * width + sx) * 4;
          for (let channel = 0; channel < 4; channel++) {
            sum[channel] += rgba[at + channel];
          }
        }
      }
      const count = (y1 - y0) * (x1 - x0);
      const at = ((top + y) * size + left + x) * 4;
      for (let channel = 0; channel < 4; channel++) {
        out[at + channel] = Math.round(sum[channel] / count);
      }
    }
  }
  return out;
}

// An uncompressed B8G8R8A8 .dds of a square RGBA picture.
function dds(rgba, size) {
  const header = Buffer.alloc(128);
  header.write('DDS ', 0, 'latin1');
  const fields = [
    124,
    0x1 | 0x2 | 0x4 | 0x8 | 0x1000,
    size,
    size,
    size * 4,
    0,
    1,
    ...new Array(11).fill(0),
    32,
    0x41,
    0,
    32,
    0x00ff0000,
    0x0000ff00,
    0x000000ff,
    0xff000000,
    0x1000,
    0,
    0,
    0,
    0,
  ];
  fields.forEach((value, index) => header.writeUInt32LE(value >>> 0, 4 + index * 4));
  const pixels = Buffer.alloc(size * size * 4);
  for (let index = 0; index < size * size; index++) {
    const at = index * 4;
    pixels[at] = rgba[at + 2];
    pixels[at + 1] = rgba[at + 1];
    pixels[at + 2] = rgba[at];
    pixels[at + 3] = rgba[at + 3];
  }
  return Buffer.concat([header, pixels]);
}

// A colour as the 5:6:5 of a DXT block, and back to 8 bits per channel.
function pack565(r, g, b) {
  return (
    (Math.round((r * 31) / 255) << 11) |
    (Math.round((g * 63) / 255) << 5) |
    Math.round((b * 31) / 255)
  );
}

function unpack565(value) {
  const r = (value >> 11) & 31;
  const g = (value >> 5) & 63;
  const b = value & 31;
  return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)];
}

// The index of the palette entry nearest to `value` (an array of channels per entry).
function nearest(palette, value) {
  let best = 0;
  let bestDistance = Infinity;
  palette.forEach((entry, index) => {
    let distance = 0;
    for (let channel = 0; channel < value.length; channel++) {
      const delta = entry[channel] - value[channel];
      distance += delta * delta;
    }
    if (distance < bestDistance) {
      best = index;
      bestDistance = distance;
    }
  });
  return best;
}

// One 4 x 4 block of DXT5: its 16 pixels' RGBA as [[r, g, b, a], ...], row by row; 16 bytes.
function dxt5Block(pixels) {
  const block = Buffer.alloc(16);
  // Alpha: the block's two extremes and six steps between them, three bits a pixel.
  const alphas = pixels.map((pixel) => pixel[3]);
  const a0 = Math.max(...alphas);
  const a1 = Math.min(...alphas);
  block[0] = a0;
  block[1] = a1;
  if (a0 > a1) {
    const levels = [[a0], [a1]];
    for (let step = 1; step <= 6; step++) {
      levels.push([Math.round(((7 - step) * a0 + step * a1) / 7)]);
    }
    // Eight pixels' indices fill three bytes: the first eight, then the other eight.
    for (let half = 0; half < 2; half++) {
      let bits = 0;
      for (let index = 0; index < 8; index++) {
        bits |= nearest(levels, [alphas[half * 8 + index]]) << (index * 3);
      }
      block.writeUIntLE(bits, 2 + half * 3, 3);
    }
  }
  // Colour: the block's box of colours, inset a little, and two steps between its corners.
  const low = [255, 255, 255];
  const high = [0, 0, 0];
  for (const pixel of pixels) {
    for (let channel = 0; channel < 3; channel++) {
      low[channel] = Math.min(low[channel], pixel[channel]);
      high[channel] = Math.max(high[channel], pixel[channel]);
    }
  }
  for (let channel = 0; channel < 3; channel++) {
    const inset = (high[channel] - low[channel]) / 16;
    high[channel] = Math.round(high[channel] - inset);
    low[channel] = Math.round(low[channel] + inset);
  }
  let c0 = pack565(...high);
  let c1 = pack565(...low);
  if (c0 < c1) {
    [c0, c1] = [c1, c0];
  }
  block.writeUInt16LE(c0, 8);
  block.writeUInt16LE(c1, 10);
  if (c0 !== c1) {
    const p0 = unpack565(c0);
    const p1 = unpack565(c1);
    const palette = [
      p0,
      p1,
      p0.map((value, channel) => Math.round((2 * value + p1[channel]) / 3)),
      p0.map((value, channel) => Math.round((value + 2 * p1[channel]) / 3)),
    ];
    let bits = 0;
    pixels.forEach((pixel, index) => {
      bits |= nearest(palette, pixel.slice(0, 3)) << (index * 2);
    });
    block.writeUInt32LE(bits >>> 0, 12);
  }
  return block;
}

// A DXT5 (BC3) .dds of a square RGBA picture whose side is a multiple of 4; one byte a pixel.
function dxt5Dds(rgba, size) {
  const header = Buffer.alloc(128);
  header.write('DDS ', 0, 'latin1');
  const fields = [
    124,
    0x1 | 0x2 | 0x4 | 0x1000 | 0x80000,
    size,
    size,
    size * size,
    0,
    1,
    ...new Array(11).fill(0),
    32,
    0x4,
    Buffer.from('DXT5', 'latin1').readUInt32LE(0),
    0,
    0,
    0,
    0,
    0,
    0x1000,
    0,
    0,
    0,
    0,
  ];
  fields.forEach((value, index) => header.writeUInt32LE(value >>> 0, 4 + index * 4));
  const blocks = [];
  for (let top = 0; top < size; top += 4) {
    for (let left = 0; left < size; left += 4) {
      const pixels = [];
      for (let y = 0; y < 4; y++) {
        for (let x = 0; x < 4; x++) {
          const at = ((top + y) * size + left + x) * 4;
          pixels.push([rgba[at], rgba[at + 1], rgba[at + 2], rgba[at + 3]]);
        }
      }
      blocks.push(dxt5Block(pixels));
    }
  }
  return Buffer.concat([header, ...blocks]);
}

function pngOf(base64, name) {
  const bytes = typeof base64 === 'string' ? Buffer.from(base64, 'base64') : Buffer.alloc(0);
  return decodePng(bytes, name);
}

// A base64 PNG as the .dds of a `size` x `size` square the game shows it on.
function pictureDds(base64, size, name) {
  return dds(fitSquare(pngOf(base64, name), size), size);
}

// A base64 PNG as the DXT5 .dds of a `size` x `size` square: the recipe book's pictures, whose
// large one (1024) would not travel uncompressed.
function bookPictureDds(base64, size, name) {
  return dxt5Dds(fitSquare(pngOf(base64, name), size), size);
}

module.exports = { decodePng, fitSquare, pictureDds, bookPictureDds, pngOf };
