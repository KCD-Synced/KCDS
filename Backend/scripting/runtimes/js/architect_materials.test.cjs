'use strict';

// World Architect's own materials (Client/resources/architect/materials.cjs and
// material_work.cjs): textures drawn by code, their normal maps and DDS files, and a model's
// material with one slot drawn as another.
const test = require('node:test');
const assert = require('node:assert/strict');

const M = require('../../../../Client/resources/architect/materials.cjs');
const work = require('../../../../Client/resources/architect/material_work.cjs');

// A material with slots, laid out as the game's own (stjames_church.mtl).
const CHURCH = [
  '<Material MtlFlags="524544">',
  ' <SubMaterials>',
  '  <Material Name="glass" Shader="Glass"><Textures/></Material>',
  '  <Material Name="plaster_b" Shader="Illum" Diffuse="0.4,0.4,0.4">',
  '   <Textures>',
  '    <Texture File="textures/plaster_a_diff.tif" Map="Diffuse">',
  '     <TexMod TileU="1.5" TileV="1.5" />',
  '    </Texture>',
  '    <Texture File="textures/plaster_a_ddna.tif" Map="Bumpmap" />',
  '   </Textures>',
  '  </Material>',
  '  <Material Name="void" Shader="Illum" Diffuse="0,0,0"/>',
  ' </SubMaterials>',
  '</Material>',
].join('\n');

// How much a texture's brightness changes from a pixel to the next, over its brightness: the
// grain a review measures on a wall (Dev/tools/architect-mcp/look/picture_review.cjs detail).
function grainOf(texture) {
  const width = texture.size;
  const lum = (at) =>
    (0.2126 * texture.rgba[at] + 0.7152 * texture.rgba[at + 1] + 0.0722 * texture.rgba[at + 2]) /
    255;
  let change = 0;
  let light = 0;
  for (let y = 0; y < width - 1; y += 1) {
    for (let x = 0; x < width - 1; x += 1) {
      const at = (y * width + x) * 4;
      const here = lum(at);
      change += Math.abs(lum(at + 4) - here) + Math.abs(lum(at + width * 4) - here);
      light += here;
    }
  }
  return change / light;
}

test('plaster has its own grain (sand and trowel), as much as stone or planks', () => {
  // Seen live: walls given the made plaster were judged plain (detail 0.025 of their brightness)
  // where the game's own plaster passed; its grain was 0.006, stone's and planks' 0.03.
  const plaster = grainOf(M.drawTexture({ kind: 'plaster', size: 256 }));
  const stone = grainOf(M.drawTexture({ kind: 'stone_blocks', size: 256 }));
  assert.ok(plaster > 0.025, `plaster grain ${plaster}`);
  assert.ok(plaster >= stone * 0.8, `plaster ${plaster} against stone ${stone}`);
});

test('every kind of texture is drawn seamless, the same for the same seed', () => {
  for (const kind of Object.keys(M.KINDS)) {
    const a = M.drawTexture({ kind, size: 256, seed: 3 });
    const b = M.drawTexture({ kind, size: 256, seed: 3 });
    assert.deepEqual(a.rgba, b.rgba, kind);
    assert.equal(a.rgba.length, 256 * 256 * 4);
    // Across the tile's edge a pixel differs from its neighbour about as much as anywhere.
    const row = 100;
    const step = (x0, x1) => {
      let sum = 0;
      for (let c = 0; c < 3; c += 1) {
        sum += Math.abs(a.rgba[(row * 256 + x0) * 4 + c] - a.rgba[(row * 256 + x1) * 4 + c]);
      }
      return sum;
    };
    let inside = 0;
    for (let x = 0; x < 255; x += 1) {
      inside = Math.max(inside, step(x, x + 1));
    }
    assert.ok(step(255, 0) <= inside, `${kind} has a seam`);
  }
  assert.notDeepEqual(
    M.drawTexture({ kind: 'plaster', size: 256, seed: 1 }).rgba,
    M.drawTexture({ kind: 'plaster', size: 256, seed: 2 }).rgba,
  );
  assert.throws(() => M.drawTexture({ kind: 'marble' }), /kind is one of/);
  assert.throws(() => M.drawTexture({ kind: 'plaster', size: 300 }), /size is one of/);
});

test('a flat relief makes a normal map facing straight out; the DDS holds every mip level', () => {
  const flat = new Float32Array(4 * 4).fill(0.5);
  const normal = M.normalMap(flat, 4);
  assert.deepEqual([...normal.subarray(0, 4)], [128, 128, 255, 64]);
  const levels = M.mipLevels(new Uint8Array(4 * 4 * 4).fill(200), 4);
  assert.deepEqual(
    levels.map((level) => level.size),
    [4, 2, 1],
  );
  const dds = M.encodeDds(levels, true);
  const view = new DataView(dds.buffer);
  assert.equal(view.getUint32(0, true), 0x20534444);
  assert.equal(view.getUint32(12, true), 4);
  assert.equal(view.getUint32(28, true), 3);
  assert.equal(view.getUint32(128, true), 91, 'sRGB for a picture');
  assert.equal(new DataView(M.encodeDds(levels, false).buffer).getUint32(128, true), 87);
  assert.equal(dds.length, 4 + 124 + 20 + (16 + 4 + 1) * 4);
});

test("a material's slot is drawn as another material or another slot, keeping its name", () => {
  assert.deepEqual(
    M.subMaterials(CHURCH).map((slot) => slot.name),
    ['glass', 'plaster_b', 'void'],
  );
  const stone = M.materialXml({ diffuse: 'objects/kcds/t/s_diff.dds', normal: 'n.dds', tile: 2 });
  const changed = M.withSlot(CHURCH, 'void', M.ownDefinition(stone));
  const slots = M.subMaterials(changed);
  assert.deepEqual(
    slots.map((slot) => slot.name),
    ['glass', 'plaster_b', 'void'],
  );
  const voidNow = changed.slice(slots[2].start, slots[2].end);
  assert.match(voidNow, /^<Material Name="void"/);
  assert.match(voidNow, /objects\/kcds\/t\/s_diff\.dds/);
  // By index, like another slot, and its pictures repeated anew.
  const like = M.withSlot(CHURCH, 2, M.retiled(M.slotDefinition(CHURCH, 'plaster_b'), 4));
  const likeVoid = M.subMaterials(like)[2];
  const text = like.slice(likeVoid.start, likeVoid.end);
  assert.match(text, /Name="void"/);
  assert.equal((text.match(/TileU="4"/g) ?? []).length, 2);
  assert.throws(() => M.withSlot(CHURCH, 'roof', stone), /no slot roof/);
});

test('a made texture is written as its pictures and its material, each in parts of a message', () => {
  const sent = [];
  const made = work.makeTexture((message) => sent.push(message), {
    kind: 'stone_blocks',
    name: 'church_stone',
    size: 256,
  });
  assert.equal(made.material, 'objects/kcds/textures/church_stone');
  const paths = [...new Set(sent.map((message) => message.path))];
  assert.deepEqual(paths, [
    'objects/kcds/textures/textures/church_stone_diff.dds',
    'objects/kcds/textures/textures/church_stone_ddna.dds',
    'objects/kcds/textures/church_stone.mtl',
  ]);
  for (const path of paths) {
    const parts = sent.filter((message) => message.path === path);
    assert.deepEqual(
      parts.map((message) => message.part),
      parts.map((_, index) => index),
    );
    assert.ok(parts.every((message) => message.parts === parts.length));
    // Each message under the 64 KiB a runtime message holds.
    assert.ok(parts.every((message) => JSON.stringify(message).length < 60 * 1024));
  }
  const mtl = Buffer.from(sent.at(-1).data, 'base64').toString('utf8');
  assert.match(
    mtl,
    /Map="Bumpmap" File="objects\/kcds\/textures\/textures\/church_stone_ddna\.dds"/,
  );
  assert.match(mtl, /%NORMAL_MAP/);
  const diffuse = Buffer.concat(
    sent
      .filter((message) => message.path.endsWith('_diff.dds'))
      .map((message) => Buffer.from(message.data, 'base64')),
  );
  assert.equal(diffuse.readUInt32LE(0), 0x20534444);
  assert.throws(
    () => work.makeTexture(() => {}, { kind: 'plaster', name: 'Bad Name' }),
    /lower-case/,
  );
  assert.throws(
    () => work.makeTexture(() => {}, { kind: 'plaster', name: 'x', color: [2, 0, 0] }),
    /each 0 to 1/,
  );
});

test("a slot's look comes from a material of yours, a game material's slot, another slot, or its own", () => {
  const read = (path) =>
    ({
      'objects/kcds/textures/stone.mtl': M.materialXml({ diffuse: 'd.dds', normal: 'n.dds' }),
      'objects/manmade/walls/wall.mtl': CHURCH,
    }[path] ?? null);
  const original = CHURCH;
  const changed = work.slotText({
    text: CHURCH,
    original,
    slot: 'void',
    look: { material: 'objects/kcds/textures/stone' },
    read,
  });
  assert.match(changed, /Name="void"[^>]*>\s*<Textures>\s*<Texture Map="Diffuse" File="d\.dds"/);
  const fromGame = work.slotText({
    text: CHURCH,
    original,
    slot: 'void',
    look: { material: 'objects/manmade/walls/wall', lookSlot: 'plaster_b' },
    read,
  });
  assert.match(fromGame, /Name="void"[^>]*Diffuse="0\.4,0\.4,0\.4"/);
  assert.throws(
    () =>
      work.slotText({
        text: CHURCH,
        original,
        slot: 'void',
        look: { material: 'objects/manmade/walls/wall' },
        read,
      }),
    /has slots: say which one/,
  );
  const back = work.slotText({ text: changed, original, slot: 'void', look: null, read });
  assert.equal(back, CHURCH);
  assert.equal(
    work.slotMaterialName('objects/kcds/opened/stjames5_0.cgf', [
      'objects/kcds/materials/stjames5_0_1',
    ]),
    'objects/kcds/materials/stjames5_0_2',
  );
  assert.equal(work.editableSlot('$lod2_church'), false);
  assert.equal(work.editableSlot('proxy_stone'), false);
  assert.equal(work.editableSlot('plaster_b'), true);
});
