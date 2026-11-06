'use strict';

// Resources as a server loads them: manifests, dependencies, start and stop. Each scenario runs
// with its scripts in JavaScript, in Lua and in C#.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {
  LANGUAGES,
  ENTRY,
  entryContent,
  delay,
  writeResource,
  serverResource,
  serverFixture,
  hostProcess,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

// A C# script whose start runs `body`.
function csharpScript(body) {
  return `
public class Main : Script
{
    public override void OnStart()
    {
        ${body}
    }
}
`;
}

// A one-line script that logs `text` when it runs.
function logs(language, text) {
  if (language === 'csharp') {
    return csharpScript(`Log.Info(${JSON.stringify(text)});`);
  }
  return language === 'lua'
    ? `print(${JSON.stringify(text)})`
    : `console.log(${JSON.stringify(text)});`;
}

// A script that fails while it starts.
function fails(language, text) {
  if (language === 'csharp') {
    return csharpScript(`throw new Exception(${JSON.stringify(text)});`);
  }
  return language === 'lua'
    ? `error(${JSON.stringify(text)})`
    : `throw new Error(${JSON.stringify(text)});`;
}

// The entry file of a resource with this script: its text, or for C# the assembly built from it.
function entryFile(name, code, language) {
  return entryContent(name, { [language]: code }, language);
}

const FIRST_COPY = {
  javascript: `kcds.core.Events.on('resourceStart', (name) => console.log('start ' + name));`,
  lua: `kcds.core.Events.on("resourceStart", function(name) print("start " .. name) end)`,
  csharp: csharpScript(`Events.On("resourceStart", (string name) => Log.Info("start " + name));`),
};

const MODE = {
  javascript: `
    const RESOURCE = 'my-mode';
    let loaded = false;
    kcds.core.Events.on('resourceStart', async (name) => {
      if (name !== RESOURCE) {
        return;
      }
      await new Promise((resolve) => setTimeout(resolve, 20));
      loaded = true;
    });
    kcds.core.Events.on('my-mode:boom', () => {
      throw new Error('handler threw');
    });
    kcds.core.Events.on('my-mode:alive', () => console.log('alive loaded=' + loaded));
    kcds.core.Events.once('my-mode:once', () => console.log('once ran'));
    kcds.core.Events.listen('my-mode:local', () => console.log('local ran'));
    kcds.core.Events.fromClient('my-mode:client', () => console.log('client ran'));
    kcds.core.Messages.handle('ping', (payload, reply) => reply('pong'));
    setInterval(() => console.log('tick'), 5);
    kcds.characters.Horse.spawn({ x: 1, y: 2, z: 3 });
    kcds.core.Events.on('playerConnect', (player) => {
      player.state.onChange('team', () => console.log('watcher ran'));
    });
    `,
  // A handler that returns a promise is waited for, as the async JavaScript one is.
  lua: `
    local RESOURCE = "my-mode"
    local loaded = false
    kcds.core.Events.on("resourceStart", function(name)
      if name ~= RESOURCE then
        return
      end
      return Promise.create(function(resolve)
        setTimeout(resolve, 20)
      end):next(function()
        loaded = true
      end)
    end)
    kcds.core.Events.on("my-mode:boom", function()
      error("handler threw")
    end)
    kcds.core.Events.on("my-mode:alive", function() print("alive loaded=" .. tostring(loaded)) end)
    kcds.core.Events.once("my-mode:once", function() print("once ran") end)
    kcds.core.Events.listen("my-mode:local", function() print("local ran") end)
    kcds.core.Events.fromClient("my-mode:client", function() print("client ran") end)
    kcds.core.Messages.handle("ping", function(payload, reply) reply("pong") end)
    setInterval(function() print("tick") end, 5)
    kcds.characters.Horse.spawn({ x = 1, y = 2, z = 3 })
    kcds.core.Events.on("playerConnect", function(player)
      player.state:onChange("team", function() print("watcher ran") end)
    end)
    `,
  // The async handler's Task is waited for, as the JavaScript promise is.
  csharp: `
public class MyMode : Script
{
    private const string Resource = "my-mode";
    private bool loaded;

    public override void OnStart()
    {
        Events.On("resourceStart", async (string name) =>
        {
            if (name != Resource)
            {
                return;
            }
            await Timers.Delay(20);
            loaded = true;
        });
        Events.On("my-mode:boom", () =>
        {
            throw new Exception("handler threw");
        });
        Events.On("my-mode:alive", () => Log.Info("alive loaded=" + (loaded ? "true" : "false")));
        Events.Once("my-mode:once", () => Log.Info("once ran"));
        Events.Listen("my-mode:local", () => Log.Info("local ran"));
        Events.FromClient("my-mode:client", () => Log.Info("client ran"));
        Messages.Handle("ping", (JsValue payload, JsValue reply) => reply.Invoke("pong"));
        Timers.SetInterval(() => Log.Info("tick"), 5);
        Horse.Spawn(new { X = 1, Y = 2, Z = 3 });
        Events.On("playerConnect", (Player player) =>
        {
            player.State.OnChange("team", () => Log.Info("watcher ran"));
        });
    }
}
`,
};

const OBSERVER = {
  javascript: `
    kcds.core.Events.on('observer:ask', async () => {
      try {
        console.log('reply ' + (await kcds.core.Messages.request('my-mode', 'ping')));
      } catch (error) {
        console.log('request failed ' + error.message);
      }
    });
    `,
  lua: `
    kcds.core.Events.on("observer:ask", function()
      local ok, pending = pcall(kcds.core.Messages.request, "my-mode", "ping")
      if not ok then
        print("request failed " .. pending.message)
        return
      end
      return pending
        :next(function(answer) print("reply " .. answer) end)
        :catch(function(problem) print("request failed " .. problem.message) end)
    end)
    `,
  csharp: `
public class Observer : Script
{
    public override void OnStart()
    {
        Events.On("observer:ask", async () =>
        {
            try
            {
                Log.Info("reply " + await Messages.Request("my-mode", "ping", null));
            }
            catch (JsError problem)
            {
                Log.Info("request failed " + problem.Message);
            }
        });
    }
}
`,
};

// The generation counter: Lua and C# scripts reach no files, so they count with a small
// JavaScript library's export instead (the cross-language way to keep data, KCDS-DOCS/lua.md).
function flaky(language, marker) {
  if (language === 'csharp') {
    return `
public class Flaky : Script
{
    public override void OnStart()
    {
        var count = Exports.Get("counter", "next").Invoke().AsInt();
        Log.Info("generation " + count);
        if (count == 1)
        {
            Timers.SetTimeout(() =>
            {
                throw new Exception("first generation fails");
            }, 5);
        }
    }
}
`;
  }
  if (language === 'lua') {
    return `
    local count = kcds.core.Exports.get("counter", "next")()
    print("generation " .. count)
    if count == 1 then
      setTimeout(function()
        error("first generation fails")
      end, 5)
    end
    `;
  }
  return `
    const fs = require('node:fs');
    const file = ${JSON.stringify(marker)};
    const count = fs.existsSync(file) ? Number(fs.readFileSync(file, 'utf8')) + 1 : 1;
    fs.writeFileSync(file, String(count));
    console.log('generation ' + count);
    if (count === 1) {
      setTimeout(() => {
        throw new Error('first generation fails');
      }, 5);
    }
    `;
}

function counter(marker) {
  return `
    const fs = require('node:fs');
    const file = ${JSON.stringify(marker)};
    kcds.core.Exports.register('next', () => {
      const count = fs.existsSync(file) ? Number(fs.readFileSync(file, 'utf8')) + 1 : 1;
      fs.writeFileSync(file, String(count));
      return count;
    });
    `;
}

// C# has no globals to patch: the like is a static field, here of a class both resources name
// alike. Each resource's assemblies load on their own, so the reader's class is its own.
const SHARED_CLASS = `
public static class Shared
{
    public static string? Flag;
}
`;

const WRITER = {
  javascript: `globalThis.sharedFlag = 'set by writer'; Array.prototype.patched = true;`,
  lua: `sharedFlag = "set by writer"; string.patched = true`,
  csharp: SHARED_CLASS + csharpScript(`Shared.Flag = "set by writer";`),
};

const READER = {
  javascript: `kcds.core.Events.on('resourceStart', () =>
      console.log('flag ' + globalThis.sharedFlag + ' patched ' + ([].patched === true)));`,
  lua: `kcds.core.Events.on("resourceStart", function()
      print("flag " .. tostring(sharedFlag) .. " patched " .. tostring(string.patched == true))
    end)`,
  csharp:
    SHARED_CLASS +
    csharpScript(`Events.On("resourceStart", () => Log.Info("flag " + (Shared.Flag ?? "null")));`),
};

const READER_SEES = {
  javascript: 'flag undefined patched false',
  lua: 'flag nil patched false',
  csharp: 'flag null',
};

for (const language of LANGUAGES) {
  const entry = ENTRY[language];

  test(`resources (${language}): the manifest name, not the folder, names a resource; duplicates warn`, async (t) => {
    const f = serverFixture(t);
    writeResource(
      f.root,
      'a-folder',
      { name: 'my-mode', kcds: { serverScripts: [entry] } },
      { [entry]: entryContent('my-mode', FIRST_COPY, language) },
    );
    writeResource(
      f.root,
      'b-folder',
      { name: 'my-mode', kcds: { serverScripts: [entry] } },
      { [entry]: entryFile('my-mode', logs(language, 'second copy ran'), language) },
    );
    await f.host.startAll();
    assert.deepEqual([...f.host.resources.keys()], ['my-mode']);
    assert.ok(f.linesOf('my-mode').includes('start my-mode'));
    assert.ok(f.linesOf('my-mode').includes('Duplicate resource ignored'));
    assert.ok(!f.linesOf('my-mode').includes('second copy ran'));
  });

  test(`resources (${language}): a missing required dependency stops the whole boot`, async (t) => {
    const f = serverFixture(t);
    const shop = { [language]: logs(language, 'shop ran') };
    serverResource(f.root, 'my-shop', shop, { requires: ['my-economy'] }, language);
    serverResource(
      f.root,
      'unrelated',
      { [language]: logs(language, 'unrelated ran') },
      {},
      language,
    );
    await assert.rejects(
      f.host.startAll(),
      /Resource 'my-shop' depends on missing resource 'my-economy'/,
    );
    assert.equal(f.host.resources.size, 0);
    assert.equal(
      f.logs.some((record) => record.text.endsWith(' ran')),
      false,
    );
  });

  test(`resources (${language}): a failing dependency fails only its dependents; optional ones are skipped`, async (t) => {
    const f = serverFixture(t);
    const code = (text) => ({ [language]: logs(language, text) });
    serverResource(
      f.root,
      'my-economy',
      { [language]: fails(language, 'economy broke') },
      {},
      language,
    );
    serverResource(f.root, 'my-shop', code('shop ran'), { requires: ['my-economy'] }, language);
    serverResource(
      f.root,
      'my-stats',
      code('stats ran'),
      {
        requires: [
          { name: 'my-economy', optional: true },
          { name: 'my-discord-log', optional: true, version: '2.0.0' },
        ],
      },
      language,
    );
    serverResource(f.root, 'unrelated', code('unrelated ran'), {}, language);
    await f.host.startAll();
    assert.deepEqual([...f.host.resources.keys()].sort(), ['my-stats', 'unrelated']);
    assert.ok(
      f.linesOf('my-shop').some((line) => line.includes('Required dependency unavailable')),
    );
    assert.ok(f.linesOf('my-stats').includes('Optional dependency unavailable: my-economy'));
    assert.ok(f.linesOf('my-stats').includes('Optional dependency missing: my-discord-log'));
  });

  test(`resources (${language}): start code, a throwing handler and the cleanup list on stop`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'my-mode', MODE, { exports: ['answer'] }, language);
    serverResource(f.root, 'observer', OBSERVER, {}, language);
    await f.host.startAll();
    // The async start handler finished before the resource counted as running.
    await f.host.services.emit('shared', 'my-mode:boom', []).catch(() => {});
    await f.host.services.emit('shared', 'my-mode:alive', []);
    assert.ok(f.linesOf('my-mode').includes('alive loaded=true'), f.linesOf('my-mode').join('\n'));
    const player = f.report(51);
    await f.host.services.emit('shared', 'observer:ask', []);
    assert.ok(f.linesOf('observer').includes('reply pong'));
    assert.equal(f.host.horses.size, 1);
    await f.host.stop('my-mode');
    const before = f.linesOf('my-mode').length;
    await f.host.services.emit('shared', 'my-mode:once', []);
    await f.host.services.emit('shared', 'my-mode:alive', []);
    await f.host.services.emit('local', 'my-mode:local', [], 'my-mode');
    await f.host.services.emit('client', 'my-mode:client', [player, null]);
    await f.host.services.emit('shared', 'observer:ask', []);
    player.state.set('team', 'ravens');
    await delay(20);
    assert.equal(f.linesOf('my-mode').length, before);
    assert.ok(f.linesOf('observer').includes('request failed Resource is not available'));
    assert.equal(f.host.horses.size, 0);
    // The state key itself stays with the entity.
    assert.equal(player.state.get('team'), 'ravens');
  });

  test(`resources (${language}): console start, stop, restart, ensure, refresh and refreshall`, async (t) => {
    const host = await hostProcess(t, (root) => {
      serverResource(root, 'my-mode', { [language]: logs(language, 'my-mode v1') }, {}, language);
    });
    const write = (name, text, version) => {
      writeResource(
        host.root,
        name,
        { name, version, kcds: { serverScripts: [entry] } },
        { [entry]: entryFile(name, logs(language, text), language) },
      );
    };
    const command = async (text, expectation) => {
      host.send({ op: 'console', text });
      await host.waitFor(expectation, text);
    };
    const logged = (resource, text) => (frame) => {
      return frame.op === 'log' && frame.resource === resource && frame.text === text;
    };
    // refresh finds a new resource but leaves it stopped.
    write('late', 'late v1', '1.0.0');
    host.send({ op: 'console', text: 'refresh' });
    await delay(100);
    assert.deepEqual(host.logLines('late'), []);
    await command('start late', logged('late', 'late v1'));
    // restart re-reads the scripts from disk.
    write('late', 'late v2', '1.0.1');
    await command('restart late', logged('late', 'late v2'));
    await command('stop late', (frame) => {
      return logged('late', 'stopped')(frame) && host.logLines('late').includes('late v2');
    });
    // ensure starts a stopped resource ...
    await command('ensure late', (frame) => {
      return host.logLines('late').filter((line) => line === 'late v2').length === 2;
    });
    // ... and restarts a running one.
    write('my-mode', 'my-mode v2', '1.0.0');
    await command('ensure my-mode', logged('my-mode', 'my-mode v2'));
    // refreshall reloads everything that was running.
    write('my-mode', 'my-mode v3', '1.0.0');
    write('late', 'late v3', '1.0.0');
    await command('refreshall', (frame) => {
      return (
        host.logLines('my-mode').includes('my-mode v3') && host.logLines('late').includes('late v3')
      );
    });
    assert.equal(await host.stop(), 0);
    assert.deepEqual(host.errors, []);
  });

  test(`resources (${language}): errorBehavior restart brings a resource back after a second`, async (t) => {
    const f = serverFixture(t);
    const marker = path.join(f.root, 'restart-count.txt');
    const requires = [];
    if (language !== 'javascript') {
      serverResource(f.root, 'counter', counter(marker), { exports: ['next'] });
      requires.push('counter');
    }
    serverResource(
      f.root,
      'flaky',
      { [language]: flaky(language, marker) },
      { errorBehavior: 'restart', requires },
      language,
    );
    await f.host.startAll();
    await delay(300);
    assert.equal(f.host.resources.has('flaky'), false);
    await delay(1200);
    assert.equal(f.host.resources.has('flaky'), true);
    assert.deepEqual(
      f.linesOf('flaky').filter((line) => line.startsWith('generation')),
      ['generation 1', 'generation 2'],
    );
    assert.ok(fs.existsSync(marker));
  });

  // Resources G1, a documented deliberate deviation: the
  // guide describes one globalThis shared by all server resources. Each of ours has its own vm
  // context (and each Lua resource its own Lua state), so a value one resource puts on its
  // globals, or a patched built-in, is not seen by another; the event bus, exports and the SDK
  // services are shared.
  test(`resources (${language}): each server resource keeps its own globals and built-ins`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'writer', WRITER, {}, language);
    serverResource(f.root, 'reader', READER, { requires: ['writer'] }, language);
    await f.host.startAll();
    assert.ok(f.linesOf('reader').includes(READER_SEES[language]), f.linesOf('reader').join('\n'));
  });
}
