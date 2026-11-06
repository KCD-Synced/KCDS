'use strict';

// Type-checks the examples of the public docs against the TypeScript declarations of the SDK
// every example of the reference (Dev/tools/web/sdk-docs) and every ```js block of
// KCDS-DOCS, and every ```ts block (the pages' TypeScript tabs) in strict mode. A call of a
// member the SDK does not have, or a global used on the wrong side, fails.
//
//   node Dev/tools/web/typecheck_examples.cjs <api.json> <types dir> <tsc>
//
// <api.json> is what extract_sdk_api.cjs wrote, <types dir> what build_typescript_declarations.py
// wrote from it (shared.d.ts, server-api.d.ts, client-api.d.ts), <tsc> the TypeScript compiler's
// bin/tsc. An example is a server script unless its member is a client one or it says
// "// client"; a page's block is a client script when it says "// client" or uses a client-only
// global. The names an example takes from its surroundings (player, npc, a helper of the reader's)
// are declared as any; the SDK's globals never are, so they are checked.

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const ROOT = path.resolve(__dirname, '../../..');
// The guides (KCDS-DOCS beside this repository, or KCDS_DOCS_DIR); without them only the
// reference's examples are checked.
const DOCS = require('../../../Backend/scripting/runtimes/js/docs_dir.cjs').docsDir();
const CURATED = path.join(__dirname, 'sdk-docs');
// Names a TypeScript program knows without a declaration file of ours.
const LANGUAGE = new Set(['undefined', 'require', 'module', 'exports', '__dirname', '__filename']);

function fail(message) {
  console.error(`typecheck_examples: ${message}`);
  process.exit(2);
}

function examplesOfReference(api) {
  const sides = new Map();
  for (const type of api.types) {
    for (const member of type.members) {
      sides.set(`${type.name}.${member.name}`, member.side);
    }
  }
  const examples = [];
  for (const file of fs.readdirSync(CURATED).filter((name) => name.endsWith('.json'))) {
    const data = JSON.parse(fs.readFileSync(path.join(CURATED, file), 'utf8'));
    for (const section of ['members', 'clientMembers']) {
      for (const [name, entry] of Object.entries(data[section] ?? {})) {
        if (!entry.example) {
          continue;
        }
        const side = section === 'clientMembers' ? 'client' : sides.get(`${data.type}.${name}`);
        examples.push({ where: `${data.type}.${name}`, code: entry.example, side });
      }
    }
  }
  return examples;
}

// The page blocks of one fence language: 'js' (checked loosely, as JavaScript) or 'ts' (strict).
function examplesOfPages(clientGlobals, language) {
  const examples = [];
  if (!DOCS) {
    return examples;
  }
  for (const file of fs.readdirSync(DOCS).filter((name) => name.endsWith('.md'))) {
    const text = fs.readFileSync(path.join(DOCS, file), 'utf8').replace(/\r\n/g, '\n');
    const pattern = new RegExp('```' + language + '\\n([\\s\\S]*?)```', 'g');
    let match = pattern.exec(text);
    let index = 0;
    while (match) {
      index += 1;
      examples.push({ where: `${file} ${language} block ${index}`, code: match[1], side: 'page' });
      match = pattern.exec(text);
    }
  }
  for (const example of examples) {
    const usesClient = [...clientGlobals].some((name) =>
      new RegExp(`\\b${name}\\.`).test(example.code),
    );
    example.side = /\/\/ client\b/.test(example.code) || usesClient ? 'client' : 'server';
  }
  return examples;
}

// The source of one side: every example in a function of its own, after the declarations of the
// names the examples take from their surroundings.
function programOf(examples, free) {
  const lines = [];
  for (const name of [...free].sort()) {
    lines.push(`declare const ${name}: any;`);
  }
  examples.forEach((example, i) => {
    example.line = lines.length + 2;
    lines.push(`// ${example.where}`);
    lines.push(`async function example${i}(): Promise<any> {`);
    lines.push(...example.code.split('\n'));
    lines.push('}');
  });
  return lines.join('\n') + '\n';
}

// `strict`: the TypeScript tabs, checked as a strict TypeScript project checks them.
function compile(tsc, dir, side, examples, free, strict) {
  const name = strict ? `${side}-strict` : side;
  fs.writeFileSync(path.join(dir, `${name}.ts`), programOf(examples, free));
  const config = {
    compilerOptions: {
      noEmit: true,
      target: 'es2022',
      lib: ['es2022'],
      types: [],
      strict,
      noImplicitAny: strict,
      allowUnreachableCode: true,
    },
    files: ['shared.d.ts', `${side}-api.d.ts`, `${name}.ts`],
  };
  fs.writeFileSync(path.join(dir, `tsconfig.${name}.json`), JSON.stringify(config));
  try {
    execFileSync(process.execPath, [tsc, '-p', path.join(dir, `tsconfig.${name}.json`)], {
      encoding: 'utf8',
    });
    return [];
  } catch (error) {
    return String(error.stdout)
      .split(/\r?\n/)
      .filter((line) => line.includes(`${name}.ts(`));
  }
}

function exampleAt(examples, line) {
  let found = null;
  for (const example of examples) {
    if (example.line <= line) {
      found = example;
    }
  }
  return found;
}

function check(tsc, dir, side, examples, sdkGlobals, strict = false) {
  const free = new Set();
  // Declare what the examples take from their surroundings, then check what is left.
  for (let round = 0; round < 6; round += 1) {
    const errors = compile(tsc, dir, side, examples, free, strict);
    let added = false;
    for (const error of errors) {
      const missing = /error TS2304: Cannot find name '([\w$]+)'/.exec(error);
      if (missing && !sdkGlobals.has(missing[1]) && !LANGUAGE.has(missing[1])) {
        if (!free.has(missing[1])) {
          free.add(missing[1]);
          added = true;
        }
      }
    }
    if (!added) {
      return errors.map((error) => {
        const line = Number(/\.ts\((\d+),/.exec(error)?.[1] ?? 0);
        const example = exampleAt(examples, line);
        return `${example ? example.where : '?'}: ${error.replace(/^.*?error /, '')}`;
      });
    }
  }
  return ['too many rounds'];
}

// An example with a "// client" or "// server" line holds the parts of both sides: each part is
// checked as a script of its side, the part before the first marker as the example's own side.
function splitBySide(examples) {
  const parts = [];
  for (const example of examples) {
    let side = example.side;
    let lines = [];
    let part = 0;
    const flush = () => {
      if (lines.join('').trim() !== '') {
        part += 1;
        parts.push({
          where: part > 1 ? `${example.where} (${side})` : example.where,
          side,
          code: lines.join('\n'),
        });
      }
      lines = [];
    };
    for (const line of example.code.split('\n')) {
      const marker = /^\/\/ (client|server)\b/.exec(line.trim());
      if (marker && marker[1] !== side) {
        flush();
        side = marker[1];
      }
      lines.push(line);
    }
    flush();
  }
  return parts;
}

function main() {
  const [apiFile, typesDir, tsc] = process.argv.slice(2);
  if (!apiFile || !typesDir || !tsc) {
    fail('usage: node typecheck_examples.cjs <api.json> <types dir> <tsc>');
  }
  const api = JSON.parse(fs.readFileSync(apiFile, 'utf8'));
  // The web platform's globals (fetch, WebSocket, ...) are not in the declarations: a script's own
  // TypeScript setup has them (lib dom or @types/node), so the examples take them as given.
  const sdkGlobals = new Set(
    api.types.filter((type) => type.group !== 'Scripting/Web platform').map((type) => type.name),
  );
  const clientGlobals = new Set(
    api.types.filter((type) => type.side === 'client').map((type) => type.name),
  );
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-examples-'));
  for (const file of ['shared.d.ts', 'server-api.d.ts', 'client-api.d.ts']) {
    fs.copyFileSync(path.join(typesDir, file), path.join(dir, file));
  }
  const loose = splitBySide([...examplesOfReference(api), ...examplesOfPages(clientGlobals, 'js')]);
  const strict = splitBySide(examplesOfPages(clientGlobals, 'ts'));
  let failures = 0;
  for (const [label, all, isStrict] of [
    ['', loose, false],
    [' TypeScript (strict)', strict, true],
  ]) {
    for (const side of ['server', 'client']) {
      const examples = all.filter((example) =>
        side === 'client' ? example.side === 'client' : example.side !== 'client',
      );
      const errors = check(tsc, dir, side, examples, sdkGlobals, isStrict);
      failures += errors.length;
      console.log(`${side}${label}: ${examples.length} examples, ${errors.length} errors`);
      for (const error of errors) {
        console.log(`  ${error}`);
      }
    }
  }
  fs.rmSync(dir, { recursive: true, force: true });
  process.exit(failures === 0 ? 0 : 1);
}

main();
