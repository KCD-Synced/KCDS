'use strict';

// Every example of the SDK reference (Dev/tools/web/sdk-docs/*.json) is valid
// JavaScript as a resource would run it: a script body where `await` may be used.

const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');
const assert = require('node:assert/strict');

const DOCS = path.resolve(__dirname, '../../../../Dev/tools/web/sdk-docs');

function examplesOf(file, data) {
  const examples = [];
  for (const section of ['members', 'clientMembers']) {
    for (const [name, entry] of Object.entries(data[section] ?? {})) {
      if (entry.example) {
        examples.push({ where: `${file} ${data.type}.${name} (${section})`, code: entry.example });
      }
    }
  }
  return examples;
}

test('sdk docs: every example compiles', () => {
  const failures = [];
  let count = 0;
  for (const file of fs.readdirSync(DOCS).filter((name) => name.endsWith('.json'))) {
    const data = JSON.parse(fs.readFileSync(path.join(DOCS, file), 'utf8'));
    for (const { where, code } of examplesOf(file, data)) {
      count += 1;
      try {
        new vm.Script(`(async () => {\n${code}\n})`, { filename: where });
      } catch (error) {
        failures.push(`${where}: ${error.message}`);
      }
    }
  }
  assert.deepEqual(failures, []);
  assert.ok(count > 0);
});
