'use strict';

// A C# client resource in the real resource runtime: the runtime program serves its
// .NET files from the dotnet folder next to it, the worker loads them and runs the resource's
// assembly over the client SDK. The resource reports to the server and stops cleanly.
//
//   node Dev/tools/test_resource_runtime_csharp.cjs <kcd2s-resource-runtime.exe>

const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const readline = require('node:readline');

const repoRoot = path.resolve(__dirname, '../..');
const { encode, decode } = require(repoRoot + '/Backend/scripting/runtimes/js/codec.cjs');
const { ResourceBundleStore } = require(repoRoot + '/Backend/scripting/runtimes/js/bundles.cjs');
const { normalizeManifest } = require(repoRoot + '/Backend/scripting/runtimes/js/manifest.cjs');
const { buildCSharp } = require(repoRoot + '/Dev/testkit/guide_scenarios/csharp.cjs');

const SOURCE = `
using Kcds;
using Kcds.Core;
using Kcds.Math;

public class Smoke : Script
{
    public override void OnStart()
    {
        var length = new Vector3(3, 4, 0).Length;
        Events.ToServer("csharpSmoke", new { length, client = Runtime.IsClient });
        Events.On("resourceStop", (string name) =>
        {
            if (name == "csharp-smoke")
            {
                Events.ToServer("csharpStopped");
            }
        });
    }
}
`;

if (!process.argv[2]) {
  throw Error('Usage: node test_resource_runtime_csharp.cjs <runtime-exe>');
}
const dotnet = path.join(path.dirname(process.argv[2]), 'dotnet', '_framework', 'dotnet.js');
if (!fs.existsSync(dotnet)) {
  throw Error(`No .NET runtime next to the program: ${dotnet}`);
}

const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-csharp-runtime-'));
fs.mkdirSync(path.join(folder, 'client'));
fs.writeFileSync(
  path.join(folder, 'client', 'Smoke.dll'),
  buildCSharp('Smoke', { 'Smoke.cs': SOURCE }),
);
const store = new ResourceBundleStore();
store.publish([
  {
    folder,
    manifest: normalizeManifest({ name: 'csharp-smoke', kcds: { client: 'client/Smoke.dll' } }),
  },
]);

const child = spawn(process.argv[2], [], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
const send = (value) => child.stdin.write(encode(value).toString('hex') + '\n');
let reported = null;
let stopped = false;
let failure = null;
const timer = setTimeout(() => {
  failure = 'timed out';
  child.kill();
}, 40000);

function answer(value) {
  if (value.op === 'runtimeReady') {
    send({ op: 'catalogInfo', ...store.info() });
  } else if (value.op === 'catalogChunk') {
    send({
      op: 'catalogData',
      requestId: value.requestId,
      ...store.catalogChunk(value.revision, value.offset, value.length),
    });
  } else if (value.op === 'fileRequest') {
    send({
      op: 'fileData',
      requestId: value.requestId,
      ...store.chunk(value.revision, value.hash, value.offset, value.length),
    });
  } else if (value.op === 'event' && value.name === 'csharpSmoke') {
    reported = JSON.parse(value.payload);
    send({ op: 'disconnect' });
  } else if (value.op === 'event' && value.name === 'csharpStopped') {
    stopped = true;
    child.stdin.end();
  } else if (value.op === 'runtimeFailed') {
    throw Error(value.text);
  }
}

readline.createInterface({ input: child.stdout }).on('line', (line) => {
  try {
    const value = decode(Buffer.from(line, 'hex'));
    if (value.op === 'log') {
      console.log(`[${value.resource}] ${value.text}`);
    }
    answer(value);
  } catch (error) {
    failure = String(error);
    child.kill();
  }
});
child.stderr.on('data', (data) => console.error(data.toString().slice(0, 2000)));
child.on('exit', (code) => {
  clearTimeout(timer);
  fs.rmSync(folder, { recursive: true, force: true });
  const ran = reported !== null && reported.length === 5 && reported.client === true;
  const ok = failure === null && ran && stopped && code === 0;
  console.log(JSON.stringify({ csharpRuntime: ok, reported, stopped, failure, exitCode: code }));
  process.exitCode = ok ? 0 : 1;
});
