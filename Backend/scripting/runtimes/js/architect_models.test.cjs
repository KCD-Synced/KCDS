'use strict';

// World Architect's model import (Dev/tools/architect-mcp/models): a Blender OBJ with its .mtl and
// pictures into the game's own model, materials and textures in the author's assets folder.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { readObj } = require('../../../../Dev/tools/architect-mcp/models/obj_reader.cjs');
const { importModel } = require('../../../../Dev/tools/architect-mcp/models/import_model.cjs');
const { half } = require('../../../../Dev/tools/architect-mcp/models/cgf_writer.cjs');

// A box as OBJ lines (Blender's frame: y up, -z forward), given in the game's frame.
function box(lines, name, material, from, to) {
  const first = lines.filter((line) => line.startsWith('v ')).length + 1;
  for (const x of [from[0], to[0]]) {
    for (const y of [from[1], to[1]]) {
      for (const z of [from[2], to[2]]) {
        lines.push(`v ${x} ${z} ${-y}`);
      }
    }
  }
  const v = (i, j, k) => first + i * 4 + j * 2 + k;
  lines.push(`o ${name}`, `usemtl ${material}`);
  const quads = [
    [v(0, 0, 0), v(1, 0, 0), v(1, 0, 1), v(0, 0, 1)],
    [v(1, 1, 0), v(0, 1, 0), v(0, 1, 1), v(1, 1, 1)],
    [v(0, 1, 0), v(0, 0, 0), v(0, 0, 1), v(0, 1, 1)],
    [v(1, 0, 0), v(1, 1, 0), v(1, 1, 1), v(1, 0, 1)],
    [v(0, 0, 1), v(1, 0, 1), v(1, 1, 1), v(0, 1, 1)],
    [v(0, 1, 0), v(1, 1, 0), v(1, 0, 0), v(0, 0, 0)],
  ];
  for (const quad of quads) {
    lines.push(`f ${quad.join(' ')}`);
  }
}

// A small house: walls, a roof of another colour, a door object and a collision object.
function house(folder) {
  const lines = ['mtllib house.mtl'];
  box(lines, 'wall_back', 'plaster', [-4, 2.7, 0], [4, 3, 4]);
  box(lines, 'wall_front', 'plaster', [-4, -3, 0], [-0.6, -2.7, 4]);
  box(lines, 'roof', 'roof_red', [-4.3, -3.3, 4], [4.3, 3.3, 4.3]);
  box(lines, 'door_front', 'plaster', [-0.6, -3, 0], [0.6, -2.7, 2.2]);
  box(lines, 'collision_floor', 'plaster', [-4, -3, 0], [4, 3, 0.1]);
  fs.writeFileSync(path.join(folder, 'house.obj'), `${lines.join('\n')}\n`);
  fs.writeFileSync(
    path.join(folder, 'house.mtl'),
    'newmtl plaster\nKd 0.9 0.85 0.7\n\nnewmtl roof_red\nKd 0.6 0.15 0.1\n',
  );
  return path.join(folder, 'house.obj');
}

// The chunks of a chunk file: [{ type, id, body }].
function chunks(bytes) {
  assert.equal(bytes.toString('latin1', 0, 4), 'CrCh');
  assert.equal(bytes.readUInt32LE(4), 0x746);
  const count = bytes.readUInt32LE(8);
  const table = bytes.readUInt32LE(12);
  const out = [];
  for (let index = 0; index < count; index += 1) {
    const at = table + index * 16;
    const offset = bytes.readUInt32LE(at + 12);
    out.push({
      type: bytes.readUInt16LE(at),
      id: bytes.readUInt32LE(at + 4),
      body: bytes.subarray(offset, offset + bytes.readUInt32LE(at + 8)),
    });
  }
  return out;
}

test("an OBJ is read in the game's frame: Blender's y up becomes z, polygons become triangles", () => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-models-'));
  const model = readObj(house(folder));
  const roof = model.objects.find((object) => object.name === 'roof');
  assert.equal(roof.material, 'roof_red');
  assert.equal(roof.triangles.length, 12);
  const heights = roof.triangles.flat().map((corner) => corner.p[2]);
  assert.deepEqual([Math.min(...heights), Math.max(...heights)], [4, 4.3]);
  assert.deepEqual(model.materials.get('roof_red').colour, [0.6, 0.15, 0.1]);
  // A picture's path with spaces after Blender's options.
  fs.writeFileSync(
    path.join(folder, 'more.mtl'),
    'newmtl wall\nmap_Kd -s 1 1 1 -bm 1 Old Town/wall photo.png\n',
  );
  const { readMaterials } = require('../../../../Dev/tools/architect-mcp/models/obj_reader.cjs');
  assert.equal(
    readMaterials(path.join(folder, 'more.mtl')).get('wall').texture,
    path.resolve(folder, 'Old Town/wall photo.png'),
  );
  fs.rmSync(folder, { recursive: true, force: true });
});

test("a model becomes the game's model: one drawn node, box proxies on the proxy sub-material, doors answered", () => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-models-'));
  const assets = path.join(folder, 'assets');
  const made = importModel({ file: house(folder), name: 'old_house', folder: 'praha' }, assets);
  assert.equal(made.mesh, 'objects/kcds/praha/old_house.cgf');
  assert.deepEqual(made.meshes, [made.mesh]);
  assert.equal(made.materials, 2);
  assert.deepEqual(made.collision, { kind: 'marked', boxes: 1 });
  assert.equal(made.doors.length, 1);
  assert.equal(made.doors[0].name, 'door_front');
  assert.ok(Math.abs(made.doors[0].size.width - 1.2) < 1e-6);
  assert.ok(Math.abs(made.doors[0].size.height - 2.2) < 1e-6);
  const mtl = fs.readFileSync(path.join(assets, `${made.material}.mtl`), 'utf8');
  const names = [...mtl.matchAll(/Name="([^"]+)"[^>]*Shader="([^"]+)"/g)].map((m) => [m[1], m[2]]);
  assert.deepEqual(names, [
    ['proxy', 'Nodraw'],
    ['plaster', 'Illum'],
    ['roof_red', 'Illum'],
  ]);
  for (let index = 0; index < 2; index += 1) {
    assert.ok(
      fs.existsSync(path.join(assets, `objects/kcds/praha/textures/old_house_${index}_diff.dds`)),
    );
  }
  const file = chunks(fs.readFileSync(path.join(assets, made.mesh)));
  // The material chunk: three sub-materials, the first physicalized as the proxies'.
  const material = file.find((chunk) => chunk.type === 0x1014).body;
  assert.equal(material.readInt32LE(128), 3);
  assert.deepEqual(
    [material.readInt32LE(132), material.readInt32LE(136), material.readInt32LE(140)],
    [0x1100, -1, -1],
  );
  // Nodes: the drawn one at the top, one box proxy under it.
  const nodes = file.filter((chunk) => chunk.type === 0x100b);
  const top = nodes.filter((node) => node.body.readInt32LE(68) === -1);
  assert.equal(top.length, 1);
  const proxies = nodes.filter((node) => node.body.toString('latin1', 0, 15) === '$physics_proxy_');
  assert.equal(proxies.length, 1);
  assert.equal(proxies[0].body.toString('latin1', 204), 'box\r\n');
  // The floor's box: centre and half size in the serialized physics box.
  const physics = file.find((chunk) => chunk.type === 0x1018).body;
  const floats = (at, count) =>
    Array.from({ length: count }, (_, k) => physics.readFloatLE(at + k * 4));
  const round = (values) => values.map((value) => Math.round(value * 1000) / 1000);
  assert.deepEqual(round(floats(24 + 28 * 4, 3)), [0, 0, 0.05]);
  assert.deepEqual(round(floats(24 + 31 * 4, 3)), [4, 3, 0.05]);
  // Drawn vertices: the wall's corner in half-float metres.
  const packed = file.find(
    (chunk) => chunk.type === 0x1016 && chunk.body.readInt32LE(4) === 15,
  ).body;
  assert.equal(packed.readUInt16LE(24), half(-4));
  fs.rmSync(folder, { recursive: true, force: true });
});

test('a model too large for one piece is several files, each one drawn node; a far one is refused', () => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-models-'));
  const lines = ['o platform'];
  const across = 160;
  const along = 80;
  for (let j = 0; j <= along; j += 1) {
    for (let i = 0; i <= across; i += 1) {
      lines.push(`v ${i * 0.5 - 40} 1 ${-(j * 0.5 - 20)}`);
    }
  }
  for (let j = 0; j < along; j += 1) {
    for (let i = 0; i < across; i += 1) {
      const a = j * (across + 1) + i + 1;
      lines.push(`f ${a} ${a + 1} ${a + across + 2} ${a + across + 1}`);
    }
  }
  const file = path.join(folder, 'platform.obj');
  fs.writeFileSync(file, `${lines.join('\n')}\n`);
  const assets = path.join(folder, 'assets');
  const made = importModel({ file, name: 'platform', folder: 'test' }, assets);
  assert.ok(made.meshes.length > 1);
  assert.equal(made.meshes[1], 'objects/kcds/test/platform_1.cgf');
  for (const mesh of made.meshes) {
    const nodes = chunks(fs.readFileSync(path.join(assets, mesh))).filter(
      (chunk) => chunk.type === 0x100b,
    );
    assert.equal(nodes.filter((node) => node.body.readInt32LE(68) === -1).length, 1);
  }
  // Its one box (the platform's) goes with one piece only.
  const boxes = made.meshes
    .map(
      (mesh) =>
        chunks(fs.readFileSync(path.join(assets, mesh))).filter((chunk) => chunk.type === 0x1018)
          .length,
    )
    .reduce((sum, count) => sum + count, 0);
  assert.equal(boxes, 1);
  assert.throws(
    () => importModel({ file, name: 'far', folder: 'test', scale: 10 }, assets),
    /reaches 400 m from its origin/,
  );
  fs.rmSync(folder, { recursive: true, force: true });
});

test('two_sided draws both sides of every face: the flag on each drawn sub-material only', () => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-models-'));
  const assets = path.join(folder, 'assets');
  const flagsOf = (made) => {
    const mtl = fs.readFileSync(path.join(assets, `${made.material}.mtl`), 'utf8');
    return [...mtl.matchAll(/Name="([^"]+)" MtlFlags="(\d+)"/g)].map((m) => [m[1], Number(m[2])]);
  };
  const one = importModel({ file: house(folder), name: 'one_side', folder: 'praha' }, assets);
  assert.deepEqual(flagsOf(one), [
    ['proxy', 525440],
    ['plaster', 524416],
    ['roof_red', 524416],
  ]);
  const both = importModel(
    { file: house(folder), name: 'both_sides', folder: 'praha', two_sided: true },
    assets,
  );
  assert.deepEqual(flagsOf(both), [
    ['proxy', 525440],
    ['plaster', 524418],
    ['roof_red', 524418],
  ]);
  fs.rmSync(folder, { recursive: true, force: true });
});
