'use strict';
const { spawn } = require('node:child_process');
const repoRoot = require('node:path').resolve(__dirname, '../..');
const readline = require('node:readline');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { encode, decode } = require(repoRoot + '/Backend/scripting/runtimes/js/codec.cjs');
const { ResourceBundleStore } = require(repoRoot + '/Backend/scripting/runtimes/js/bundles.cjs');
const { normalizeManifest } = require(repoRoot + '/Backend/scripting/runtimes/js/manifest.cjs');

const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-real-runtime-'));
fs.writeFileSync(
  path.join(folder, 'client.js'),
  `
  if (typeof process !== 'undefined') throw Error('Node access leaked');
  kcds.core.Events.toServer('smoke', {length: new kcds.math.Vector3(3,4,0).length, client: kcds.core.Runtime.isClient});
  kcds.core.Events.on('resourceStop', name => {if (name === 'smoke') kcds.core.Events.toServer('stopped');});
`,
);
const store = new ResourceBundleStore();
store.publish([
  {
    folder,
    manifest: normalizeManifest({ name: 'smoke', kcds: { clientScripts: ['client.js'] } }),
  },
]);
if (!process.argv[2]) throw Error('Usage: node test_resource_runtime.cjs <runtime-exe>');
const child = spawn(process.argv[2], [], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
const send = (value) => child.stdin.write(encode(value).toString('hex') + '\n');
let eventSeen = false;
let readySeen = false;
let readyCount = 0;
let startCount = 0;
let stopped = false;
let failure = false;
const timer = setTimeout(() => {
  failure = true;
  console.error('Runtime smoke timed out');
  child.kill();
}, 25000);
readline.createInterface({ input: child.stdout }).on('line', (line) => {
  try {
    const value = decode(Buffer.from(line, 'hex'));
    console.log(JSON.stringify(value));
    if (value.op === 'runtimeReady') send({ op: 'catalogInfo', ...store.info() });
    else if (value.op === 'catalogChunk')
      send({
        op: 'catalogData',
        requestId: value.requestId,
        ...store.catalogChunk(value.revision, value.offset, value.length),
      });
    else if (value.op === 'fileRequest')
      send({
        op: 'fileData',
        requestId: value.requestId,
        ...store.chunk(value.revision, value.hash, value.offset, value.length),
      });
    else if (value.op === 'event' && value.name === 'smoke') {
      const result = JSON.parse(value.payload);
      if (result.length !== 5 || result.client !== true) throw Error('Wrong SDK result');
      eventSeen = true;
      startCount++;
    } else if (value.op === 'resourcesReady') {
      readySeen = true;
      if (!eventSeen) throw Error('Resource did not execute');
      readyCount++;
      if (startCount !== 1) throw Error('Repeated catalog restarted the resource');
      if (readyCount === 1) send({ op: 'catalogInfo', ...store.info() });
      else if (readyCount === 2) send({ op: 'disconnect' });
      else throw Error('Unexpected ready acknowledgment');
    } else if (value.op === 'event' && value.name === 'stopped') {
      stopped = true;
      child.stdin.end();
    } else if (value.op === 'runtimeFailed') throw Error(value.text);
  } catch (error) {
    failure = true;
    console.error(String(error));
    child.kill();
  }
});
child.stderr.on('data', (data) => console.error(data.toString().slice(0, 2000)));
child.on('exit', (code) => {
  clearTimeout(timer);
  fs.rmSync(folder, { recursive: true, force: true });
  const ok =
    !failure &&
    eventSeen &&
    readySeen &&
    readyCount === 2 &&
    startCount === 1 &&
    stopped &&
    code === 0;
  console.log(JSON.stringify({ runtimeSmoke: ok, eventSeen, readySeen, stopped, exitCode: code }));
  process.exitCode = ok ? 0 : 1;
});
