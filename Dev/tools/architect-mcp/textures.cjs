'use strict';

// A World Architect material from a photo or picture, made by the MCP server on the author's PC
// (the texture tool): the picture as a game texture (.dds, its mip levels in it) and a material
// (.mtl) that draws it, written into the author's assets folder (Documents\KCDS\World
// Architect\assets) at objects/kcds/<folder>/, where the editor loads them as the author's own
// files. A PNG is read here; any other picture (JPG, WebP, ...) through ffmpeg when it is
// installed. Node's own zlib only.

const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const { execFileSync } = require('node:child_process');

const NAME = /^[a-z0-9_]{1,32}$/;
// The largest side a texture keeps, pixels (a side is a power of two up to this).
const LARGEST_SIDE = 2048;
const SMALLEST_SIDE = 4;
const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
// The ground or wall a material's things sound and break as (the game's surface types).
const SURFACES = Object.freeze({
  stone: 'mat_stone',
  wood: 'mat_wood',
  plaster: 'mat_stone',
  metal: 'mat_metal',
  cloth: 'mat_fabric',
  ground: 'mat_soil',
});
// A DDS file: the header (DDSURFACEDESC2), its pixel format and its DX10 header, 32-bit BGRA.
const DDS_MAGIC = 0x20534444;
const DDS_HEADER_SIZE = 124;
const DDS_PIXEL_FORMAT_SIZE = 32;
const DDSD_CAPS = 0x1;
const DDSD_HEIGHT = 0x2;
const DDSD_WIDTH = 0x4;
const DDSD_PITCH = 0x8;
const DDSD_PIXELFORMAT = 0x1000;
const DDSD_MIPMAPCOUNT = 0x20000;
const DDSCAPS_COMPLEX = 0x8;
const DDSCAPS_TEXTURE = 0x1000;
const DDSCAPS_MIPMAP = 0x400000;
// The pictures are sRGB, as the game's own (a DX10 header, as its textures have): without it the
// game reads them as linear and every colour comes out pale (Prague's walls).
const DDPF_FOURCC = 0x4;
const DX10_FOURCC = 0x30315844;
const DX10_HEADER_SIZE = 20;
const DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91;
const D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3;
// The game's own mark of its textures (CRYF, bytes F Y R C at the header's end) and its flag for
// an sRGB picture, in the header's first reserved word.
const CRY_FOURCC = Buffer.from('FYRC', 'latin1');
const CRY_SRGB_FLAG = 0x800;

// ---- PNG ----

// The picture of a PNG's bytes: { width, height, rgba } (8 bits a channel, not interlaced).
function decodePng(bytes) {
  if (!bytes.subarray(0, 8).equals(PNG_SIGNATURE)) {
    throw new Error('not a PNG file');
  }
  let at = 8;
  let header = null;
  let palette = null;
  let transparency = null;
  const data = [];
  while (at < bytes.length) {
    const length = bytes.readUInt32BE(at);
    const type = bytes.toString('latin1', at + 4, at + 8);
    const body = bytes.subarray(at + 8, at + 8 + length);
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
    at += 12 + length;
  }
  if (!header) {
    throw new Error('the PNG has no header');
  }
  const { width, height, depth, colour, interlace } = header;
  const channels = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[colour];
  if (depth !== 8 || channels === undefined || interlace !== 0) {
    throw new Error('the PNG must be 8 bits a channel and not interlaced (save it again)');
  }
  const raw = zlib.inflateSync(Buffer.concat(data));
  const stride = width * channels;
  const rows = Buffer.alloc(stride * height);
  unfilter(raw, rows, stride, channels, height);
  const rgba = Buffer.alloc(width * height * 4);
  for (let pixel = 0; pixel < width * height; pixel += 1) {
    const from = pixel * channels;
    const to = pixel * 4;
    if (colour === 3) {
      const index = rows[from];
      rgba[to] = palette[index * 3];
      rgba[to + 1] = palette[index * 3 + 1];
      rgba[to + 2] = palette[index * 3 + 2];
      rgba[to + 3] = transparency && index < transparency.length ? transparency[index] : 255;
    } else if (channels <= 2) {
      rgba[to] = rows[from];
      rgba[to + 1] = rows[from];
      rgba[to + 2] = rows[from];
      rgba[to + 3] = channels === 2 ? rows[from + 1] : 255;
    } else {
      rgba[to] = rows[from];
      rgba[to + 1] = rows[from + 1];
      rgba[to + 2] = rows[from + 2];
      rgba[to + 3] = channels === 4 ? rows[from + 3] : 255;
    }
  }
  return { width, height, rgba };
}

// PNG's row filters undone: raw (a filter byte before each row) into rows.
function unfilter(raw, rows, stride, step, height) {
  for (let row = 0; row < height; row += 1) {
    const filter = raw[row * (stride + 1)];
    const source = row * (stride + 1) + 1;
    const target = row * stride;
    for (let x = 0; x < stride; x += 1) {
      const left = x >= step ? rows[target + x - step] : 0;
      const up = row > 0 ? rows[target + x - stride] : 0;
      const corner = row > 0 && x >= step ? rows[target + x - stride - step] : 0;
      let value = raw[source + x];
      if (filter === 1) {
        value += left;
      } else if (filter === 2) {
        value += up;
      } else if (filter === 3) {
        value += Math.floor((left + up) / 2);
      } else if (filter === 4) {
        value += paeth(left, up, corner);
      }
      rows[target + x] = value & 0xff;
    }
  }
}

function paeth(left, up, corner) {
  const guess = left + up - corner;
  const toLeft = Math.abs(guess - left);
  const toUp = Math.abs(guess - up);
  const toCorner = Math.abs(guess - corner);
  if (toLeft <= toUp && toLeft <= toCorner) {
    return left;
  }
  return toUp <= toCorner ? up : corner;
}

// ---- Reading a picture ----

// ffmpeg's path when it is installed (KCDS_FFMPEG, else on the PATH), null without it.
function findFfmpeg() {
  const named = process.env.KCDS_FFMPEG;
  if (named && fs.existsSync(named)) {
    return named;
  }
  try {
    execFileSync('ffmpeg', ['-version'], { stdio: 'ignore', windowsHide: true });
    return 'ffmpeg';
  } catch {
    return null;
  }
}

// The picture of a file: a PNG read here, another kind turned into a PNG by ffmpeg first.
function readPicture(file) {
  const bytes = fs.readFileSync(file);
  if (bytes.subarray(0, 8).equals(PNG_SIGNATURE)) {
    return decodePng(bytes);
  }
  const ffmpeg = findFfmpeg();
  if (!ffmpeg) {
    throw new Error(`${path.basename(file)} is no PNG and ffmpeg is not installed: save it as PNG`);
  }
  const png = execFileSync(
    ffmpeg,
    [
      '-v',
      'error',
      '-i',
      file,
      '-frames:v',
      '1',
      '-pix_fmt',
      'rgba',
      '-f',
      'image2pipe',
      '-c:v',
      'png',
      '-',
    ],
    { maxBuffer: 512 * 1024 * 1024, windowsHide: true },
  );
  return decodePng(png);
}

// ---- Sizes and mip levels ----

// The power of two nearest a side, SMALLEST_SIDE to LARGEST_SIDE.
function sideFor(side, largest) {
  let power = SMALLEST_SIDE;
  while (power * 2 <= Math.min(largest, LARGEST_SIDE) && power * 1.5 < side) {
    power *= 2;
  }
  return power;
}

// The picture drawn again at width x height (each new pixel the mean of the old ones it covers,
// or the nearest blend when it grows).
function resample(picture, width, height) {
  const out = Buffer.alloc(width * height * 4);
  const scaleX = picture.width / width;
  const scaleY = picture.height / height;
  for (let y = 0; y < height; y += 1) {
    const top = y * scaleY;
    const bottom = Math.max(top + 1, (y + 1) * scaleY);
    for (let x = 0; x < width; x += 1) {
      const left = x * scaleX;
      const right = Math.max(left + 1, (x + 1) * scaleX);
      const sum = [0, 0, 0, 0];
      let count = 0;
      for (let sy = Math.floor(top); sy < Math.ceil(bottom) && sy < picture.height; sy += 1) {
        for (let sx = Math.floor(left); sx < Math.ceil(right) && sx < picture.width; sx += 1) {
          const at = (sy * picture.width + sx) * 4;
          for (let channel = 0; channel < 4; channel += 1) {
            sum[channel] += picture.rgba[at + channel];
          }
          count += 1;
        }
      }
      const to = (y * width + x) * 4;
      for (let channel = 0; channel < 4; channel += 1) {
        out[to + channel] = Math.round(sum[channel] / Math.max(1, count));
      }
    }
  }
  return { width, height, rgba: out };
}

// The picture and its halves down to 1 x 1.
function mipLevels(picture) {
  const levels = [picture];
  let last = picture;
  while (last.width > 1 || last.height > 1) {
    last = resample(last, Math.max(1, last.width >> 1), Math.max(1, last.height >> 1));
    levels.push(last);
  }
  return levels;
}

// ---- DDS and MTL ----

// A DDS file of 32-bit BGRA pixels with its mip levels.
function encodeDds(levels) {
  const { width, height } = levels[0];
  const header = Buffer.alloc(4 + DDS_HEADER_SIZE + DX10_HEADER_SIZE);
  header.writeUInt32LE(DDS_MAGIC, 0);
  header.writeUInt32LE(DDS_HEADER_SIZE, 4);
  header.writeUInt32LE(
    DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT,
    8,
  );
  header.writeUInt32LE(height, 12);
  header.writeUInt32LE(width, 16);
  header.writeUInt32LE(width * 4, 20);
  header.writeUInt32LE(levels.length, 28);
  header.writeUInt32LE(CRY_SRGB_FLAG, 36);
  const format = 4 + 72;
  header.writeUInt32LE(DDS_PIXEL_FORMAT_SIZE, format);
  header.writeUInt32LE(DDPF_FOURCC, format + 4);
  header.writeUInt32LE(DX10_FOURCC, format + 8);
  header.writeUInt32LE(DDSCAPS_COMPLEX | DDSCAPS_TEXTURE | DDSCAPS_MIPMAP, format + 32);
  CRY_FOURCC.copy(header, 4 + 120);
  const dx10 = 4 + DDS_HEADER_SIZE;
  header.writeUInt32LE(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, dx10);
  header.writeUInt32LE(D3D10_RESOURCE_DIMENSION_TEXTURE2D, dx10 + 4);
  header.writeUInt32LE(0, dx10 + 8);
  header.writeUInt32LE(1, dx10 + 12);
  header.writeUInt32LE(0, dx10 + 16);
  const pixels = levels.map((level) => {
    const bgra = Buffer.alloc(level.rgba.length);
    for (let at = 0; at < bgra.length; at += 4) {
      bgra[at] = level.rgba[at + 2];
      bgra[at + 1] = level.rgba[at + 1];
      bgra[at + 2] = level.rgba[at];
      bgra[at + 3] = level.rgba[at + 3];
    }
    return bgra;
  });
  return Buffer.concat([header, ...pixels]);
}

// A material drawing one texture (the game's Illum shader), the texture repeated `tile` times.
function materialXml(texture, surface, tile) {
  const repeat = tile === 1 ? '' : `\n      <TexMod TileU="${tile}" TileV="${tile}" />\n    `;
  const inner = repeat ? `>${repeat}</Texture>` : ' />';
  return (
    '<?xml version="1.0" encoding="us-ascii"?>\n' +
    `<Material MtlFlags="524288" Shader="Illum" GenMask="0" StringGenMask="" ` +
    `SurfaceType="${surface}" Diffuse="1,1,1" Specular="0.04,0.04,0.04" Opacity="1" ` +
    `Shininess="10" LayerAct="1">\n` +
    '  <Textures>\n' +
    `    <Texture Map="Diffuse" File="${texture}"${inner}\n` +
    '  </Textures>\n' +
    '</Material>\n'
  );
}

// ---- The tool ----

// Makes a material from a picture in the assets folder; answers its name for set_material and
// the files written. args: { image, name, folder, surface, tile, size }.
function makeTexture(args, assetsFolder) {
  const name = String(args.name ?? '');
  const folder = String(args.folder ?? 'textures');
  if (!NAME.test(name) || !NAME.test(folder)) {
    throw new Error('name and folder are 1 to 32 lower-case letters, digits or _');
  }
  const image = String(args.image ?? '');
  if (!image || !fs.existsSync(image)) {
    throw new Error(`no picture at ${image || '(no image given)'}`);
  }
  if (!assetsFolder) {
    throw new Error('the game named no assets folder (open the editor first)');
  }
  const surface = SURFACES[args.surface ?? 'stone'];
  if (!surface) {
    throw new Error(`surface is one of ${Object.keys(SURFACES).join(', ')}`);
  }
  const tile = Number(args.tile ?? 1);
  if (!(tile >= 0.01 && tile <= 100)) {
    throw new Error('tile is 0.01 to 100 (times the texture repeats on a face)');
  }
  const base = `objects/kcds/${folder}`;
  const texture = `${base}/textures/${name}_diff.dds`;
  const material = `${base}/${name}`;
  const written = writeTexture(
    image,
    path.join(assetsFolder, texture),
    Number(args.size ?? LARGEST_SIDE),
  );
  fs.writeFileSync(path.join(assetsFolder, `${material}.mtl`), materialXml(texture, surface, tile));
  return { material, texture, size: written.size, from: written.from };
}

// A picture written as a game texture at `target` (its folder made), its largest side at most
// `largest` pixels; answers its size and the picture's.
function writeTexture(image, target, largest = LARGEST_SIDE) {
  const picture = readPicture(image);
  const width = sideFor(picture.width, largest);
  const height = sideFor(picture.height, largest);
  const levels = mipLevels(resample(picture, width, height));
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, encodeDds(levels));
  return { size: { width, height }, from: { width: picture.width, height: picture.height } };
}

module.exports = {
  makeTexture,
  writeTexture,
  NAME,
  decodePng,
  encodeDds,
  materialXml,
  mipLevels,
  resample,
  sideFor,
  SURFACES,
};
