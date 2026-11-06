'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const readline = require('node:readline');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { encode, decode } = require('./codec.cjs');

test('production host routes console command arguments and removes stopped handlers', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-console-'));
  const folder = path.join(root, 'contract');
  fs.mkdirSync(folder);
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({ name: 'contract', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(folder, 'index.js'),
    `kcds.core.Events.on('consoleCommand', (command, args) => {
      console.info('received:' + JSON.stringify({ command, args }));
    });`,
  );
  const child = spawn(process.execPath, [path.join(__dirname, 'main.cjs'), root], {
    windowsHide: true,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  const frames = [];
  const errors = [];
  child.stderr.on('data', (chunk) => errors.push(String(chunk)));
  readline.createInterface({ input: child.stdout }).on('line', (line) => {
    frames.push(decode(Buffer.from(line, 'hex')));
  });
  const exited = once(child, 'exit');
  const deadline = setTimeout(() => child.kill('SIGKILL'), 10000);
  t.after(async () => {
    clearTimeout(deadline);
    if (child.exitCode === null) {
      child.stdin.end();
      await exited;
    }
    fs.rmSync(root, { recursive: true, force: true });
  });
  const send = (value) => child.stdin.write(encode(value).toString('hex') + '\n');
  const waitFor = async (predicate) => {
    const end = Date.now() + 5000;
    while (!frames.some(predicate)) {
      assert.ok(Date.now() < end, 'expected production host frame before timeout');
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
  };
  await waitFor((frame) => frame.op === 'ready');
  send({ op: 'console', text: '  inspect   alpha beta  ' });
  await waitFor((frame) => frame.text?.startsWith('received:'));
  const received = frames.filter((frame) => frame.text?.startsWith('received:'));
  assert.equal(received.length, 1);
  assert.deepEqual(JSON.parse(received[0].text.slice('received:'.length)), {
    command: 'inspect',
    args: ['alpha', 'beta'],
  });
  send({ op: 'console', text: 'stop contract' });
  send({ op: 'console', text: 'inspect after-stop' });
  child.stdin.end();
  const [code] = await exited;
  assert.equal(code, 0);
  assert.deepEqual(errors, []);
  assert.equal(frames.filter((frame) => frame.text?.startsWith('received:')).length, 1);
});
