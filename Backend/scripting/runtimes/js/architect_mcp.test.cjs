'use strict';

// World Architect's MCP server (Dev/tools/architect-mcp): MCP over stdio in, the game's AI bridge
// (a token line, then one JSON line each way) out, against a stand-in for the game.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');

const SERVER = path.join(__dirname, '../../../../Dev/tools/architect-mcp/server.cjs');
const { TOOLS } = require('../../../../Dev/tools/architect-mcp/tools.cjs');
const { TOOLS: STEP_TOOLS } = require('../../../../Dev/tools/architect-mcp/editor_steps.cjs');

// A stand-in game: takes the token, then answers each request as the editor would.
function standInGame(token, answer) {
  const asked = [];
  const server = net.createServer((socket) => {
    let buffer = '';
    let trusted = false;
    socket.setEncoding('utf8');
    // The MCP server's process is killed at the end: its connection resets.
    socket.on('error', () => {});
    socket.on('data', (chunk) => {
      buffer += chunk;
      let end = buffer.indexOf('\n');
      while (end >= 0) {
        const line = buffer.slice(0, end);
        buffer = buffer.slice(end + 1);
        if (!trusted) {
          trusted = line === token;
          socket.write(trusted ? 'ok\n' : 'no\n');
        } else {
          const request = JSON.parse(line);
          asked.push(request);
          socket.write(`${JSON.stringify({ id: request.id, ...answer(request) })}\n`);
        }
        end = buffer.indexOf('\n');
      }
    });
  });
  return { server, asked };
}

function mcp(env) {
  const child = spawn(process.execPath, [SERVER], { env: { ...process.env, ...env } });
  let buffer = '';
  const waiting = new Map();
  child.stdout.setEncoding('utf8');
  child.stdout.on('data', (chunk) => {
    buffer += chunk;
    let end = buffer.indexOf('\n');
    while (end >= 0) {
      const message = JSON.parse(buffer.slice(0, end));
      buffer = buffer.slice(end + 1);
      waiting.get(message.id)?.(message);
      end = buffer.indexOf('\n');
    }
  });
  let next = 1;
  const ask = (method, params) =>
    new Promise((resolve) => {
      const id = next;
      next += 1;
      waiting.set(id, resolve);
      child.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', id, method, params })}\n`);
    });
  return { child, ask };
}

test('the MCP server lists the tools and passes a call to the game after its token', async () => {
  const local = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-mcp-'));
  fs.mkdirSync(path.join(local, 'KCDS', 'architect'), { recursive: true });
  fs.writeFileSync(path.join(local, 'KCDS', 'architect', 'ai-token'), 'secret-token');
  const game = standInGame('secret-token', (request) =>
    request.command === 'place'
      ? { ok: true, result: { id: 1, kind: 'prefab', at: request.args.at }, state: { steps: 1 } }
      : { ok: false, error: `unknown command "${request.command}"` },
  );
  await new Promise((resolve) => game.server.listen(0, '127.0.0.1', resolve));
  const port = game.server.address().port;
  const { child, ask } = mcp({ LOCALAPPDATA: local, KCDS_ARCHITECT_AI_PORT: String(port) });
  try {
    const started = await ask('initialize', { protocolVersion: '2025-06-18', capabilities: {} });
    assert.equal(started.result.serverInfo.name, 'kcds-world-architect');
    const listed = await ask('tools/list', {});
    // World Architect's commands, then the editors' steps (the Studio's tools): one server.
    assert.equal(listed.result.tools.length, TOOLS.length + STEP_TOOLS.length);
    assert.ok(listed.result.tools.some((tool) => tool.name === 'terrain'));
    assert.ok(listed.result.tools.some((tool) => tool.name === 'studio_state'));
    const placed = await ask('tools/call', {
      name: 'place',
      arguments: { name: 'structures/living/houses/house_1s_2r_a', at: { x: 1, y: 2 } },
    });
    assert.equal(placed.result.isError, false);
    assert.deepEqual(JSON.parse(placed.result.content[0].text).result.at, { x: 1, y: 2 });
    assert.deepEqual(game.asked[0].args.at, { x: 1, y: 2 });
    const wrong = await ask('tools/call', { name: 'undo', arguments: {} });
    assert.equal(wrong.result.isError, true);
    assert.match(wrong.result.content[0].text, /unknown command/);
  } finally {
    child.kill();
    game.server.close();
    fs.rmSync(local, { recursive: true, force: true });
  }
});

test("review's pictures come back as images, each with its measured light and what it means", async () => {
  const local = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-mcp-'));
  fs.mkdirSync(path.join(local, 'KCDS', 'architect'), { recursive: true });
  fs.writeFileSync(path.join(local, 'KCDS', 'architect', 'ai-token'), 'secret-token');
  // A 2 x 2 PNG, near black and blue (a room lit only by the sky).
  const dark =
    'iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEUlEQVR4nGNg45Jj45JjgFAABLYAuXWxB1gAAAAASUVORK5CYII=';
  const game = standInGame('secret-token', (request) =>
    request.command === 'review'
      ? {
          ok: true,
          result: {
            area: { at: { x: 1, y: 2, z: 3 }, radius: 10 },
            indoors: true,
            issues: [{ level: 'Warning', text: 'chandelier floats', id: 4 }],
            pictures: [{ label: 'from the north-east corner', png: dark, width: 2, height: 2 }],
          },
        }
      : { ok: false, error: 'no' },
  );
  await new Promise((resolve) => game.server.listen(0, '127.0.0.1', resolve));
  const port = game.server.address().port;
  const { child, ask } = mcp({ LOCALAPPDATA: local, KCDS_ARCHITECT_AI_PORT: String(port) });
  try {
    const answer = await ask('tools/call', { name: 'review', arguments: { at: { x: 1, y: 2 } } });
    const [label, image, notes, found, checklist] = answer.result.content;
    assert.equal(label.text, 'View from the north-east corner:');
    assert.deepEqual(image, { type: 'image', data: dark, mimeType: 'image/png' });
    assert.match(notes.text, /Measured: /);
    assert.match(notes.text, /Dim \(brightness 0\.042\)/);
    assert.equal(JSON.parse(found.text).issues[0].text, 'chandelier floats');
    assert.match(checklist.text, /as a player standing there would/);
  } finally {
    child.kill();
    game.server.close();
    fs.rmSync(local, { recursive: true, force: true });
  }
});

test('without a running game the MCP server says so instead of failing', async () => {
  const local = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-mcp-'));
  const { child, ask } = mcp({ LOCALAPPDATA: local, KCDS_ARCHITECT_AI_PORT: '1' });
  try {
    const answer = await ask('tools/call', { name: 'state', arguments: {} });
    assert.equal(answer.result.isError, true);
    assert.match(answer.result.content[0].text, /not running/);
  } finally {
    child.kill();
    fs.rmSync(local, { recursive: true, force: true });
  }
});
