'use strict';

// the server's map is known before the resources start. The native server passes
// --map=<level> to the host, so World.level and the level's own areas are ready while a resource
// loads and in resourceStart, not only after the hostSettings message.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const readline = require('node:readline');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { decode } = require('./codec.cjs');

const RESOURCE = `
console.info('load:' + kcds.world.World.level + ':' + (kcds.world.Area.all('level').length > 0));
kcds.core.Events.on('resourceStart', (name) => {
  if (name === 'mode') {
    console.info('start:' + kcds.world.World.level + ':' + (kcds.world.Area.all('level').length > 0));
  }
});
`;

async function runHost(t, extra) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-level-start-'));
  const folder = path.join(root, 'mode');
  fs.mkdirSync(folder);
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({ name: 'mode', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(path.join(folder, 'index.js'), RESOURCE);
  const child = spawn(process.execPath, [path.join(__dirname, 'main.cjs'), root, ...extra], {
    windowsHide: true,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  const frames = [];
  readline.createInterface({ input: child.stdout }).on('line', (line) => {
    frames.push(decode(Buffer.from(line, 'hex')));
  });
  const exited = once(child, 'exit');
  t.after(async () => {
    if (child.exitCode === null) {
      child.stdin.end();
      await exited;
    }
    fs.rmSync(root, { recursive: true, force: true });
  });
  const end = Date.now() + 10000;
  while (!frames.some((frame) => frame.op === 'ready')) {
    assert.ok(Date.now() < end, 'the host did not get ready');
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  child.stdin.end();
  await exited;
  return frames.filter((frame) => frame.op === 'log').map((frame) => frame.text);
}

test('with --map the level and its areas are ready while resources load and start', async (t) => {
  const texts = await runHost(t, ['1000:16', '--map=trosecko']);
  assert.ok(texts.includes('load:trosecko:true'), texts.join('\n'));
  assert.ok(texts.includes('start:trosecko:true'), texts.join('\n'));
});

test('without --map the level is empty until hostSettings, as before', async (t) => {
  const texts = await runHost(t, []);
  assert.ok(texts.includes('load::false'), texts.join('\n'));
});
