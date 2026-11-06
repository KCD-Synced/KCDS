'use strict';

// C# server resources: a resource built into .dll files runs on the side's .NET
// WebAssembly runtime, in its own load context, over the same SDK a JavaScript resource gets.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { csharpResource } = require('../../../../Dev/testkit/guide_scenarios/csharp.cjs');

function waitFor(predicate, what) {
  const end = Date.now() + 5000;
  return new Promise((resolve, reject) => {
    const check = () => {
      if (predicate()) {
        resolve();
      } else if (Date.now() > end) {
        reject(new Error(`Timed out waiting for ${what}`));
      } else {
        setTimeout(check, 5);
      }
    };
    check();
  });
}

const GREETER = `
using Kcds;
using Kcds.Core;
using Kcds.Math;
using Kcds.Players;

public class Greeter : Script
{
    public override void OnStart()
    {
        var vector = new Vector3(3, 4, 0);
        Log.Info($"length {vector.Length} distance {vector.Distance(Vector3.Zero())} players {Player.All().Length}");
        Events.On(EventNames.PlayerConnect, (Player player) =>
        {
            Log.Info($"joined {player.Id} {player.Nickname}");
            Chat.SendToPlayer(player, "welcome", new { Color = 0xff0000ff });
        });
        Events.On("csPing", (double a, double b) => Log.Info($"ping {a + b}"));
        Events.On("csSlow", async (string text) =>
        {
            await Timers.Delay(10);
            Log.Info("slow " + text);
        });
        try
        {
            Chat.SendToAll(5);
        }
        catch (Exception problem)
        {
            Log.Info("caught " + problem.Message);
        }
        Timers.SetTimeout(() => Log.Info("timer"), 5);
        Console.WriteLine("console line");
    }
}
`;

test('a C# resource uses the typed SDK: classes, statics, events, chat, timers, async', async (t) => {
  const f = serverFixture(t);
  csharpResource(f.root, 'greeter', { 'Greeter.cs': GREETER });
  await f.host.startAll();
  const lines = () => f.linesOf('greeter');
  await waitFor(() => lines().includes('timer'), 'the timer');
  assert.ok(lines().includes('length 5 distance 5 players 0'), lines().join('\n'));
  assert.ok(lines().includes('console line'));
  assert.ok(
    lines().some((line) => /^caught .*Chat text must be string/.test(line)),
    lines().join('\n'),
  );
  f.report(7);
  await waitFor(() => lines().some((line) => line.startsWith('joined')), 'the join handler');
  assert.ok(lines().includes('joined 7 Player 7'), lines().join('\n'));
  const welcome = f.sent.find((message) => message.op === 'chat' && message.target === 7);
  assert.equal(welcome?.text, 'welcome');
  const api = f.host.makeAPI();
  await api.Events.emit('csPing', 2, 3);
  assert.ok(lines().includes('ping 5'));
  // An async handler is waited for, as an async JavaScript handler is.
  await api.Events.emit('csSlow', 'done');
  assert.ok(lines().includes('slow done'), lines().join('\n'));
});

const TICKER = `
using Kcds;
using Kcds.Core;

public class Ticker : Script
{
    public override void OnStart()
    {
        Events.On("csTick", () => Log.Info("tick"));
        Timers.SetInterval(() => Log.Info("interval"), 5);
    }
}
`;

test('stopping a C# resource drops its handlers and timers', async (t) => {
  const f = serverFixture(t);
  csharpResource(f.root, 'ticker', { 'Ticker.cs': TICKER });
  await f.host.startAll();
  const lines = () => f.linesOf('ticker');
  await waitFor(() => lines().includes('interval'), 'the interval');
  await f.host.stop('ticker');
  const before = lines().length;
  await f.host.makeAPI().Events.emit('csTick');
  await new Promise((resolve) => setTimeout(resolve, 40));
  assert.equal(lines().length, before);
});

const BROKEN = `
using Kcds;

public class Broken : Script
{
    public override void OnStart()
    {
        throw new InvalidOperationException("the C# start failed");
    }
}
`;

test('a C# exception at start stops the resource with its message', async (t) => {
  const f = serverFixture(t);
  csharpResource(f.root, 'broken', { 'Broken.cs': BROKEN });
  await f.host.startAll();
  assert.equal(f.host.resources.has('broken'), false);
  assert.ok(
    f.linesOf('broken').some((line) => line.includes('the C# start failed')),
    f.linesOf('broken').join('\n'),
  );
});

const STATIC_COUNTER = `
using Kcds;

public class Counter : Script
{
    public static int Starts;

    public override void OnStart()
    {
        Starts += 1;
        Log.Info("starts " + Starts);
        Kcds.Core.Exports.Register("starts", new Func<int>(() => Starts));
    }
}
`;

test('each C# resource has its own types and static state, and a restart starts afresh', async (t) => {
  const f = serverFixture(t);
  csharpResource(f.root, 'first', { 'Counter.cs': STATIC_COUNTER }, { exports: ['starts'] });
  csharpResource(f.root, 'second', { 'Counter.cs': STATIC_COUNTER }, { exports: ['starts'] });
  await f.host.startAll();
  assert.ok(f.linesOf('first').includes('starts 1'));
  assert.ok(f.linesOf('second').includes('starts 1'));
  await f.host.ensure('first');
  assert.deepEqual(
    f.linesOf('first').filter((line) => line.startsWith('starts')),
    ['starts 1', 'starts 1'],
  );
  assert.equal(f.host.services.owners.get('first').exports.get('starts')(), 1);
});

// Exported C# functions answering values JavaScript passed in: they come back as the very values.
const ECHO = `
using Kcds;
using Kcds.Core;

public class Echo : Script
{
    public override void OnStart()
    {
        Exports.Register("same", new Func<JsValue, JsValue>((value) => value));
        Exports.Register("later", new Func<JsValue, Task<JsValue>>(async (value) =>
        {
            await Timers.Delay(1);
            return value;
        }));
        Exports.Register("list", new Func<int[]>(() => new[] { 1, 2, 3 }));
    }
}
`;

test('what a C# function answers reaches JavaScript as the value itself', async (t) => {
  const f = serverFixture(t);
  csharpResource(f.root, 'echo', { 'Echo.cs': ECHO }, { exports: ['same', 'later', 'list'] });
  await f.host.startAll();
  const exported = f.host.services.owners.get('echo').exports;
  const array = [1, 2];
  const fn = () => 'called';
  assert.equal(exported.get('same')(array), array);
  assert.equal(exported.get('same')(fn), fn);
  assert.equal(exported.get('same')(fn)(), 'called');
  assert.equal(await exported.get('later')(array), array);
  // An array C# made is a plain array (the .NET runtime marks objects it holds with a symbol of
  // its own, which JSON and the SDK do not see).
  const listed = exported.get('list')();
  assert.ok(Array.isArray(listed));
  assert.deepEqual([...listed], [1, 2, 3]);
  assert.equal(exported.get('same')(undefined), null);
});

const KINDS = `
using Kcds;
using Kcds.Core;
using Kcds.Players;

public class Kinds : Script
{
    public override void OnStart()
    {
        Events.On("kinds:check", (Entity entity) =>
        {
            var name = entity switch
            {
                Player player => "player " + player.Nickname,
                Kcds.Characters.Horse horse => "horse " + horse.Name,
                _ => "entity",
            };
            Log.Info(name);
        });
    }
}
`;

test('an SDK object is wrapped as the class that made it: an Entity that is a player is a Player', async (t) => {
  const f = serverFixture(t);
  csharpResource(f.root, 'kinds', { 'Kinds.cs': KINDS });
  await f.host.startAll();
  const player = f.report(4, { nickname: 'Hans' });
  await f.host.services.emit('shared', 'kinds:check', [player]);
  const horse = f.host.api.Horse.spawn({ x: 1, y: 2, z: 3 }, undefined, undefined, 'Pebbles');
  await f.host.services.emit('shared', 'kinds:check', [horse]);
  assert.deepEqual(
    f.linesOf('kinds').filter((line) => line !== 'started'),
    ['player Hans', 'horse Pebbles'],
  );
});

test('the manifest refuses C# sources: a resource lists the .dll it was built into', async (t) => {
  const f = serverFixture(t);
  const directory = path.join(f.root, 'sources');
  fs.mkdirSync(directory);
  fs.writeFileSync(path.join(directory, 'Main.cs'), 'class Main {}');
  fs.writeFileSync(
    path.join(directory, 'package.json'),
    JSON.stringify({ name: 'sources', kcds: { server: 'Main.cs' } }),
  );
  await f.host.startAll();
  assert.equal(f.host.resources.has('sources'), false);
  assert.ok(f.linesOf('sources').some((line) => /lists the \.dll files/.test(line)));
});

const CLIENT = `
using Kcds;
using Kcds.Core;
using Kcds.Ui;

public class Client : Script
{
    public override void OnStart()
    {
        Log.Info("client C# started " + new Kcds.Math.Vector3(1, 2, 2).Length);
        Events.On("csHello", (JsValue data) =>
        {
            Hud.ShowNotification("C#: " + data["text"]);
            Events.ToServer("csPong", new { Sum = data["n"].AsInt() + 1 });
        });
    }
}
`;

test('a C# client resource goes through the catalog and runs in the player runtime', async (t) => {
  const { ResourceBundleStore, digest } = require('./bundles.cjs');
  const { normalizeManifest } = require('./manifest.cjs');
  const { BundleCache } = require('../../../../Client/resources/bundle_cache.cjs');
  const { BrowserResourceRuntime } = require('../../../../Client/resources/runtime.cjs');
  const { buildCSharp } = require('../../../../Dev/testkit/guide_scenarios/csharp.cjs');
  const os = require('node:os');
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-csharp-catalog-'));
  t.after(() => fs.rmSync(folder, { recursive: true, force: true }));
  fs.mkdirSync(path.join(folder, 'client'));
  fs.writeFileSync(
    path.join(folder, 'client', 'Client.dll'),
    buildCSharp('Client', { 'Client.cs': CLIENT }),
  );
  const source = { name: 'shipped', kcds: { client: 'client/Client.dll' } };
  const store = new ResourceBundleStore();
  store.report = (text) => assert.fail(text);
  store.publish([{ folder, manifest: normalizeManifest(source) }]);
  const catalog = store.manifest();
  assert.deepEqual(catalog.resources[0].manifest.kcds.language, { client: 'csharp' });
  const cache = new BundleCache(async (bytes) => digest(bytes));
  await cache.acquire(catalog, async ({ revision, hash, offset, length }) => {
    const chunk = store.chunk(revision, hash, offset, length);
    return { ...chunk, bytes: Uint8Array.from(Buffer.from(chunk.data, 'base64')) };
  });
  const sent = [];
  const logs = [];
  const runtime = new BrowserResourceRuntime({
    send: (message) => sent.push(message),
    log: (resource, text) => logs.push(String(text)),
  });
  t.after(() => runtime.disconnect());
  runtime.hud.receiveHost({ ready: true });
  await runtime.apply({ resources: catalog.resources }, (name, file) => cache.read(name, file));
  assert.ok(logs.includes('client C# started 3'), logs.join('\n'));
  await runtime.services.emit('shared', 'csHello', [{ text: 'hi', n: 41 }]);
  const shown = sent.filter((message) => message.op === 'hud').map((message) => message.args[0]);
  assert.deepEqual(shown, ['C#: hi']);
  const pong = sent.find((message) => message.op === 'event' && message.name === 'csPong');
  assert.deepEqual(JSON.parse(pong.payload), { sum: 42 });
});

// The starter project the KCDS-Types zip ships (Backend/scripting/runtimes/csharp/Template), built
// as an author builds it: `dotnet build` in its folder, against the SDK's Kcds.Sdk.dll.
test('the C# resource template builds with dotnet and runs on both sides', async (t) => {
  const { execFileSync } = require('node:child_process');
  const { ensureHost, DOTNET_DIR } = require('../../../../Dev/testkit/guide_scenarios/csharp.cjs');
  ensureHost();
  const f = serverFixture(t);
  const resource = path.join(f.root, 'my-resource');
  const template = path.resolve(__dirname, '../csharp/Template');
  fs.cpSync(template, resource, { recursive: true });
  execFileSync(
    'dotnet',
    ['build', '-c', 'Release', '--nologo', `-p:KcdsSdk=${path.join(DOTNET_DIR, 'Kcds.Sdk.dll')}`],
    { cwd: path.join(resource, 'server'), stdio: 'pipe' },
  );
  execFileSync(
    'dotnet',
    ['build', '-c', 'Release', '--nologo', `-p:KcdsSdk=${path.join(DOTNET_DIR, 'Kcds.Sdk.dll')}`],
    { cwd: path.join(resource, 'client'), stdio: 'pipe' },
  );
  assert.ok(fs.existsSync(path.join(resource, 'bin', 'server', 'Server.dll')));
  assert.ok(fs.existsSync(path.join(resource, 'bin', 'client', 'Client.dll')));
  // The SDK is the host's: the build never copies it next to the scripts.
  assert.equal(fs.existsSync(path.join(resource, 'bin', 'client', 'Kcds.Sdk.dll')), false);
  // Players get the client's folder only: never the server's assembly, nor the sources.
  const { ResourceBundleStore } = require('./bundles.cjs');
  const { normalizeManifest } = require('./manifest.cjs');
  const store = new ResourceBundleStore();
  store.report = (text) => assert.fail(text);
  const manifest = JSON.parse(fs.readFileSync(path.join(resource, 'package.json'), 'utf8'));
  store.publish([{ folder: resource, manifest: normalizeManifest(manifest) }]);
  const sent = store.manifest().resources[0].files.map((file) => file.name);
  assert.ok(sent.includes('bin/client/Client.dll'), sent.join('\n'));
  assert.deepEqual(
    sent.filter((name) => !name.startsWith('bin/client/')),
    [],
  );
  await f.host.startAll();
  assert.ok(f.linesOf('my-resource').includes('my-resource started on the server'));
  const player = f.report(5, { nickname: 'Henry' });
  const welcome = f.sent.filter((message) => message.op === 'chat').map((message) => message.text);
  assert.deepEqual(welcome, ['Welcome, Henry!']);

  const { clientFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
  const client = clientFixture(t);
  client.runtime.hud.receiveHost({ ready: true });
  const bytes = new Uint8Array(fs.readFileSync(path.join(resource, 'bin', 'client', 'Client.dll')));
  const shipped = {
    hash: 'my-resource',
    manifest: normalizeManifest(manifest),
    files: [{ name: 'bin/client/Client.dll' }],
  };
  await client.runtime.apply({ resources: [shipped] }, () => bytes);
  const hello = client.sent.find((message) => message.op === 'event');
  assert.equal(hello?.name, 'my-resource:hello');
  await f.host.services.emit('client', 'my-resource:hello', [player, null]);
  assert.ok(f.linesOf('my-resource').includes('Henry says hello'));
});
