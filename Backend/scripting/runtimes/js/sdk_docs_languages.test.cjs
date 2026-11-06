'use strict';

// Every example of the SDK reference (Dev/tools/web/sdk-docs/*.json) in Lua and C# too:
// the website shows each member's example with a tab per language. Checked here, as the
// guide's twins are (lua_guide_examples.test.cjs, csharp_guide_examples.test.cjs):
//
//   - every `example` has its `exampleLua` and `exampleCs`, unless the type is one only JavaScript
//     has (JS_ONLY_TYPES)
//   - every Lua example compiles as Lua 5.4 and names only SDK members a script has
//   - every C# example compiles against the KCDS SDK (Kcds.Sdk.dll), all in one build, as the
//     body of a method of a Script where it may await; the names it takes from its surroundings
//     (the player it is about, ...) are its `exampleCsGiven`, { name: C# type }. An example that
//     declares a class is compiled as it is.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {
  serverFixture,
  clientFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const DOCS = path.resolve(__dirname, '../../../../Dev/tools/web/sdk-docs');

// Types a Lua or C# script does not have, as "type: why".
const JS_ONLY_TYPES = new Map([['process', "Node's process: JavaScript resources only"]]);

const USINGS = [
  'Kcds',
  'Kcds.Core',
  'Kcds.Math',
  'Kcds.Players',
  'Kcds.Characters',
  'Kcds.World',
  'Kcds.Ui',
];

// Every curated example: { where, type, entry }.
function entries() {
  const found = [];
  for (const file of fs
    .readdirSync(DOCS)
    .filter((name) => name.endsWith('.json'))
    .sort()) {
    const data = JSON.parse(fs.readFileSync(path.join(DOCS, file), 'utf8'));
    for (const section of ['members', 'clientMembers']) {
      for (const [name, entry] of Object.entries(data[section] ?? {})) {
        if (entry.example) {
          found.push({
            where: `${file} ${data.type}.${name} (${section})`,
            type: data.type,
            entry,
          });
        }
      }
    }
  }
  return found;
}

test('sdk docs: every example is in Lua and C# too', () => {
  const problems = [];
  for (const { where, type, entry } of entries()) {
    if (JS_ONLY_TYPES.has(type)) {
      if (entry.exampleLua || entry.exampleCs) {
        problems.push(`${where}: ${type} is JavaScript only, but has a Lua or C# example`);
      }
      continue;
    }
    if (!entry.exampleLua) {
      problems.push(`${where}: no exampleLua`);
    }
    if (!entry.exampleCs) {
      problems.push(`${where}: no exampleCs`);
    }
  }
  assert.deepEqual(problems, []);
});

// The SDK tree of each side, as a Lua script reaches it: { domain: { Name: value } }.
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
    // A live value with nothing in it here (LocalPlayer before a game reports one).
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

function missingIn(tree, paths) {
  return paths.filter(({ domain, name, member }) => {
    const domainTree = tree[domain];
    if (!domainTree || !Object.hasOwn(domainTree, name)) {
      return true;
    }
    return member !== undefined && !hasMember(domainTree[name], member);
  });
}

test('sdk docs: every Lua example compiles and names only SDK members a script has', async (t) => {
  const { LuaFactory } = require('./languages/vendor/wasmoon/index.js');
  const wasm = path.join(__dirname, 'languages/vendor/wasmoon/glue.wasm');
  const engine = await new LuaFactory(wasm).createEngine();
  t.after(() => engine.global.close());
  const trees = await sideTrees(t);
  const problems = [];
  for (const { where, entry } of entries()) {
    if (!entry.exampleLua) {
      continue;
    }
    engine.global.set('source', entry.exampleLua);
    const problem = await engine.doString(
      'local chunk, problem = load(source, "=example", "t") return problem',
    );
    if (problem) {
      problems.push(`${where}: ${problem}`);
      continue;
    }
    for (const part of sideParts(entry.exampleLua)) {
      const paths = [];
      for (const match of part.matchAll(/kcds\.([a-z]+)\.([A-Za-z]\w*)(?:[.:]([A-Za-z_]\w*))?/g)) {
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
});

// An example of both sides (a "-- server" part, then a "-- client" part) as its parts, each
// checked against its own side; any other example as one part.
function sideParts(code) {
  const parts = code.split(/^(?=-- (?:server|client)\b)/m).filter((part) => part.trim());
  return parts.length > 0 ? parts : [code];
}

// A C# example as a source file of the check: a class as it is, other code as a method's body
// with the names it is given as fields.
function sourceOf(entry, namespace) {
  const usings = USINGS.map((name) => `global using ${name};`);
  const code = entry.exampleCs;
  const declares =
    /^(public |internal |static |sealed |abstract |partial )*(class|record|struct|enum|interface) /m;
  if (declares.test(code)) {
    return [...usings, `namespace ${namespace};`, '#line 1', code].join('\n');
  }
  const given = Object.entries(entry.exampleCsGiven ?? {}).map(
    ([name, type]) => `    private ${type} ${name} = ${type === 'JsValue' ? 'default' : 'null!'};`,
  );
  return [
    ...usings,
    `namespace ${namespace};`,
    '#pragma warning disable CS1998, CS0162, CS0168, CS0219, CS8321, CS0649, CS0169, CS8600, CS8602, CS8604, CS8618, CS8625',
    'public class Example : Script',
    '{',
    ...given,
    '',
    '    public async Task Snippet()',
    '    {',
    '#line 1',
    ...code.split('\n').map((line) => (line ? '        ' + line : line)),
    '#line default',
    '    }',
    '}',
  ].join('\n');
}

test('sdk docs: every C# example compiles against the SDK', () => {
  const { buildCSharp } = require('../../../../Dev/testkit/guide_scenarios/csharp.cjs');
  const sources = {};
  const places = new Map();
  let index = 0;
  for (const { where, entry } of entries()) {
    if (!entry.exampleCs) {
      continue;
    }
    index += 1;
    const file = `Reference_${index}.cs`;
    sources[file] = sourceOf(entry, `Reference.Example${index}`);
    places.set(file, where);
  }
  assert.ok(index > 0);
  try {
    buildCSharp('ReferenceExamples', sources);
  } catch (error) {
    // The build's errors name the file and line: say which member's example they are.
    let message = String(error.message);
    // the highest numbers first, so Reference_12.cs is not read as Reference_1.cs
    for (const [file, place] of [...places].reverse()) {
      message = message.replaceAll(file, place);
    }
    assert.fail(message);
  }
});
