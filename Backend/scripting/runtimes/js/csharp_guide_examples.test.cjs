'use strict';

// Every guide example of the public docs (KCDS-DOCS/*.md) in C# too. A ```cs block
// right after an example's ```lua block is that example in C#, and the site shows the three as one
// box with a tab per language. Checked here:
//
//   - every JavaScript example with a Lua twin has its C# twin right after that, and the few that
//     only JavaScript has (lua_guide_examples.test.cjs, JS_ONLY) have none
//   - every C# block compiles against the KCDS SDK (Kcds.Sdk.dll), all in one build, with the
//     SDK's namespaces as global usings, as the resource template has them
//     (Backend/scripting/runtimes/csharp/Template). A block that declares a class is compiled as
//     it is; any other block is the body of a method of a Script, where it may await.
//
// KCDS-DOCS/csharp.md's own examples are C# only and compile the same way.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const { docsDir, SKIP } = require('./docs_dir.cjs');

const DOCS = docsDir();
if (!DOCS) {
  test('guides: C# examples', { skip: SKIP }, () => {});
  return;
}

// JavaScript blocks with no Lua and no C# twin: "page block-number" (lua_guide_examples.test.cjs).
const JS_ONLY = new Set([]);

// The SDK's namespaces, as the template's Directory.Build.props lists them.
const USINGS = [
  'Kcds',
  'Kcds.Core',
  'Kcds.Math',
  'Kcds.Players',
  'Kcds.Characters',
  'Kcds.World',
  'Kcds.Ui',
];

// The names an example takes from its surroundings, as the guide's text describes them (a player
// the example is about, ...), declared for every example that is not a whole class.
const SURROUNDINGS = [
  'Player player = null!;',
  'Player admin = null!;',
  'Player guard = null!;',
  'Player prisoner = null!;',
  'Player condemned = null!;',
  'Npc npc = null!;',
  // a table the page made above (tabletop.md)
  'Tabletop board = null!;',
  'Player thief = null!;',
  'double horseId;',
  'double playerId;',
  'double guardId;',
  // the staff's Steam ids (client-camera.md), a resource's own bans (licence.md)
  'HashSet<string> staff = new();',
  'HashSet<string> myBans = new();',
  // what a resource keeps for players who left (players-connecting.md)
  'Dictionary<string, JsValue> savedInventories = new();',
  'Vector3 position = null!;',
  // the horse and the places a page's example is about (horses-dogs.md, animations.md, world.md)
  'Horse horse = null!;',
  'Vector3 stall = null!;',
  'Vector3 rack = null!;',
  // your launcher's one-time tickets (players-connecting.md)
  'interface ILauncherTickets { Task<bool> Redeem(string? token, string? steamId); }',
  'ILauncherTickets launcherTickets = null!;',
  // the studio export's prop and animation (asset-studio.md)
  'object prop = null!;',
  'object animation = null!;',
  'Vector3 counter = null!;',
  'JsValue data;',
  // a resource's own function answering a task (debugging.md)
  'Func<Player, JsValue, Task> buy = null!;',
  // a resource's own lookup of an account (join-queue.md)
  'Func<JsValue, Task<JsValue>> lookUpAccount = null!;',
];

// Examples with a Lua twin and no C# one, as "page block-number: why" (JavaScript blocks counted).
// The page says so where the C# twin would be.
const NOT_IN_CSHARP = new Map([
  ['playground.md 1', 'runs code typed in the console; C# is compiled before it runs'],
]);

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

// Whether block `next` follows `block` directly, with only white space between them.
function adjacent(text, block, next) {
  return next !== undefined && text.slice(block.end, next.start).trim() === '';
}

test('guides: every JavaScript example has its C# twin after its Lua one', () => {
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
      // The example's TypeScript twin, when it has one, comes before the Lua one.
      const typescript = blocks[index + 1];
      const last =
        adjacent(text, block, typescript) && languageOf(typescript) === 'ts' ? index + 1 : index;
      const script = blocks[last];
      const lua = blocks[last + 1];
      const csharp = blocks[last + 2];
      const hasLua = adjacent(text, script, lua) && languageOf(lua) === 'lua';
      const after = hasLua ? lua : script;
      const next = hasLua ? csharp : lua;
      const hasCsharp = adjacent(text, after, next) && languageOf(next) === 'cs';
      if (JS_ONLY.has(`${name} ${jsIndex}`) || NOT_IN_CSHARP.has(`${name} ${jsIndex}`)) {
        if (hasCsharp) {
          problems.push(`${where}: listed as JavaScript only, but has a C# twin`);
        }
        return;
      }
      if (!hasCsharp) {
        problems.push(`${where}: no C# twin right after its Lua twin`);
      }
    });
  }
  assert.deepEqual(problems, []);
});

// A C# block as a source file of the check: a class as it is, other code as a method's body.
function sourceOf(block, namespace) {
  const code = block.code;
  const usings = USINGS.map((name) => `global using ${name};`);
  const own = [];
  const body = [];
  for (const line of code.split('\n')) {
    if (/^using [A-Za-z.]+;\s*(\/\/.*)?$/.test(line)) {
      own.push(line);
    } else {
      body.push(line);
    }
  }
  const declares =
    /^(public |internal |static |sealed |abstract |partial )*(class|record|struct|enum|interface) /m;
  const lines = [...own, `namespace ${namespace};`, ''];
  if (declares.test(body.join('\n'))) {
    // An error's line is the block's own line.
    lines.push(`#line ${own.length + 1}`, ...body);
  } else {
    lines.push(
      '#pragma warning disable CS1998, CS0162, CS0168, CS0219, CS8321, CS0649, CS0169',
      'public class Example : Script',
      '{',
      ...SURROUNDINGS.map((field) => '    private ' + field),
      '',
      '    public async Task Snippet()',
      '    {',
      '#line 1',
      ...body.map((line) => (line ? '        ' + line : line)),
      '#line default',
      '    }',
      '}',
    );
  }
  return { usings, text: lines.join('\n') };
}

test('guides: every C# example compiles against the SDK', (t) => {
  const { buildCSharp } = require('../../../../Dev/testkit/guide_scenarios/csharp.cjs');
  const sources = {};
  const places = new Map();
  let count = 0;
  for (const { name, text } of pages()) {
    let index = 0;
    for (const block of blocksOf(text)) {
      if (languageOf(block) !== 'cs') {
        continue;
      }
      index += 1;
      count += 1;
      const file = `${name.replace(/\W/g, '_')}_${index}.cs`;
      const { usings, text: source } = sourceOf(block, `Docs.${name.replace(/\W/g, '_')}_${index}`);
      sources[file] = source;
      sources['GlobalUsings.cs'] = usings.join('\n') + '\n';
      places.set(file, `${name} cs block ${index}`);
    }
  }
  if (count === 0) {
    t.skip('no C# examples yet');
    return;
  }
  try {
    buildCSharp('DocsExamples', sources);
  } catch (error) {
    // The build's errors name the file and line: say which page and block they are.
    let message = String(error.message);
    for (const [file, place] of places) {
      message = message.replaceAll(file, place);
    }
    assert.fail(message);
  }
});
