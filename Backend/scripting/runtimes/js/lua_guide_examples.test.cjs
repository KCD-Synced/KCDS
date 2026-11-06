'use strict';

// Every example of the public guides (KCDS-DOCS/*.md) in TypeScript and Lua too.
// A ```ts block right after a ```js block is that example in TypeScript, and a ```lua block right
// after those the example in Lua; the site shows them as one box with a tab per language.
// Checked here, as the JavaScript and TypeScript examples are type-checked
// (Dev/tools/web/typecheck_examples.cjs):
//
//   - every JavaScript block has its TypeScript twin right after it
//   - every JavaScript block has its Lua twin right after it (or after its TypeScript twin),
//     unless it is one of the few that only JavaScript has (JS_ONLY)
//   - every Lua block compiles as Lua 5.4
//   - every SDK path a Lua block names (kcds.<domain>.<Type>[.member]) is one a script has on the
//     server or in the player's game
//
// KCDS-DOCS/lua.md's examples also run (lua_docs_examples.test.cjs), and the first resource
// guide runs in both languages (scripts_first_resource.test.cjs).

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {
  serverFixture,
  clientFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const { docsDir, SKIP } = require('./docs_dir.cjs');

const DOCS = docsDir();
if (!DOCS) {
  test('guides: Lua examples', { skip: SKIP }, () => {});
  return;
}

// JavaScript blocks with no Lua twin, as "page block-number: why". Empty: every guide example is
// in all three languages.
const JS_ONLY = new Map([]);

// The page's code blocks in order: { info, code, start, end } with the fence's info string.
function blocksOf(text) {
  const blocks = [];
  const pattern = /^```([^\n]*)\n([\s\S]*?)^```[ \t]*$/gm;
  let match = pattern.exec(text);
  while (match) {
    blocks.push({
      info: match[1].trim(),
      code: match[2],
      start: match.index,
      end: pattern.lastIndex,
    });
    match = pattern.exec(text);
  }
  return blocks;
}

function languageOf(block) {
  return block.info.split(/\s+/)[0];
}

function pages() {
  return fs
    .readdirSync(DOCS)
    .filter((name) => name.endsWith('.md'))
    .map((name) => ({
      name,
      text: fs.readFileSync(path.join(DOCS, name), 'utf8').replaceAll('\r\n', '\n'),
    }));
}

function adjacent(text, block, next) {
  return Boolean(next) && text.slice(block.end, next.start).trim() === '';
}

// The JavaScript block at `index`, or its TypeScript twin right after it: the block the example's
// Lua twin follows.
function lastOfScript(text, blocks, index) {
  const next = blocks[index + 1];
  return adjacent(text, blocks[index], next) && languageOf(next) === 'ts' ? index + 1 : index;
}

test('guides: every JavaScript example has its TypeScript twin right after it', () => {
  const problems = [];
  for (const { name, text } of pages()) {
    const blocks = blocksOf(text);
    let jsIndex = 0;
    blocks.forEach((block, index) => {
      if (languageOf(block) !== 'js') {
        return;
      }
      jsIndex += 1;
      if (lastOfScript(text, blocks, index) === index) {
        problems.push(`${name} js block ${jsIndex}: no TypeScript twin right after it`);
      }
    });
  }
  assert.deepEqual(problems, []);
});

test('guides: every JavaScript example has its Lua twin next to it', () => {
  const problems = [];
  for (const { name, text } of pages()) {
    const blocks = blocksOf(text);
    let jsIndex = 0;
    blocks.forEach((block, index) => {
      if (languageOf(block) !== 'js') {
        return;
      }
      jsIndex += 1;
      const where = `${name} js block ${jsIndex}`;
      const last = lastOfScript(text, blocks, index);
      const next = blocks[last + 1];
      const paired = adjacent(text, blocks[last], next) && languageOf(next) === 'lua';
      if (JS_ONLY.has(`${name} ${jsIndex}`)) {
        if (paired) {
          problems.push(`${where}: listed as JavaScript only, but has a Lua twin`);
        }
        return;
      }
      if (!paired) {
        problems.push(`${where}: no Lua twin right after it`);
      }
    });
  }
  assert.deepEqual(problems, []);
});

// The SDK tree of each side, as a script reaches it: { domain: { Name: value } }.
async function sideTrees(t) {
  const server = serverFixture(t);
  const folder = path.join(server.root, 'probe');
  fs.mkdirSync(folder);
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({ name: 'probe', kcds: { server: 'main.lua' } }),
  );
  fs.writeFileSync(path.join(folder, 'main.lua'), '');
  await server.host.startAll();
  const client = clientFixture(t);
  await client.runtime.apply(
    { resources: [client.resource('probe', { lua: '' }, {}, 'probe', 'lua')] },
    client.read,
  );
  return {
    server: server.host.resources.get('probe').vm.kcds,
    client: client.runtime.resources.get('probe').lua.scope.kcds,
  };
}

function hasMember(value, member) {
  if (value === null || value === undefined) {
    // A live value with nothing in it here (LocalPlayer before a game reports one): its members
    // cannot be read off it, so they are not checked.
    return true;
  }
  if (member === 'new') {
    return typeof value === 'function';
  }
  let object = value;
  while (object && object !== Object.prototype && object !== Function.prototype) {
    if (Object.getOwnPropertyNames(object).includes(member)) {
      return true;
    }
    object = Object.getPrototypeOf(object);
  }
  return false;
}

// Whether every path is one the tree has.
function missingIn(tree, paths) {
  return paths.filter(({ domain, name, member }) => {
    const domainTree = tree[domain];
    if (!domainTree || !Object.hasOwn(domainTree, name)) {
      return true;
    }
    return member !== undefined && !hasMember(domainTree[name], member);
  });
}

test('guides: every Lua example compiles and names only SDK members a script has', async (t) => {
  const { LuaFactory } = require('./languages/vendor/wasmoon/index.js');
  const wasm = path.join(__dirname, 'languages/vendor/wasmoon/glue.wasm');
  const engine = await new LuaFactory(wasm).createEngine();
  t.after(() => engine.global.close());
  const trees = await sideTrees(t);
  const problems = [];
  let count = 0;
  for (const { name, text } of pages()) {
    let luaIndex = 0;
    for (const block of blocksOf(text)) {
      if (languageOf(block) !== 'lua') {
        continue;
      }
      luaIndex += 1;
      count += 1;
      const where = `${name} lua block ${luaIndex}`;
      engine.global.set('source', block.code);
      const problem = await engine.doString(
        'local chunk, problem = load(source, "=example", "t") return problem',
      );
      if (problem) {
        problems.push(`${where}: ${problem}`);
        continue;
      }
      const paths = [];
      for (const match of block.code.matchAll(
        /kcds\.([a-z]+)\.([A-Za-z]\w*)(?:[.:]([A-Za-z_]\w*))?/g,
      )) {
        paths.push({ domain: match[1], name: match[2], member: match[3], text: match[0] });
      }
      const onServer = missingIn(trees.server, paths);
      const onClient = missingIn(trees.client, paths);
      if (onServer.length > 0 && onClient.length > 0) {
        const missing = onServer.length <= onClient.length ? onServer : onClient;
        problems.push(`${where}: no ${missing.map((entry) => entry.text).join(', ')}`);
      }
    }
  }
  assert.deepEqual(problems, []);
  assert.ok(count > 0);
});
