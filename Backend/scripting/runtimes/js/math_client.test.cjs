'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');
const { normalizeManifest } = require('./manifest.cjs');
const math = require('./math.cjs');
const verifyMath = require('../../../../Dev/testkit/math_contract.cjs');

test('downloaded CommonJS math has complete geometric behavior in the production client evaluator', async () => {
  const expectedChecks = verifyMath(math);
  const messages = [];
  const runtime = new BrowserResourceRuntime({ send: (message) => messages.push(message) });
  const files = new Map([
    [
      'math-contract.cjs',
      fs.readFileSync(path.resolve(__dirname, '../../../../Dev/testkit/math_contract.cjs')),
    ],
    [
      'main.js',
      Buffer.from(`
      const verify = require('./math-contract.cjs');
      const count = verify(kcds.math);
      kcds.core.Events.toServer('math-checked', count);
    `),
    ],
  ]);
  const resource = (hash) => ({
    hash,
    manifest: normalizeManifest({ name: 'math-check', kcds: { clientScripts: ['main.js'] } }),
    files: [...files.keys()].map((name) => ({ name })),
  });
  try {
    await runtime.apply({ resources: [resource('first')] }, (_, name) => files.get(name));
    await runtime.apply({ resources: [resource('second')] }, (_, name) => files.get(name));
    assert.ok(expectedChecks > 100);
    assert.equal(messages.length, 2);
    for (const message of messages) {
      assert.equal(message.name, 'math-checked');
      assert.equal(Number(message.payload), expectedChecks);
    }
  } finally {
    await runtime.disconnect();
  }
});
