'use strict';

// Every Lua example of the public Lua guide (KCDS-DOCS/lua.md) runs as a resource
// would run it, on its side, without an error. A block that starts with `-- <path>.lua` is a module
// file the other blocks may require; one that starts with `-- client` is a client script.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { normalizeManifest } = require('./manifest.cjs');
const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');

const { docsDir, SKIP } = require('./docs_dir.cjs');

const DOCS = docsDir();
if (!DOCS) {
  test('the Lua guide examples', { skip: SKIP }, () => {});
  return;
}
const GUIDE = path.join(DOCS, 'lua.md');

function luaBlocks() {
  const text = fs.readFileSync(GUIDE, 'utf8').replaceAll('\r\n', '\n');
  return [...text.matchAll(/```lua\n([\s\S]*?)```/g)].map((match) => match[1]);
}

function modulesOf(blocks) {
  const modules = {};
  for (const code of blocks) {
    const header = /^-- ([A-Za-z0-9_/]+\.lua)\n/.exec(code);
    if (header) {
      modules[header[1]] = code;
    }
  }
  return modules;
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

test('the Lua guide has examples for both sides', () => {
  const blocks = luaBlocks();
  assert.ok(blocks.length >= 8, `${blocks.length} examples`);
  assert.ok(blocks.some((code) => code.startsWith('-- client')));
});

test('every server example of the Lua guide runs', async (t) => {
  const realFetch = globalThis.fetch;
  globalThis.fetch = async () => Response.json({ players: 3 });
  t.after(() => {
    globalThis.fetch = realFetch;
  });
  const f = serverFixture(t);
  const blocks = luaBlocks();
  const modules = modulesOf(blocks);
  const names = [];
  blocks.forEach((code, index) => {
    if (code.startsWith('-- client') || /^-- [A-Za-z0-9_/]+\.lua\n/.test(code)) {
      return;
    }
    const name = `guide-${index}`;
    names.push(name);
    const directory = path.join(f.root, name);
    fs.mkdirSync(path.join(directory, 'shared'), { recursive: true });
    fs.writeFileSync(
      path.join(directory, 'package.json'),
      JSON.stringify({ name, kcds: { server: 'main.lua' } }),
    );
    fs.writeFileSync(path.join(directory, 'main.lua'), code);
    for (const [file, source] of Object.entries(modules)) {
      fs.writeFileSync(path.join(directory, file), source);
    }
  });
  await f.host.startAll();
  await delay(50);
  for (const name of names) {
    const lines = f.linesOf(name);
    assert.ok(f.host.resources.has(name), `${name} stopped:\n${lines.join('\n')}`);
    // The pcall example prints the error it catches on purpose.
    const unexpected = lines.filter((line) => {
      return /error|failed/i.test(line) && !line.startsWith('false\tTypeError: Chat text');
    });
    assert.deepEqual(unexpected, [], `${name}:\n${lines.join('\n')}`);
  }
  const all = names.flatMap((name) => f.linesOf(name));
  assert.ok(all.includes('players online\t3'), all.join('\n'));
  assert.ok(all.includes('status\t200\t3'));
  assert.ok(all.includes('42'));
  assert.ok(all.includes('false\tTypeError: Chat text must be string'));
});

test('the guide hello resource answers its command and the count request', async (t) => {
  const f = serverFixture(t);
  const [server] = luaBlocks();
  const directory = path.join(f.root, 'hello-lua');
  fs.mkdirSync(directory);
  fs.writeFileSync(
    path.join(directory, 'package.json'),
    JSON.stringify({ name: 'hello-lua', kcds: { server: 'server.lua' } }),
  );
  fs.writeFileSync(path.join(directory, 'server.lua'), server);
  await f.host.startAll();
  f.report(7);
  const api = f.host.makeAPI();
  const player = api.Player.getById(7);
  await api.Events.emit('playerCommand', player, 'Hello', ['Good', 'morning']);
  const chat = f.sent.filter((message) => message.op === 'chat' && message.target === 7);
  assert.ok(
    chat.some((message) => message.text === 'Good morning'),
    JSON.stringify(chat),
  );
});

test('the guide client example runs in a client runtime', async (t) => {
  const code = luaBlocks().find((block) => block.startsWith('-- client'));
  const logs = [];
  const runtime = new BrowserResourceRuntime({
    send: () => {},
    log: (resource, text) => logs.push(String(text)),
  });
  t.after(() => runtime.disconnect());
  const bytes = new TextEncoder().encode(code);
  const resource = {
    hash: 'hello-lua',
    manifest: normalizeManifest({ name: 'hello-lua', kcds: { client: 'client.lua' } }),
    files: [{ name: 'client.lua' }],
  };
  await runtime.apply({ resources: [resource] }, () => bytes);
  assert.ok(logs.includes('client resource started'), logs.join('\n'));
});
