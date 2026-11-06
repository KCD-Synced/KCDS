'use strict';
// Real-WebView acceptance for the client timer contract.
//
// Drives the built kcd2s-resource-runtime WebView worker with a temporary
// resource that runs the shared Dev/testkit/timer_contract.cjs fixture and,
// crucially, observes the ACTUAL browser timer handle types. The browser's
// ambient setTimeout/setInterval return numeric ids, so the resource runtime
// must hand those numbers straight back. A reload with a changed catalog proves
// generation-1 timers never fire into generation 2. No Node timer substitute
// is used: every assertion below runs inside the real WebView.
const { spawn } = require('node:child_process');
const repoRoot = require('node:path').resolve(__dirname, '../..');
const readline = require('node:readline');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { encode, decode } = require(repoRoot + '/Backend/scripting/runtimes/js/codec.cjs');
const { ResourceBundleStore } = require(repoRoot + '/Backend/scripting/runtimes/js/bundles.cjs');
const { normalizeManifest } = require(repoRoot + '/Backend/scripting/runtimes/js/manifest.cjs');

if (!process.argv[2]) throw Error('Usage: node test_client_timer_contract.cjs <runtime-exe>');

// Single source of truth: embed the shared fixture, drop its CommonJS export.
const fixtureSource = fs
  .readFileSync(path.join(repoRoot, 'Dev/testkit/timer_contract.cjs'), 'utf8')
  .replace(/module\.exports\s*=\s*verifyTimerContract;?\s*$/m, '');

function writeResource(folder, generation) {
  fs.writeFileSync(
    path.join(folder, 'client.js'),
    `${fixtureSource}
    if (typeof process !== 'undefined') throw Error('Node access leaked');
    const generation = ${generation};
    kcds.core.Events.on('resourceStop', (name) => {
      if (name === 'timercontract') kcds.core.Events.toServer('stopped', { generation });
    });
    // Observe real browser handle identity before anything else runs.
    const timeoutHandle = setTimeout(() => { kcds.core.Events.toServer('leak', { generation }); }, 300);
    const intervalHandle = setInterval(() => { kcds.core.Events.toServer('leak', { generation }); }, 120);
    const observed = {
      generation,
      timeoutType: typeof timeoutHandle,
      intervalType: typeof intervalHandle,
      isClient: kcds.core.Runtime.isClient,
      isServer: kcds.core.Runtime.isServer,
      apiVersion: kcds.core.Runtime.apiVersion,
      consoleOk: ['log', 'info', 'warn', 'error', 'debug'].every((m) => typeof console[m] === 'function'),
    };
    clearTimeout(timeoutHandle);
    clearInterval(intervalHandle);
    console.log('timer contract generation ' + generation);
    const env = { setTimeout, setInterval, clearTimeout, clearInterval, queueMicrotask, console, kcds };
    verifyTimerContract(env)
      .then((checks) => {
        if (generation === 1) {
          // These handles deliberately remain active when generation 1 stops.
          // The host waits past their deadlines after reload to verify cleanup.
          setTimeout(() => kcds.core.Events.toServer('leak', { generation }), 1000);
          setInterval(() => kcds.core.Events.toServer('leak', { generation }), 1000);
        }
        kcds.core.Events.toServer('contract', { ...observed, checks });
      })
      .catch((error) => kcds.core.Events.toServer('contract', { ...observed, error: String(error) }));
    `,
  );
}

const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-timer-contract-'));
writeResource(folder, 1);
const store = new ResourceBundleStore();
const manifest = normalizeManifest({
  name: 'timercontract',
  kcds: { clientScripts: ['client.js'] },
});
store.publish([{ folder, manifest }]);

const child = spawn(process.argv[2], [], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
const send = (value) => child.stdin.write(encode(value).toString('hex') + '\n');

const results = [];
let leaked = false;
let reloaded = false;
let stopped = false;
let failure = null;
const timer = setTimeout(() => {
  failure = 'timed out waiting for WebView contract';
  child.kill();
}, 40000);

readline.createInterface({ input: child.stdout }).on('line', (line) => {
  let value;
  try {
    value = decode(Buffer.from(line, 'hex'));
  } catch {
    return;
  }
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
  } else if (value.op === 'event' && value.name === 'contract') {
    const result = JSON.parse(value.payload);
    if (results.some((previous) => previous.generation === result.generation)) {
      failure = 'duplicate timer contract generation';
      child.kill();
      return;
    }
    results.push(result);
    if (result.generation === 1 && !reloaded) {
      // resourcesReady reports script startup, before its asynchronous checks
      // finish. Reload only after the first contract has actually completed.
      reloaded = true;
      writeResource(folder, 2);
      store.publish([{ folder, manifest }]);
      send({ op: 'catalogInfo', ...store.info() });
    } else if (result.generation === 2) {
      // Observe beyond generation 1's still-active timer deadlines.
      setTimeout(() => send({ op: 'disconnect' }), 1300);
    }
  } else if (value.op === 'event' && value.name === 'leak') {
    leaked = true;
  } else if (value.op === 'event' && value.name === 'stopped') {
    if (JSON.parse(value.payload).generation === 2) {
      stopped = true;
      child.stdin.end();
    }
  } else if (value.op === 'runtimeFailed') {
    failure = value.text;
    child.kill();
  }
});
child.stderr.on('data', (data) => process.stderr.write(data.toString().slice(0, 2000)));
child.on('exit', (code) => {
  clearTimeout(timer);
  fs.rmSync(folder, { recursive: true, force: true });
  const numericIds =
    results.length > 0 &&
    results.every((r) => r.timeoutType === 'number' && r.intervalType === 'number');
  const contractPassed =
    results.length === 2 &&
    results[0].generation === 1 &&
    results[1].generation === 2 &&
    results.every((r) => r.checks >= 20 && !r.error);
  const environmentOk = results.every(
    (r) =>
      r.isClient === true &&
      r.isServer === false &&
      r.consoleOk &&
      typeof r.apiVersion === 'string',
  );
  const ok =
    !failure && numericIds && contractPassed && environmentOk && !leaked && stopped && code === 0;
  console.log(
    JSON.stringify(
      {
        clientTimerContract: ok,
        numericIds,
        contractPassed,
        environmentOk,
        leaked,
        stopped,
        failure,
        exitCode: code,
        results,
      },
      null,
      2,
    ),
  );
  process.exitCode = ok ? 0 : 1;
});
