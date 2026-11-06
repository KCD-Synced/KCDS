'use strict';

// The editors' script-step tools of the MCP server (Dev/tools/architect-mcp/editor_steps.cjs):
// its tools, how it finds the game's pipe, and a tool call through a stand-in for
// the game client's pipe (Client/runtime/architect/editor_pipe.hpp: JSON lines { id, line } in,
// { id, text } out).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const {
  TOOLS,
  SURFACES,
  findPipe,
  EditorLink,
  handle,
  makeMesh,
  importGltf,
  importGltfAnimation,
} = require('../../../../Dev/tools/architect-mcp/editor_steps.cjs');

test('the server answers initialize and lists the studio and editor tools', async () => {
  const init = await handle({ method: 'initialize', params: {} }, null);
  assert.equal(init.result.serverInfo.name, 'kcds-architect');
  assert.deepEqual(init.result.capabilities, { tools: {} });
  const list = await handle({ method: 'tools/list' }, null);
  assert.deepEqual(
    list.result.tools.map((tool) => tool.name),
    TOOLS.map((tool) => tool.name),
  );
  assert.ok(list.result.tools.some((tool) => tool.name === 'studio_make_prop'));
  const unknown = await handle({ method: 'nothing' }, null);
  assert.equal(unknown.error.code, -32601);
});

test('the pipe: the named game, else the first KCDS editor pipe', () => {
  assert.equal(
    findPipe({ KCDS_EDITOR_PID: '42' }, () => []),
    '\\\\.\\pipe\\kcds-editor-42',
  );
  assert.equal(
    findPipe({}, () => ['other', 'kcds-editor-7', 'kcds-editor-9']),
    '\\\\.\\pipe\\kcds-editor-7',
  );
  assert.equal(
    findPipe({}, () => ['other']),
    null,
  );
});

test('without a game a tool call says so', async () => {
  const call = await handle({ method: 'tools/call', params: { name: 'studio_state' } }, null);
  assert.equal(call.result.isError, true);
  assert.match(call.result.content[0].text, /No KCDS editor/);
});

test('a call with wrong arguments is answered with what is wrong, and the server goes on', async () => {
  const wrong = await handle(
    { method: 'tools/call', params: { name: 'editor_camera', arguments: { at: [1, 2, 3] } } },
    null,
  );
  assert.equal(wrong.result.isError, true);
  assert.match(wrong.result.content[0].text, /missing position, look_at; unknown at/);
  const after = await handle({ method: 'tools/list' }, null);
  assert.ok(after.result.tools.length > 0);
});

test('a tool call goes to the editor as its steps and comes back as their answers', async () => {
  const path = `\\\\.\\pipe\\kcds-editor-test-${process.pid}`;
  const seen = [];
  const game = net.createServer((socket) => {
    socket.setEncoding('utf8');
    let buffer = '';
    socket.on('data', (text) => {
      buffer += text;
      let end;
      while ((end = buffer.indexOf('\n')) >= 0) {
        const { id, line } = JSON.parse(buffer.slice(0, end));
        buffer = buffer.slice(end + 1);
        seen.push(line);
        socket.write(`${JSON.stringify({ id, text: `${line} -> ok` })}\n`);
      }
    });
  });
  await new Promise((resolve) => game.listen(path, resolve));
  const link = new EditorLink(path);
  try {
    const made = await handle(
      { method: 'tools/call', params: { name: 'studio_make_prop', arguments: { kind: 'whip' } } },
      link,
    );
    assert.equal(made.result.content[0].text, 'make whip -> ok');
    const camera = await handle(
      {
        method: 'tools/call',
        params: { name: 'editor_camera', arguments: { position: [1, 2, 3], look_at: [4, 5, 6] } },
      },
      link,
    );
    assert.equal(camera.result.content[0].text, 'view 1 2 3 4 5 6 -> ok');
    const placed = await handle(
      {
        method: 'tools/call',
        params: { name: 'studio_place', arguments: { prop: { joint: 'LeftHand' } } },
      },
      link,
    );
    assert.equal(placed.result.content[0].text, 'place {"prop":{"joint":"LeftHand"}} -> ok');
    assert.deepEqual(seen, [
      'make whip',
      'view 1 2 3 4 5 6',
      'place {"prop":{"joint":"LeftHand"}}',
    ]);
  } finally {
    link.close();
    await new Promise((resolve) => game.close(resolve));
  }
});

test('studio_make_mesh writes the shape’s model files, then shows it in the editor', async () => {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-shapes-'));
  const seen = [];
  const link = {
    step: async (line) => {
      seen.push(line);
      return `${line} -> ok`;
    },
  };
  try {
    const content = await makeMesh(
      {
        name: 'crate',
        shape: 'box',
        size: [0.6, 0.4, 0.3],
        colour: [0.5, 0.3, 0.1],
        surface: 'mat_wood',
      },
      link,
      process.env,
      folder,
    );
    const made = JSON.parse(content[0].text);
    assert.deepEqual(
      [made.name, made.shape, made.vertices, made.triangles],
      ['crate', 'box', 24, 12],
    );
    const model = fs.readFileSync(path.join(folder, 'crate.cgf'));
    assert.equal(model.subarray(0, 4).toString(), 'CrCh');
    const material = fs.readFileSync(path.join(folder, 'crate.mtl'), 'utf8');
    assert.match(material, /Diffuse="0.5000,0.3000,0.1000"/);
    assert.deepEqual(seen, ['meshes', 'mesh crate']);
    assert.equal(content[1].text, 'mesh crate -> ok');
    // What studio_shapes.py refuses comes back as the tool's error, and nothing goes to the editor.
    await assert.rejects(
      makeMesh({ name: 'post', shape: 'cylinder', radius: 0 }, link, process.env, folder),
      /radius is from 0.01 to 50 metres/,
    );
    assert.equal(seen.length, 2);
  } finally {
    fs.rmSync(folder, { recursive: true, force: true });
  }
});

test('studio_import_gltf converts a .glb into your mesh, then shows it; a bad file says why', async () => {
  // A quad facing up (glTF +y): positions, then indices, in one binary chunk.
  const positions = [-1, 0, 1, 1, 0, 1, 1, 0, -1, -1, 0, -1];
  const binary = Buffer.alloc(48 + 12);
  positions.forEach((value, index) => binary.writeFloatLE(value, index * 4));
  [0, 1, 2, 0, 2, 3].forEach((value, index) => binary.writeUInt16LE(value, 48 + index * 2));
  const document = {
    asset: { version: '2.0' },
    scenes: [{ nodes: [0] }],
    nodes: [{ mesh: 0 }],
    meshes: [{ primitives: [{ attributes: { POSITION: 0 }, indices: 1 }] }],
    accessors: [
      { bufferView: 0, componentType: 5126, count: 4, type: 'VEC3' },
      { bufferView: 1, componentType: 5123, count: 6, type: 'SCALAR' },
    ],
    bufferViews: [
      { buffer: 0, byteOffset: 0, byteLength: 48 },
      { buffer: 0, byteOffset: 48, byteLength: 12 },
    ],
    buffers: [{ byteLength: 60 }],
  };
  let text = Buffer.from(JSON.stringify(document));
  text = Buffer.concat([text, Buffer.alloc((4 - (text.length % 4)) % 4, 0x20)]);
  const chunk = (data, kind) => {
    const head = Buffer.alloc(8);
    head.writeUInt32LE(data.length, 0);
    head.writeUInt32LE(kind, 4);
    return Buffer.concat([head, data]);
  };
  const body = Buffer.concat([chunk(text, 0x4e4f534a), chunk(binary, 0x004e4942)]);
  const header = Buffer.alloc(12);
  header.writeUInt32LE(0x46546c67, 0);
  header.writeUInt32LE(2, 4);
  header.writeUInt32LE(12 + body.length, 8);
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-gltf-'));
  const file = path.join(folder, 'quad.glb');
  fs.writeFileSync(file, Buffer.concat([header, body]));
  const seen = [];
  const link = {
    step: async (line) => {
      seen.push(line);
      return `${line} -> ok`;
    },
  };
  try {
    const content = await importGltf({ path: file, name: 'quad' }, link, process.env, folder);
    const made = JSON.parse(content[0].text);
    assert.deepEqual([made.vertices, made.triangles, made.materials], [4, 2, 1]);
    assert.ok(fs.existsSync(path.join(folder, 'quad.cgf')));
    assert.deepEqual(seen, ['meshes', 'mesh quad']);
    await assert.rejects(
      importGltf({ path: path.join(folder, 'none.glb'), name: 'none' }, link, process.env, folder),
      /^Error: studio_import_gltf: /,
    );
  } finally {
    fs.rmSync(folder, { recursive: true, force: true });
  }
});

test('studio_import_gltf_animation measures the game skeleton, converts the file and sets it', async () => {
  // A rig: Hips > RightArm (out to the character's right, glTF -x), the arm turned -90 degrees
  // about glTF z (up) over 1 s.
  const binary = Buffer.alloc(8 + 32);
  binary.writeFloatLE(0, 0);
  binary.writeFloatLE(1, 4);
  const half = Math.PI / 4;
  [0, 0, 0, 1, 0, 0, -Math.sin(half), Math.cos(half)].forEach((value, index) =>
    binary.writeFloatLE(value, 8 + index * 4),
  );
  const document = {
    asset: { version: '2.0' },
    scenes: [{ nodes: [0] }],
    nodes: [
      { name: 'mixamorig:Hips', translation: [0, 1, 0], children: [1] },
      { name: 'mixamorig:RightArm', translation: [-0.2, 0.4, 0], children: [2] },
      { name: 'mixamorig:RightForeArm', translation: [-0.3, 0, 0] },
    ],
    animations: [
      {
        channels: [{ sampler: 0, target: { node: 1, path: 'rotation' } }],
        samplers: [{ input: 0, output: 1 }],
      },
    ],
    accessors: [
      { bufferView: 0, componentType: 5126, count: 2, type: 'SCALAR' },
      { bufferView: 1, componentType: 5126, count: 2, type: 'VEC4' },
    ],
    bufferViews: [
      { buffer: 0, byteOffset: 0, byteLength: 8 },
      { buffer: 0, byteOffset: 8, byteLength: 32 },
    ],
    buffers: [
      { uri: `data:application/octet-stream;base64,${binary.toString('base64')}`, byteLength: 40 },
    ],
  };
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-gltf-rig-'));
  const file = path.join(folder, 'rig.gltf');
  fs.writeFileSync(file, JSON.stringify(document));
  const axes = { x: [1, 0, 0], y: [0, 1, 0], z: [0, 0, 1] };
  const info = {
    Hips: { name: 'Hips', found: true, axes, bone: [0.2, 0, 0.4], child: 'RightArm' },
    RightArm: { name: 'RightArm', found: true, axes, bone: [0.3, 0, 0], child: 'RightForeArm' },
    RightForeArm: { name: 'RightForeArm', found: true, axes },
  };
  const seen = [];
  // The editor's answers as it writes them: "<line> -> <result> <state>".
  const answer = (line, result) =>
    `${line} -> ${JSON.stringify(result)} ${JSON.stringify({ open: true })}`;
  const link = {
    step: async (line) => {
      seen.push(line.split(' ')[0]);
      if (line === 'joints') {
        return answer(line, [
          { id: 0, name: 'Hips', parent: -1 },
          { id: 1, name: 'RightArm', parent: 0 },
          { id: 2, name: 'RightForeArm', parent: 1 },
        ]);
      }
      if (line.startsWith('jointinfo')) {
        return answer(line, {
          joints: line
            .split(' ')
            .slice(1)
            .map((name) => info[name]),
        });
      }
      if (line.startsWith('animation')) {
        return answer('animation', { keys: JSON.parse(line.slice(10)).keys.length });
      }
      return answer(line, null);
    },
  };
  try {
    const content = await importGltfAnimation({ path: file, rate: 4 }, link);
    const summary = JSON.parse(content[0].text);
    assert.deepEqual(summary.mapped, {
      Hips: 'mixamorig:Hips',
      RightArm: 'mixamorig:RightArm',
      RightForeArm: 'mixamorig:RightForeArm',
    });
    assert.equal(summary.keys, 5);
    // Bones shown, the joints read, the body stood still, measured, then the animation set.
    assert.deepEqual(seen, ['bones', 'joints', 'animation', 'jointinfo', 'animation']);
    assert.match(content[1].text, /^animation -> \{"keys":5\}/);
    await assert.rejects(importGltfAnimation({ path: file }, null), /No KCDS editor/);
  } finally {
    fs.rmSync(folder, { recursive: true, force: true });
  }
});

test('the surfaces studio_make_mesh offers are the ones studio_shapes.py takes', () => {
  const script = fs.readFileSync(
    path.join(__dirname, '../../../../Dev/tools/assets/kcds_studio/studio_shapes.py'),
    'utf8',
  );
  const tuple = /SURFACES = \(([^)]*)\)/.exec(script)[1];
  assert.deepEqual(
    [...tuple.matchAll(/"(mat_[a-z_]+)"/g)].map((match) => match[1]),
    [...SURFACES],
  );
  const tool = TOOLS.find((one) => one.name === 'studio_make_mesh');
  assert.deepEqual(tool.inputSchema.properties.surface.enum, SURFACES);
});
