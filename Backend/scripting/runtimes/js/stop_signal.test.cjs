'use strict';

// A Ctrl+C or a service manager's stop signal reaches the resource host together with the native
// server (systemd signals every process of the service). The host must not end on it: the native
// server stops it in order (serverStopping, then the input closes), so resources stop as usual and
// the next start finds no crash report. Windows has no such signal to send a child, so the test
// runs where POSIX signals are.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const readline = require('node:readline');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { encode, decode } = require('./codec.cjs');

const RESOURCE = `
kcds.core.Events.on('resourceStop', (name) => {
  if (name === 'mode') {
    console.info('stopped in order');
  }
});
`;

function frame(value) {
  return Buffer.from(encode(value)).toString('hex') + '\n';
}

async function waitFor(check, what) {
  const end = Date.now() + 10000;
  while (!check()) {
    assert.ok(Date.now() < end, what);
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
}

test(
  'a stop signal waits for the native server: resources stop, no crash report',
  { skip: process.platform === 'win32' && 'POSIX signals only' },
  async (t) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-stop-signal-'));
    const resources = path.join(root, 'resources');
    const folder = path.join(resources, 'mode');
    fs.mkdirSync(folder, { recursive: true });
    fs.writeFileSync(
      path.join(folder, 'package.json'),
      JSON.stringify({ name: 'mode', kcds: { serverScripts: ['index.js'] } }),
    );
    fs.writeFileSync(path.join(folder, 'index.js'), RESOURCE);
    const child = spawn(process.execPath, [path.join(__dirname, 'main.cjs'), resources], {
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    const frames = [];
    readline.createInterface({ input: child.stdout }).on('line', (line) => {
      frames.push(decode(Buffer.from(line, 'hex')));
    });
    const exited = once(child, 'exit');
    t.after(async () => {
      if (child.exitCode === null && child.signalCode === null) {
        child.kill('SIGKILL');
        await exited;
      }
      fs.rmSync(root, { recursive: true, force: true });
    });
    await waitFor(() => frames.some((value) => value.op === 'ready'), 'the host did not get ready');

    child.kill('SIGINT');
    child.kill('SIGTERM');
    await new Promise((resolve) => setTimeout(resolve, 300));
    assert.equal(child.exitCode, null, 'the host ended on the signal');
    assert.equal(child.signalCode, null, 'the host ended on the signal');

    child.stdin.write(frame({ op: 'serverStopping' }));
    child.stdin.end();
    const [code] = await exited;
    assert.equal(code, 0);
    const texts = frames.filter((value) => value.op === 'log').map((value) => value.text);
    assert.ok(texts.includes('stopped in order'), texts.join('\n'));
    const crashes = path.join(root, 'crashes');
    assert.deepEqual(fs.readdirSync(crashes), []);
  },
);
