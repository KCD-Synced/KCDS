'use strict';

// The studio's simple props made in the game (Client/resources/studio/shapes.cjs):
// byte for byte the files Dev/tools/assets/kcds_studio/studio_shapes.py writes, and the prop
// menu that makes them.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const shapes = require('../../../../Client/resources/studio/shapes.cjs');

const SCRIPT = path.join(__dirname, '../../../../Dev/tools/assets/kcds_studio/studio_shapes.py');
const PYTHON = process.env.KCDS_PYTHON || (process.platform === 'win32' ? 'python' : 'python3');

function pythonFiles(name, options) {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-shapes-js-'));
  try {
    execFileSync(PYTHON, ['-I', SCRIPT, name, JSON.stringify(options), folder]);
    return {
      model: fs.readFileSync(path.join(folder, `${name}.cgf`)),
      material: fs.readFileSync(path.join(folder, `${name}.mtl`), 'ascii'),
    };
  } finally {
    fs.rmSync(folder, { recursive: true, force: true });
  }
}

// The file's chunks (CrCh 0x746): { type, offset, size } in table order.
function chunksOf(model) {
  const count = model.readUInt32LE(8);
  const table = model.readUInt32LE(12);
  return Array.from({ length: count }, (_, index) => ({
    type: model.readUInt16LE(table + index * 16),
    size: model.readUInt32LE(table + index * 16 + 8),
    offset: model.readUInt32LE(table + index * 16 + 12),
  }));
}

test('each shape is byte for byte the files studio_shapes.py writes', () => {
  const cases = [
    [
      'crate',
      { shape: 'box', size: [0.6, 0.4, 0.3], colour: [0.6, 0.35, 0.15], surface: 'mat_wood' },
    ],
    ['floor_tile', { shape: 'plane', size: [2, 3] }],
    ['post', { shape: 'cylinder', radius: 0.1, height: 1.5, sides: 16, surface: 'mat_metal' }],
    ['ball', { shape: 'sphere', radius: 0.3, rings: 8, sides: 12, colour: [0.2, 0.5, 0.8] }],
  ];
  for (const [name, options] of cases) {
    const python = pythonFiles(name, options);
    const made = shapes.shapeMessage(name, options);
    assert.equal(made.message.material, python.material, `${name}.mtl`);
    assert.equal(
      Buffer.from(made.model).toString('hex'),
      python.model.toString('hex'),
      `${name}.cgf`,
    );
    assert.equal(made.message.model, python.model.toString('base64'), `${name} in base64`);
  }
});

test('the default sphere: every chunk the same, its tangents at most one unit apart', () => {
  // Sines and cosines may differ by their last bit between JavaScript and Python, which moves a
  // tangent exactly at a rounding half by one unit (of 32767); nothing else.
  const python = pythonFiles('ball', { shape: 'sphere' });
  const made = Buffer.from(shapes.shapeMessage('ball', { shape: 'sphere' }).model);
  assert.equal(made.length, python.model.length);
  const chunks = chunksOf(python.model);
  assert.deepEqual(chunksOf(made), chunks);
  // The third data stream is the tangents (stream type 6).
  const tangents = chunks.find(
    (chunk) => chunk.type === 0x1016 && python.model.readInt32LE(chunk.offset + 4) === 6,
  );
  for (const chunk of chunks) {
    const ours = made.subarray(chunk.offset, chunk.offset + chunk.size);
    const theirs = python.model.subarray(chunk.offset, chunk.offset + chunk.size);
    if (chunk !== tangents) {
      assert.ok(ours.equals(theirs), `chunk 0x${chunk.type.toString(16)} at ${chunk.offset}`);
      continue;
    }
    for (let at = 24; at < chunk.size; at += 2) {
      assert.ok(
        Math.abs(ours.readInt16LE(at) - theirs.readInt16LE(at)) <= 1,
        `tangent part at ${at}`,
      );
    }
  }
});

test('half floats and rounding as Python packs them', () => {
  // 1, -2, 0.5, 65504 (the largest), a subnormal, -0 and a value halfway between two halves.
  assert.equal(shapes.halfBits(1), 0x3c00);
  assert.equal(shapes.halfBits(-2), 0xc000);
  assert.equal(shapes.halfBits(0.5), 0x3800);
  assert.equal(shapes.halfBits(65504), 0x7bff);
  assert.equal(shapes.halfBits(2 ** -24), 0x0001);
  assert.equal(shapes.halfBits(-0), 0x8000);
  assert.equal(shapes.halfBits(1 + 2 ** -11), 0x3c00);
  assert.equal(shapes.halfBits(1 + 3 * 2 ** -11), 0x3c02);
  assert.throws(() => shapes.halfBits(70000), /too large/);
  assert.equal(shapes.roundHalfEven(2.5), 2);
  assert.equal(shapes.roundHalfEven(3.5), 4);
  assert.equal(shapes.roundHalfEven(-16383.5), -16384);
});

function setup() {
  const { StudioEditor } = require('../../../../Client/resources/studio/editor.cjs');
  const sent = [];
  const editor = new StudioEditor({
    NativeUI: {},
    NoClip: {
      getPosition: () => ({ x: 32.6, y: 28, z: 33.3 }),
      screenLine: () => ({ x: 0, y: 1, z: 0 }),
    },
    Key: { isDown: () => false },
    send: (message) => sent.push(message),
    log: () => {},
  });
  editor.shell = { render: () => {}, viewPointer: () => ({ x: 900, y: 500 }) };
  const last = (op) => sent.filter((message) => message.op === op).at(-1);
  return { editor, sent, last };
}

test('the prop menu makes a shape: the game client writes it, it shows, Details change it', () => {
  const { editor, sent, last } = setup();
  editor.pressFolder(1);
  editor.meshesKept(['suzanne']);
  const tiles = editor.view().dock.tiles.map((tile) => tile.name);
  assert.deepEqual(tiles, ['suzanne', 'New box', 'New cylinder', 'New sphere', 'New plane']);
  // New box: its files as studio_shapes.py writes them, to the game client.
  editor.pressTile(1);
  const write = last('studioMeshWrite');
  assert.equal(write.name, 'box_1');
  const python = pythonFiles('box_1', {
    shape: 'box',
    size: [0.5, 0.5, 0.5],
    colour: [0.8, 0.8, 0.8],
  });
  assert.equal(write.model, python.model.toString('base64'));
  assert.equal(write.material, python.material);
  assert.match(editor.message, /^Making box_1: 24 vertices, 12 triangles/);
  // Written: shown beside the character, listed, and kept with the project.
  editor.meshWrittenKept({ name: 'box_1', ok: true, error: '' });
  assert.equal(last('studioMesh').name, 'box_1');
  assert.ok(editor.meshes.includes('box_1'));
  const row = (label) => editor.details().rows.findIndex((one) => one.label === label);
  assert.equal(editor.details().kind, 'A simple prop, made here');
  assert.equal(editor.details().rows[row('Width')].value, '0.50 m');
  // Wider, then wood: written anew under the same name; shown again it loads anew.
  editor.pressDetail(row('Width'), 1);
  assert.equal(last('studioMeshWrite').name, 'box_1');
  assert.deepEqual(editor.shapes.box_1.size, [0.55, 0.5, 0.5]);
  editor.pressDetail(row('Colour'), 1);
  assert.match(last('studioMeshWrite').material, /Diffuse="0\.4500,0\.3000,0\.1500"/);
  const shown = sent.filter((message) => message.op === 'studioMesh').length;
  editor.meshWrittenKept({ name: 'box_1', ok: true, error: '' });
  assert.equal(sent.filter((message) => message.op === 'studioMesh').length, shown + 1);
  const saved = JSON.parse(last('architectSave').data);
  assert.deepEqual(saved.shapes.box_1, {
    shape: 'box',
    size: [0.55, 0.5, 0.5],
    colour: [0.45, 0.3, 0.15],
    surface: 'mat_wood',
  });
  // The next one gets the next free name; a kept project knows its shapes again.
  editor.pressTile(editor.items().findIndex((item) => item.shape === 'box'));
  assert.equal(last('studioMeshWrite').name, 'box_2');
  const fresh = setup();
  fresh.editor.projectKept(JSON.stringify(saved));
  assert.deepEqual(fresh.editor.shapes.box_1.size, [0.55, 0.5, 0.5]);
  // A shape whose files are gone from the folder is forgotten; one being written is not.
  fresh.editor.meshesKept(['suzanne']);
  assert.deepEqual(fresh.editor.shapes, {});
  editor.meshesKept(['suzanne', 'box_1']);
  assert.ok(editor.shapes.box_2, 'box_2 is being written');
  // A refusal is said.
  editor.meshWrittenKept({ name: 'box_2', ok: false, error: 'the files could not be written' });
  assert.equal(editor.message, 'box_2: the files could not be written.');
});

test('the options are checked as studio_shapes.py checks them', () => {
  const refusals = [
    [{ shape: 'cone' }, /shape is one of/],
    [{ shape: 'box', size: [1, 1] }, /size is 3/],
    [{ shape: 'cylinder', radius: 0 }, /radius is from 0.01 to 50 metres/],
    [{ shape: 'sphere', rings: 1.5 }, /rings is a whole number/],
    [{ shape: 'plane', colour: [1, 2, 0] }, /colour is/],
    [{ shape: 'plane', surface: 'mat_lava' }, /surface is one of/],
    [{ shape: 'plane', texture: 'x' }, /unknown texture/],
  ];
  for (const [options, message] of refusals) {
    assert.throws(() => shapes.make('one', options), message);
  }
  assert.throws(() => shapes.make('bad name', { shape: 'box' }), /the name is/);
  assert.deepEqual(shapes.SURFACES, [
    ...require('../../../../Dev/tools/architect-mcp/editor_steps.cjs').SURFACES,
  ]);
});
