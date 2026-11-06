'use strict';

// SDK types are reached through `kcds`. This keeps resource scripts, reference examples and guides
// from using a type by its bare name, which only fails once the script runs.

const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const assert = require('node:assert/strict');
const { DOMAINS, DOMAIN_OF, withNamespace } = require('./sdk_namespace.cjs');

const ROOT = path.resolve(__dirname, '../../../..');
const NAMES = [...DOMAIN_OF.keys()].sort((a, b) => b.length - a.length).join('|');
const QUALIFIED = /kcds\.[a-z]+\.[A-Za-z0-9]+/g;
const DESTRUCTURED = /(?:const|let|var)\s*\{([^}]*)\}\s*=\s*kcds\.[a-z]+/g;

// A member call, a construction, or a type annotation that names an SDK type on its own.
const BARE_USES = [
  new RegExp(`(?<![\\w$.'"\`/-])(${NAMES})\\s*\\.\\s*[A-Za-z_$]`, 'g'),
  new RegExp(`\\b(?:new|instanceof|typeof|extends)\\s+(${NAMES})\\b`, 'g'),
  new RegExp(`(?<=[\\w)\\]]\\??:\\s*|\\bas\\s+)(${NAMES})\\b(?=\\s*(?:[,)=;|\\[{]|=>|$))`, 'gm'),
];

function withoutComments(code) {
  return code.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/(^|\s)\/\/[^\n]*/g, '$1');
}

// What a piece of script code uses without kcds, apart from names it took from kcds itself.
function bareUses(code) {
  const source = withoutComments(code);
  const local = new Set();
  for (const match of source.matchAll(DESTRUCTURED)) {
    for (const part of match[1].split(',')) {
      local.add(part.split(':').at(-1).trim());
    }
  }
  const cleaned = source.replace(QUALIFIED, 'kcdsPath');
  const found = [];
  for (const pattern of BARE_USES) {
    for (const match of cleaned.matchAll(pattern)) {
      if (!local.has(match[1])) {
        found.push(match[0].trim());
      }
    }
  }
  return found;
}

function filesUnder(folder, keep) {
  const found = [];
  const walk = (current) => {
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const full = path.join(current, entry.name);
      if (entry.isDirectory()) {
        if (entry.name !== 'node_modules' && entry.name !== 'dist') {
          walk(full);
        }
      } else if (keep(entry.name)) {
        found.push(full);
      }
    }
  };
  walk(path.join(ROOT, folder));
  return found;
}

function report(where, code, failures) {
  for (const use of bareUses(code)) {
    failures.push(`${where}: ${use}`);
  }
}

test('bareUses finds what the checks below rely on', () => {
  assert.deepEqual(bareUses('Door.all();'), ['Door.a']);
  assert.deepEqual(bareUses('new Vector3(1, 2, 3);'), ['new Vector3']);
  assert.deepEqual(bareUses('const r = value as Quaternion;'), ['Quaternion']);
  assert.deepEqual(bareUses('function f(player: Player) {}'), ['Player']);
  assert.deepEqual(bareUses('kcds.world.Door.all(); new kcds.math.Vector3(1, 2, 3);'), []);
  assert.deepEqual(bareUses('const { Door } = kcds.world; Door.all();'), []);
  assert.deepEqual(bareUses('// Door.all() in a comment'), []);
  assert.deepEqual(bareUses("console.log('Door.all');"), []);
});

test('resource scripts reach the SDK through kcds', () => {
  const script = (name) => /\.(js|ts|cjs)$/.test(name) && !name.endsWith('.d.ts');
  const failures = [];
  for (const folder of ['Backend/resources', 'Dev/testkit/kcds_api_check']) {
    for (const file of filesUnder(folder, script)) {
      report(path.relative(ROOT, file), fs.readFileSync(file, 'utf8'), failures);
    }
  }
  assert.deepEqual(failures, []);
});

test('reference examples reach the SDK through kcds', () => {
  const failures = [];
  const docs = path.join(ROOT, 'Dev/tools/web/sdk-docs');
  for (const file of fs.readdirSync(docs).filter((name) => name.endsWith('.json'))) {
    const data = JSON.parse(fs.readFileSync(path.join(docs, file), 'utf8'));
    for (const section of ['members', 'clientMembers']) {
      for (const [name, entry] of Object.entries(data[section] ?? {})) {
        if (entry.example) {
          report(`${file} ${name}`, entry.example, failures);
        }
      }
    }
  }
  assert.deepEqual(failures, []);
});

const { docsDir, SKIP } = require('./docs_dir.cjs');

test('guide code reaches the SDK through kcds', { skip: !docsDir() && SKIP }, () => {
  const failures = [];
  const docs = path.relative(ROOT, docsDir());
  for (const file of filesUnder(docs, (name) => name.endsWith('.md'))) {
    const text = fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n');
    for (const block of text.matchAll(/```(?:js|javascript|ts|typescript)\n([\s\S]*?)```/g)) {
      report(path.relative(ROOT, file), block[1], failures);
    }
  }
  assert.deepEqual(failures, []);
});

test('withNamespace moves SDK types under kcds and leaves the rest', () => {
  const scope = withNamespace({ Door: 1, Vector3: 2, console: 3 });
  assert.equal(scope.Door, undefined);
  assert.equal(scope.Vector3, undefined);
  assert.equal(scope.console, 3);
  assert.equal(scope.kcds.world.Door, 1);
  assert.equal(scope.kcds.math.Vector3, 2);
  assert.deepEqual(Object.keys(scope.kcds), Object.keys(DOMAINS));
});

test('withNamespace keeps a value that changes later live', () => {
  const globals = { LocalPlayer: null };
  const scope = withNamespace(globals);
  assert.equal(scope.kcds.players.LocalPlayer, null);
  globals.LocalPlayer = 7;
  assert.equal(scope.kcds.players.LocalPlayer, 7);
});

test('a type belongs to one domain', () => {
  const all = Object.values(DOMAINS).flat();
  assert.equal(new Set(all).size, all.length);
});
