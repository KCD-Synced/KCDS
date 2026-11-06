'use strict';

// Shared fixtures for the 0.1.1 guide scenario suites.
//
// Each scenario drives a production runtime the way a resource author would: the trusted server
// ResourceHost over real resource folders, the production main.cjs process over its IPC framing,
// or the browser BrowserResourceRuntime over an in-memory catalog.

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const readline = require('node:readline');
const { spawn } = require('node:child_process');
const { once } = require('node:events');

const JS_RUNTIME = path.resolve(__dirname, '../../../Backend/scripting/runtimes/js');
const { ResourceHost } = require(path.join(JS_RUNTIME, 'host.cjs'));
const { encode, decode } = require(path.join(JS_RUNTIME, 'codec.cjs'));
const { normalizeManifest } = require(path.join(JS_RUNTIME, 'manifest.cjs'));
const { BrowserResourceRuntime } = require(path.resolve(
  __dirname,
  '../../../Client/resources/runtime.cjs',
));

const GLOBAL_WORLD = 4294967295;

function delay(milliseconds) {
  return new Promise((resolve) => {
    setTimeout(resolve, milliseconds);
  });
}

// Waits until a resource logged a line the predicate accepts. A Lua script chains promises with
// :next and a C# script's awaits continue as .NET schedules them, so a script's last line can come
// after the host's call settled.
async function untilLogged(f, resource, predicate) {
  const end = Date.now() + 3000;
  while (!f.linesOf(resource).some(predicate)) {
    if (Date.now() > end) {
      throw new Error(
        `${resource} did not log the expected line:\n${f.linesOf(resource).join('\n')}`,
      );
    }
    await delay(5);
  }
}

// Writes one resource folder. The folder name and the manifest name may differ on purpose.
function writeResource(root, folder, manifest, files) {
  const directory = path.join(root, folder);
  fs.mkdirSync(directory, { recursive: true });
  fs.writeFileSync(path.join(directory, 'package.json'), JSON.stringify(manifest));
  for (const [file, code] of Object.entries(files)) {
    fs.writeFileSync(path.join(directory, file), code);
  }
}

// The resource languages a scenario runs in, and each one's entry file.
const LANGUAGES = Object.freeze(['javascript', 'lua', 'csharp']);
const ENTRY = Object.freeze({ javascript: 'main.js', lua: 'main.lua', csharp: 'Main.dll' });

// What a scenario's C# script may use without saying so: the SDK's namespaces.
const CSHARP_USINGS = [
  'using Kcds;',
  'using Kcds.Core;',
  'using Kcds.Math;',
  'using Kcds.Players;',
  'using Kcds.Characters;',
  'using Kcds.World;',
  'using Kcds.Ui;',
  '',
].join('\n');

// The entry file's contents: the script itself, or for C# the assembly built from it.
function entryContent(name, code, language) {
  const source = codeIn(code, language);
  if (language !== 'csharp') {
    return source;
  }
  const { buildCSharp } = require('./csharp.cjs');
  const assembly = 'Scenario_' + name.replace(/[^A-Za-z0-9]/g, '_');
  return buildCSharp(assembly, { 'Main.cs': CSHARP_USINGS + source });
}

// What a script prints for a missing value, "null" or "undefined" in JavaScript: Lua has nil for
// both, C# null.
function shown(language, value) {
  if (value !== 'null' && value !== 'undefined') {
    return value;
  }
  if (language === 'lua') {
    return 'nil';
  }
  return language === 'csharp' ? 'null' : value;
}

// A scenario's script in one language: the code itself, or { javascript, lua, csharp } with each.
function codeIn(code, language) {
  if (typeof code === 'string') {
    if (language !== 'javascript') {
      throw new Error(`The scenario has no ${language} code`);
    }
    return code;
  }
  if (typeof code?.[language] !== 'string') {
    throw new Error(`The scenario has no ${language} code`);
  }
  return code[language];
}

// A server resource with one entry script, in a folder of the same name.
function serverResource(root, name, code, kcds = {}, language = 'javascript') {
  const entry = ENTRY[language];
  writeResource(
    root,
    name,
    { name, kcds: { serverScripts: [entry], ...kcds } },
    { [entry]: entryContent(name, code, language) },
  );
}

// The trusted server host with a recording bridge. Resource scripts publish observations with
// console.log, which the host forwards to bridge.log(resource, text).
function serverFixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-guide-'));
  const sent = [];
  const logs = [];
  const host = new ResourceHost(root, {
    send: (message) => sent.push(message),
    log: (resource, text) => logs.push({ resource, text: String(text) }),
  });
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  const linesOf = (resource) => {
    const lines = [];
    for (const entry of logs) {
      if (entry.resource === resource) {
        lines.push(entry.text);
      }
    }
    return lines;
  };
  // A native player report, as main.cjs forwards it from the server.
  const report = (id, fields = {}) => {
    return host.updatePlayer({
      id,
      nickname: `Player ${id}`,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      ...fields,
    });
  };
  return { root, host, sent, logs, linesOf, report };
}

// The browser client runtime over an in-memory catalog, with its production evaluator, so live
// getters such as LocalPlayer behave as in the WebView worker.
function clientFixture(t) {
  const sent = [];
  const logs = [];
  const files = new Map();
  const runtime = new BrowserResourceRuntime({
    send: (message) => sent.push(message),
    log: (resource, text) => logs.push({ resource, text: String(text) }),
  });
  const resource = (name, source, kcds = {}, hash = name, language = 'javascript') => {
    const entry = ENTRY[language];
    const content = entryContent(name, source, language);
    files.set(
      name + '/' + entry,
      typeof content === 'string' ? new TextEncoder().encode(content) : new Uint8Array(content),
    );
    return {
      hash,
      manifest: normalizeManifest({ name, kcds: { clientScripts: [entry], ...kcds } }),
      files: [{ name: entry }],
    };
  };
  const read = (name, file) => {
    const bytes = files.get(name + '/' + file);
    if (!bytes) {
      throw new Error('Unknown file');
    }
    return bytes;
  };
  const linesOf = (name) => {
    const lines = [];
    for (const entry of logs) {
      if (entry.resource === name) {
        lines.push(entry.text);
      }
    }
    return lines;
  };
  t.after(() => runtime.disconnect());
  return { runtime, resource, read, sent, logs, linesOf };
}

// The production server host process (main.cjs) over its hex-framed IPC, as the native server
// runs it. `setup(root)` writes the resources before the process starts.
async function hostProcess(t, setup) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-guide-process-'));
  setup(root);
  const child = spawn(process.execPath, [path.join(JS_RUNTIME, 'main.cjs'), root], {
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
  const killer = setTimeout(() => child.kill(), 20000);
  t.after(async () => {
    clearTimeout(killer);
    if (child.exitCode === null) {
      child.stdin.end();
      await exited;
    }
    fs.rmSync(root, { recursive: true, force: true });
  });
  const send = (message) => {
    child.stdin.write(encode(message).toString('hex') + '\n');
  };
  const waitFor = async (predicate, what = 'host frame') => {
    const end = Date.now() + 8000;
    while (!frames.some(predicate)) {
      if (Date.now() > end) {
        throw new Error(`Timed out waiting for ${what}`);
      }
      await delay(10);
    }
  };
  const logLines = (resource) => {
    const lines = [];
    for (const frame of frames) {
      if (frame.op === 'log' && frame.resource === resource) {
        lines.push(frame.text);
      }
    }
    return lines;
  };
  const clientEvent = (player, name, payload) => {
    send({
      op: 'clientRequest',
      peer: player,
      player,
      playerIndex: 0,
      message: { op: 'event', name, payload },
    });
  };
  // A player as a client reports them; the first report is followed by the client's word that its
  // resources run, as a game sends it once they are downloaded (events to it wait until then).
  const reported = new Set();
  const report = (id, fields = {}) => {
    send({
      op: 'player',
      id,
      nickname: `Player ${id}`,
      ready: true,
      position: { x: 0, y: 0, z: 0 },
      rotation: { w: 1, x: 0, y: 0, z: 0 },
      ...fields,
    });
    if (!reported.has(id)) {
      reported.add(id);
      send({
        op: 'clientRequest',
        peer: id,
        player: id,
        playerIndex: 0,
        message: { op: 'resourcesReady' },
      });
    }
  };
  const stop = async () => {
    child.stdin.end();
    const [code] = await exited;
    return code;
  };
  await waitFor((frame) => frame.op === 'ready', 'host ready');
  return { root, frames, errors, send, waitFor, logLines, clientEvent, report, stop };
}

module.exports = {
  GLOBAL_WORLD,
  LANGUAGES,
  ENTRY,
  codeIn,
  entryContent,
  shown,
  delay,
  untilLogged,
  writeResource,
  serverResource,
  serverFixture,
  clientFixture,
  hostProcess,
};
