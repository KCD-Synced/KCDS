'use strict';

// Events in resource scripts: listening, ordering and handlers that fail.
// Each test follows one walkthrough of the guide with our own resources and wording, with its
// scripts in JavaScript and in Lua.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  LANGUAGES,
  delay,
  serverResource,
  serverFixture,
  hostProcess,
  shown,
  untilLogged,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const LOBBY = {
  javascript: `
    const seen = [];
    const stop = kcds.core.Events.on('playerChat', (player, text) => seen.push('on:' + text));
    const onceResult = kcds.core.Events.once('playerChat', (player, text) => seen.push('once:' + text));
    const removed = (player, text) => seen.push('off:' + text);
    kcds.core.Events.on('playerChat', removed);
    kcds.core.Events.off('playerChat', removed);
    kcds.core.Events.on('lobby:report', () => {
      console.log(JSON.stringify({ seen, stopType: typeof stop, onceResult: String(onceResult) }));
    });
    kcds.core.Events.on('lobby:stop', () => stop());
    `,
  lua: `
    local seen = {}
    local stop = kcds.core.Events.on("playerChat", function(player, text)
      table.insert(seen, "on:" .. text)
    end)
    local onceResult = kcds.core.Events.once("playerChat", function(player, text)
      table.insert(seen, "once:" .. text)
    end)
    local function removed(player, text)
      table.insert(seen, "off:" .. text)
    end
    kcds.core.Events.on("playerChat", removed)
    kcds.core.Events.off("playerChat", removed)
    kcds.core.Events.on("lobby:report", function()
      print(json.encode({ seen = seen, stopType = type(stop), onceResult = tostring(onceResult) }))
    end)
    kcds.core.Events.on("lobby:stop", function() stop() end)
    `,
  // Events.Once answers nothing in C# (void), which the report shows as null.
  csharp: `
public class Lobby : Script
{
    private readonly List<string> seen = new();

    public override void OnStart()
    {
        var stop = Events.On("playerChat", (Player player, string text) => seen.Add("on:" + text));
        Events.Once("playerChat", (Player player, string text) => seen.Add("once:" + text));
        Action<Player, string> removed = (player, text) => seen.Add("off:" + text);
        Events.On("playerChat", removed);
        Events.Off("playerChat", removed);
        Events.On("lobby:report", () =>
        {
            var report = new { seen, stopType = stop.Type, onceResult = "null" };
            Log.Info(System.Text.Json.JsonSerializer.Serialize(report));
        });
        Events.On("lobby:stop", () => stop.Invoke());
    }
}
`,
};

const ROUNDS = {
  javascript: `
    const order = [];
    kcds.core.Events.on('rounds:end', async (winner) => {
      order.push('first:start');
      await null;
      order.push('first:after-await');
    });
    kcds.core.Events.on('rounds:end', (winner) => {
      order.push('second:sync');
      throw new Error('second failed');
    });
    kcds.core.Events.on('rounds:end', async (winner) => {
      order.push('third:start');
      await null;
      throw new Error('third failed');
    });
    kcds.core.Events.on('rounds:run', async () => {
      const pending = kcds.core.Events.emit('rounds:end', 'Ravens');
      // Every handler already ran up to its first await before emit returned.
      const before = order.slice();
      let failure;
      try {
        await pending;
      } catch (error) {
        failure = error;
      }
      console.log(JSON.stringify({
        before,
        after: order,
        aggregate: failure.name === 'AggregateError' && Array.isArray(failure.errors),
        causes: failure.errors.map((error) => error.message),
      }));
    });
    `,
  // Lua handlers run to their end before emit returns; their errors aggregate the same way.
  lua: `
    local order = {}
    kcds.core.Events.on("rounds:end", function(winner)
      table.insert(order, "first:start")
    end)
    kcds.core.Events.on("rounds:end", function(winner)
      table.insert(order, "second:sync")
      error("second failed", 0)
    end)
    kcds.core.Events.on("rounds:end", function(winner)
      table.insert(order, "third:start")
      error("third failed", 0)
    end)
    kcds.core.Events.on("rounds:run", function()
      local pending = kcds.core.Events.emit("rounds:end", "Ravens")
      local before = { table.unpack(order) }
      pending:catch(function(failure)
        local causes = {}
        for _, cause in ipairs(failure.errors) do
          table.insert(causes, cause.message)
        end
        print(json.encode({
          before = before,
          after = order,
          aggregate = failure.name == "AggregateError" and #failure.errors == 2,
          causes = causes,
        }))
      end)
    end)
    `,
  // C# handlers run to their end before emit returns too; an async handler (rounds:run) is waited
  // for, and the rejected emit is a JsError with the AggregateError in it.
  csharp: `
public class Rounds : Script
{
    private readonly List<string> order = new();

    public override void OnStart()
    {
        Events.On("rounds:end", (string winner) => order.Add("first:start"));
        Events.On("rounds:end", (string winner) =>
        {
            order.Add("second:sync");
            throw new InvalidOperationException("second failed");
        });
        Events.On("rounds:end", (string winner) =>
        {
            order.Add("third:start");
            throw new InvalidOperationException("third failed");
        });
        Events.On("rounds:run", async () =>
        {
            var pending = Events.Emit("rounds:end", "Ravens");
            var before = order.ToArray();
            try
            {
                await pending;
            }
            catch (JsError failure)
            {
                var causes = failure.Error["errors"].Select((cause) => cause["message"].ToString()).ToArray();
                var report = new
                {
                    before,
                    after = order,
                    aggregate = failure.Name == "AggregateError" && failure.Error["errors"].Length == 2,
                    causes,
                };
                Log.Info(System.Text.Json.JsonSerializer.Serialize(report));
            }
        });
    }
}
`,
};

const QUEUE = {
  javascript: `
    const inQueue = new Set();
    kcds.core.Events.on('playerSpawned', (player) => inQueue.add(player.id));
    kcds.core.Events.on('playerDisconnect', (player) => {
      console.log('leaving:' + player.nickname + ':' + (kcds.players.Player.getById(player.id) === player));
      inQueue.delete(player.id);
    });
    kcds.core.Events.on('queue:announce', (text) => {
      for (const id of inQueue) {
        const player = kcds.players.Player.getById(id);
        if (player) {
          kcds.core.Chat.sendToPlayer(player, text);
        }
      }
    });
    kcds.core.Events.on('queue:lookup', (id) => {
      console.log('lookup:' + id + ':' + kcds.players.Player.getById(id));
    });
    `,
  lua: `
    local inQueue = {}
    kcds.core.Events.on("playerSpawned", function(player)
      inQueue[player.id] = true
    end)
    kcds.core.Events.on("playerDisconnect", function(player)
      local same = kcds.players.Player.getById(player.id) == player
      print("leaving:" .. player.nickname .. ":" .. tostring(same))
      inQueue[player.id] = nil
    end)
    kcds.core.Events.on("queue:announce", function(text)
      for id in pairs(inQueue) do
        local player = kcds.players.Player.getById(id)
        if player then
          kcds.core.Chat.sendToPlayer(player, text)
        end
      end
    end)
    kcds.core.Events.on("queue:lookup", function(id)
      print("lookup:" .. id .. ":" .. tostring(kcds.players.Player.getById(id)))
    end)
    `,
  csharp: `
public class QueueScript : Script
{
    private readonly HashSet<double> inQueue = new();

    public override void OnStart()
    {
        Events.On("playerSpawned", (Player player) => inQueue.Add(player.Id));
        Events.On("playerDisconnect", (Player player) =>
        {
            var same = Player.GetById(player.Id) == player;
            Log.Info("leaving:" + player.Nickname + ":" + (same ? "true" : "false"));
            inQueue.Remove(player.Id);
        });
        Events.On("queue:announce", (string text) =>
        {
            foreach (var id in inQueue)
            {
                var player = Player.GetById(id);
                if (player != null)
                {
                    Chat.SendToPlayer(player, text);
                }
            }
        });
        Events.On("queue:lookup", (int id) =>
        {
            Log.Info("lookup:" + id + ":" + (Player.GetById(id)?.ToString() ?? "null"));
        });
    }
}
`,
};

const MODE = {
  javascript: `
    kcds.core.Events.on('mode:round.end', (winner) => {
      if (typeof winner !== 'string') {
        return;
      }
      console.log('mode heard ' + winner);
    });
    kcds.core.Events.listen('mode:private', (value) => console.log('mode private ' + value));
    kcds.core.Events.on('mode:run', async () => {
      await kcds.core.Events.emit('mode:round.end', 'Ravens');
      await kcds.core.Events.emit('mode:round.end', 42);
      await kcds.core.Events.toResource('scoreboard', 'mode:round.end', 'Wolves');
      await kcds.core.Events.fire('mode:private', 'one');
      console.log('count ' + kcds.core.Events.listenerCount('mode:round.end'));
      console.log('unlisten ' + typeof kcds.core.Events.unlisten);
      // Emitting a native name runs every resource's handlers as if the game raised it.
      await kcds.core.Events.emit('playerDisconnect', kcds.players.Player.getById(8));
    });
    `,
  lua: `
    local Events = kcds.core.Events
    Events.on("mode:round.end", function(winner)
      if type(winner) ~= "string" then
        return
      end
      print("mode heard " .. winner)
    end)
    Events.listen("mode:private", function(value) print("mode private " .. value) end)
    Events.on("mode:run", function()
      Events.emit("mode:round.end", "Ravens")
        :next(function() return Events.emit("mode:round.end", 42) end)
        :next(function() return Events.toResource("scoreboard", "mode:round.end", "Wolves") end)
        :next(function() return Events.fire("mode:private", "one") end)
        :next(function()
          print("count " .. Events.listenerCount("mode:round.end"))
          print("unlisten " .. type(Events.unlisten))
          -- Emitting a native name runs every resource's handlers as if the game raised it.
          return Events.emit("playerDisconnect", kcds.players.Player.getById(8))
        end)
    end)
    `,
  csharp: `
public class Mode : Script
{
    public override void OnStart()
    {
        Events.On("mode:round.end", (JsValue winner) =>
        {
            if (winner.Type != "string")
            {
                return;
            }
            Log.Info("mode heard " + winner);
        });
        Events.Listen("mode:private", (string value) => Log.Info("mode private " + value));
        Events.On("mode:run", async () =>
        {
            await Events.Emit("mode:round.end", "Ravens");
            await Events.Emit("mode:round.end", 42);
            await Events.ToResource("scoreboard", "mode:round.end", "Wolves");
            await Events.Fire("mode:private", "one");
            Log.Info("count " + Events.ListenerCount("mode:round.end"));
            Log.Info("unlisten " + new JsValue(Sdk.At("core", "Events"))["unlisten"]);
            // Emitting a native name runs every resource's handlers as if the game raised it.
            await Events.Emit("playerDisconnect", Player.GetById(8));
        });
    }
}
`,
};

const SCOREBOARD = {
  javascript: `
    kcds.core.Events.on('mode:round.end', (winner) => console.log('scoreboard heard ' + winner));
    kcds.core.Events.once('mode:round.end', () => console.log('scoreboard once'));
    kcds.core.Events.listen('mode:private', (value) => console.log('scoreboard private ' + value));
    kcds.core.Events.on('playerDisconnect', (player) => console.log('scoreboard saw leave ' + player.id));
    `,
  lua: `
    kcds.core.Events.on("mode:round.end", function(winner) print("scoreboard heard " .. winner) end)
    kcds.core.Events.once("mode:round.end", function() print("scoreboard once") end)
    kcds.core.Events.listen("mode:private", function(value) print("scoreboard private " .. value) end)
    kcds.core.Events.on("playerDisconnect", function(player)
      print("scoreboard saw leave " .. player.id)
    end)
    `,
  csharp: `
public class Scoreboard : Script
{
    public override void OnStart()
    {
        Events.On("mode:round.end", (JsValue winner) => Log.Info("scoreboard heard " + winner));
        Events.Once("mode:round.end", () => Log.Info("scoreboard once"));
        Events.Listen("mode:private", (string value) => Log.Info("scoreboard private " + value));
        Events.On("playerDisconnect", (Player player) => Log.Info("scoreboard saw leave " + player.Id));
    }
}
`,
};

const CARELESS = {
  javascript: `
      kcds.core.Events.on('careless:tick', () => {
        throw new Error('handler broke');
      });
      kcds.core.Events.on('consoleCommand', (command) => {
        if (command === 'fire') {
          // Neither awaited nor caught: the rejection is unhandled.
          kcds.core.Events.emit('careless:tick');
        }
      });
      `,
  lua: `
      kcds.core.Events.on("careless:tick", function()
        error("handler broke")
      end)
      kcds.core.Events.on("consoleCommand", function(command)
        if command == "fire" then
          -- Neither waited for nor caught: the rejection is unhandled.
          kcds.core.Events.emit("careless:tick")
        end
      end)
      `,
  // A C# task nobody awaits keeps its error to itself, as .NET's unobserved tasks do: the
  // resource carries on (the test checks that instead).
  csharp: `
public class Careless : Script
{
    public override void OnStart()
    {
        Events.On("careless:tick", () => { throw new InvalidOperationException("handler broke"); });
        Events.On("consoleCommand", (string command) =>
        {
            if (command == "fire")
            {
                _ = Events.Emit("careless:tick");
            }
        });
    }
}
`,
};

const CAREFUL = {
  javascript: `
      kcds.core.Events.on('careless:tick', () => console.log('careful handled'));
      kcds.core.Events.on('consoleCommand', (command) => {
        if (command === 'fire') {
          kcds.core.Events.emit('careless:tick').catch((error) => console.log('careful caught ' + error.name));
        }
      });
      `,
  lua: `
      kcds.core.Events.on("careless:tick", function() print("careful handled") end)
      kcds.core.Events.on("consoleCommand", function(command)
        if command == "fire" then
          kcds.core.Events.emit("careless:tick"):catch(function(problem)
            print("careful caught " .. problem.name)
          end)
        end
      end)
      `,
  csharp: `
public class Careful : Script
{
    public override void OnStart()
    {
        Events.On("careless:tick", () => Log.Info("careful handled"));
        Events.On("consoleCommand", async (string command) =>
        {
            if (command != "fire")
            {
                return;
            }
            try
            {
                await Events.Emit("careless:tick");
            }
            catch (JsError problem)
            {
                Log.Info("careful caught " + problem.Name);
            }
        });
    }
}
`,
};

// The mode resource emits on the shared bus; the scoreboard, in its own language, hears it.
async function customEventsShareOneBus(t, modeLanguage, scoreboardLanguage) {
  const f = serverFixture(t);
  serverResource(f.root, 'mode', MODE, {}, modeLanguage);
  serverResource(f.root, 'scoreboard', SCOREBOARD, {}, scoreboardLanguage);
  await f.host.startAll();
  f.report(8);
  await f.host.services.emit('shared', 'mode:run', []);
  await untilLogged(f, 'scoreboard', (line) => line === 'scoreboard saw leave 8');
  assert.deepEqual(
    f.linesOf('mode').filter((line) => line !== 'started'),
    [
      'mode heard Ravens',
      'mode private one',
      'count 2',
      `unlisten ${shown(modeLanguage, 'undefined')}`,
    ],
  );
  assert.deepEqual(
    f.linesOf('scoreboard').filter((line) => line !== 'started'),
    [
      'scoreboard heard Ravens',
      'scoreboard once',
      'scoreboard heard 42',
      'scoreboard heard Wolves',
      'scoreboard saw leave 8',
    ],
  );
  // The emitted native name did not touch the real player directory.
  assert.notEqual(f.host.api.Player.getById(8), null);
}

// Events between resources in different languages: every pair.
for (const modeLanguage of LANGUAGES) {
  for (const scoreboardLanguage of LANGUAGES) {
    if (modeLanguage === scoreboardLanguage) {
      continue;
    }
    test(`events: a ${modeLanguage} mode's events reach a ${scoreboardLanguage} scoreboard`, async (t) => {
      await customEventsShareOneBus(t, modeLanguage, scoreboardLanguage);
    });
  }
}

for (const language of LANGUAGES) {
  test(`events (${language}): on returns an unsubscribe, once returns nothing, off removes a handler`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'lobby', LOBBY, {}, language);
    await f.host.startAll();
    const player = f.report(3);
    f.host.receiveChat(player.id, 'first');
    await f.host.api.Events.emit('lobby:stop');
    f.host.receiveChat(player.id, 'second');
    await f.host.services.emit('shared', 'lobby:report', []);
    const report = JSON.parse(f.linesOf('lobby').find((line) => line.startsWith('{')));
    assert.deepEqual(report.seen, ['on:first', 'once:first']);
    assert.equal(report.stopType, 'function');
    assert.equal(report.onceResult, shown(language, 'undefined'));
  });

  test(`events (${language}): handlers run in order and their failures aggregate`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'rounds', ROUNDS, {}, language);
    await f.host.startAll();
    await f.host.services.emit('shared', 'rounds:run', []);
    await untilLogged(f, 'rounds', (line) => line.startsWith('{'));
    const lines = f.linesOf('rounds');
    const report = JSON.parse(lines.find((line) => line.startsWith('{')));
    assert.deepEqual(report.before, ['first:start', 'second:sync', 'third:start']);
    if (language === 'javascript') {
      // An async handler runs to its first await before emit returns, the rest after.
      assert.deepEqual(report.after, [
        'first:start',
        'second:sync',
        'third:start',
        'first:after-await',
      ]);
    } else {
      assert.deepEqual(report.after, report.before);
    }
    assert.equal(report.aggregate, true);
    assert.deepEqual(report.causes.sort(), ['second failed', 'third failed']);
    // The synchronous throw is also logged.
    const stack = language === 'javascript' ? '\n    at ' : '';
    assert.ok(lines.some((line) => line.includes('second failed') && line.includes(stack)));
  });

  test(`events (${language}): keep ids, not handles; teardown handlers still read the leaving player`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'queue', QUEUE, {}, language);
    await f.host.startAll();
    f.report(4);
    f.report(5);
    f.host.disconnect(4);
    await f.host.services.emit('shared', 'queue:announce', ['round starts']);
    await f.host.services.emit('shared', 'queue:lookup', [4]);
    const lines = f.linesOf('queue');
    assert.ok(lines.includes('leaving:Player 4:true'), lines.join('\n'));
    assert.ok(lines.includes(`lookup:4:${shown(language, 'null')}`), lines.join('\n'));
    const chat = f.sent.filter((message) => message.op === 'chat');
    assert.deepEqual(
      chat.map((message) => [message.target, message.text]),
      [[5, 'round starts']],
    );
  });

  test(`events (${language}): custom events share one bus; toResource, fire and listenerCount`, async (t) => {
    await customEventsShareOneBus(t, language, language);
  });

  test(`events (${language}): an unhandled emit rejection triggers the emitting resource errorBehavior`, async (t) => {
    const host = await hostProcess(t, (root) => {
      serverResource(root, 'careless', CARELESS, { errorBehavior: 'stop' }, language);
      serverResource(root, 'careful', CAREFUL, {}, language);
    });
    host.send({ op: 'console', text: 'fire' });
    if (language === 'csharp') {
      // A C# task nobody awaits is unobserved, as in .NET: the careless resource runs on.
      await host.waitFor(
        (frame) => frame.op === 'log' && frame.text === 'careful caught AggregateError',
        'the careful catch',
      );
      await delay(100);
      assert.ok(!host.logLines('careless').includes('stopped'));
    } else {
      await host.waitFor(
        (frame) => frame.op === 'log' && frame.resource === 'careless' && frame.text === 'stopped',
        'careless resource stop',
      );
    }
    await delay(50);
    assert.ok(host.logLines('careful').includes('careful caught AggregateError'));
    assert.ok(!host.logLines('careful').includes('stopped'));
    assert.equal(await host.stop(), 0);
    assert.deepEqual(host.errors, []);
  });
}

// Events G1: each server resource has its own vm context, and the bus's
// AggregateError still passes instanceof against the resource's own AggregateError
// (Backend/scripting/runtimes/js/realms.cjs). A JavaScript matter: Lua tells it by its name.
test('events: a server resource can test the emit failure with instanceof AggregateError', async (t) => {
  const f = serverFixture(t);
  serverResource(
    f.root,
    'realm',
    `
      kcds.core.Events.on('realm:fail', () => {
        throw new Error('fails');
      });
      kcds.core.Events.on('realm:run', async () => {
        try {
          await kcds.core.Events.emit('realm:fail');
        } catch (error) {
          console.log('instanceof ' + (error instanceof AggregateError));
        }
      });
      `,
  );
  await f.host.startAll();
  await f.host.services.emit('shared', 'realm:run', []);
  assert.ok(f.linesOf('realm').includes('instanceof true'));
});
