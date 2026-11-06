'use strict';

// Client and server events in resource scripts: a scoreboard request, JSON payloads and checks on
// what clients send. The server half is the production host process (main.cjs) over its IPC framing; the client half
// is the browser BrowserResourceRuntime. A small relay stands in for the native transport, which
// the two-client kit test Dev/testkit/guide_scenarios_roundtrip.py covers with real UDP clients.
// Each scenario runs with its scripts in JavaScript and in Lua; a Lua table has no key
// order, so the Lua scripts print what JavaScript prints as JSON text in a fixed form.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  LANGUAGES,
  delay,
  serverResource,
  clientFixture,
  hostProcess,
  shown,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

// Forwards the client runtime's toServer messages to the host process as one player's requests,
// and the host's events to the client runtime, parsed like the production worker: those for
// everyone (broadcast) and those for this player (player.emit).
function relay(host, client, playerId) {
  let clientSent = 0;
  let hostSent = 0;
  return async () => {
    while (clientSent < client.sent.length) {
      const message = client.sent[clientSent];
      clientSent += 1;
      if (message.op === 'event') {
        host.clientEvent(playerId, message.name, message.payload);
      }
    }
    await delay(30);
    const events = host.frames.filter((frame) => frame.op === 'event');
    while (hostSent < events.length) {
      const frame = events[hostSent];
      hostSent += 1;
      if (frame.player !== undefined && frame.player !== playerId) {
        continue;
      }
      await client.runtime.services.emit('shared', frame.name, [JSON.parse(frame.payload)]);
    }
  };
}

const SCOREBOARD_SERVER = {};
const SCOREBOARD_CLIENT = {};
const PAYLOADS_SERVER = {};
const PAYLOADS_CLIENT = {};
const FLARE_SERVER = {};

// The C# scripts.
SCOREBOARD_SERVER.csharp = `
public class Tally : Script
{
    private const string Resource = "tally";
    private readonly Dictionary<double, int> scores = new() { [21] = 5 };
    private readonly Dictionary<double, long> lastAsk = new();

    public override void OnStart()
    {
        Events.FromClient(Resource + ":scores.ask", (Player sender) =>
        {
            var player = sender;
            var now = Environment.TickCount64;
            // Throttle: one answer per player every 500 ms.
            if (lastAsk.TryGetValue(player.Id, out var previous) && now - previous < 500)
            {
                Log.Info("throttled " + player.Id);
                return;
            }
            lastAsk[player.Id] = now;
            var rows = Player.All()
                .Select((online) => new { name = online.Nickname, score = scores.GetValueOrDefault(online.Id) })
                .ToList();
            Log.Info("answer " + player.Id + " emit=" + player.AsValue()["emit"].Type);
            // Only the player who asked gets the rows.
            player.Emit(Resource + ":scores", System.Text.Json.JsonSerializer.Serialize(rows));
        });
        Events.On("playerDisconnect", (Player player) =>
        {
            lastAsk.Remove(player.Id);
            Log.Info("forgot " + player.Id + " size=" + lastAsk.Count);
        });
    }
}
`;

SCOREBOARD_CLIENT.csharp = `
public class TallyClient : Script
{
    public override void OnStart()
    {
        Events.On("tally:scores", (JsValue payload) =>
        {
            if (!payload.IsArray)
            {
                return;
            }
            Log.Info("rows " + payload.ToJson());
        });
        Events.On("resourceStart", (string name) =>
        {
            if (name == "tally")
            {
                Events.ToServer("tally:scores.ask");
            }
        });
    }
}
`;

PAYLOADS_SERVER.csharp = `
public class Payloads : Script
{
    public override void OnStart()
    {
        Events.On("consoleCommand", (string command) =>
        {
            if (command != "send")
            {
                return;
            }
            Events.Broadcast("payloads:mixed", new Dictionary<string, object?>
            {
                ["spot"] = new Vector3(1, 2, 3),
                ["facing"] = Quaternion.Identity(),
                ["list"] = new object?[] { 1, "two", true, null },
            });
            Events.Broadcast("payloads:text", "\\"already json\\"");
        });
        Events.FromClient("payloads:back", (Player sender, JsValue payload) =>
        {
            Log.Info("back " + payload.ToJson());
        });
    }
}
`;

// A received payload is plain data: no methods, no SDK types (C# reads it as a JsValue).
PAYLOADS_CLIENT.csharp = `
public class PayloadsClient : Script
{
    public override void OnStart()
    {
        Events.On("payloads:mixed", (JsValue payload) =>
        {
            Log.Info("mixed " + payload.ToJson());
            Log.Info("methods " + payload["spot"]["clone"]);
            Log.Info("fields " + string.Join(",", payload.Keys));
        });
        Events.On("payloads:text", (JsValue payload) => Log.Info("text " + payload));
        Events.On("payloads:reply", () =>
        {
            Events.ToServer("payloads:back", new { Spot = new Vector3(4, 5, 6), Missing = (object?)null });
            Events.ToServer("payloads:back", "hello");
            Events.ToServer("payloads:back", new { Text = "hello" });
        });
    }
}
`;

FLARE_SERVER.csharp = `
public class Flares : Script
{
    private const double MaxReach = 30;

    private static Vector3? ReadPosition(JsValue payload)
    {
        if (payload.IsNull || payload.Type != "object")
        {
            return null;
        }
        var (x, y, z) = (payload["x"], payload["y"], payload["z"]);
        if (x.Type != "number" || y.Type != "number" || z.Type != "number")
        {
            return null;
        }
        if (!double.IsFinite(x.AsNumber()) || !double.IsFinite(y.AsNumber()) || !double.IsFinite(z.AsNumber()))
        {
            return null;
        }
        return new Vector3(x.AsNumber(), y.AsNumber(), z.AsNumber());
    }

    public override void OnStart()
    {
        Events.FromClient("flares:light", (Player sender, JsValue payload) =>
        {
            var player = sender;
            var target = ReadPosition(payload);
            if (target == null || !player.Ready)
            {
                Log.Info("refused " + player.Id + " shape");
                return;
            }
            if (player.Position.Distance(target) > MaxReach)
            {
                Log.Info("refused " + player.Id + " reach");
                return;
            }
            // The sender is the connection; an id inside the payload is never trusted.
            Log.Info("lit by " + player.Id + " claimed " + payload["playerId"] + " at " + target);
        });
        Events.On("playerDied", () => Log.Info("native playerDied ran"));
        Events.On("flares:internal", () => Log.Info("internal ran"));
    }
}
`;

SCOREBOARD_SERVER.javascript = `
const RESOURCE = 'tally';
const scores = new Map();
const lastAsk = new Map();
kcds.core.Events.fromClient(RESOURCE + ':scores.ask', (sender) => {
  const player = sender;
  const now = Date.now();
  // Throttle: one answer per player every 500 ms.
  const previous = lastAsk.get(player.id) || 0;
  if (now - previous < 500) {
    console.log('throttled ' + player.id);
    return;
  }
  lastAsk.set(player.id, now);
  const rows = [];
  for (const online of kcds.players.Player.all()) {
    rows.push({ name: online.nickname, score: scores.get(online.id) || 0 });
  }
  console.log('answer ' + player.id + ' emit=' + typeof player.emit);
  // Only the player who asked gets the rows.
  player.emit(RESOURCE + ':scores', JSON.stringify(rows));
});
kcds.core.Events.on('playerDisconnect', (player) => {
  lastAsk.delete(player.id);
  console.log('forgot ' + player.id + ' size=' + lastAsk.size);
});
scores.set(21, 5);
`;

SCOREBOARD_SERVER.lua = `
local RESOURCE = "tally"
local scores = {}
local lastAsk = {}
kcds.core.Events.fromClient(RESOURCE .. ":scores.ask", function(sender)
  local player = sender
  local now = performance:now()
  -- Throttle: one answer per player every 500 ms.
  local previous = lastAsk[player.id]
  if previous and now - previous < 500 then
    print("throttled " .. player.id)
    return
  end
  lastAsk[player.id] = now
  local rows = {}
  for _, online in ipairs(kcds.players.Player.all()) do
    table.insert(rows, { name = online.nickname, score = scores[online.id] or 0 })
  end
  print("answer " .. player.id .. " emit=" .. type(player.emit))
  -- Only the player who asked gets the rows.
  player:emit(RESOURCE .. ":scores", json.encode(rows))
end)
kcds.core.Events.on("playerDisconnect", function(player)
  lastAsk[player.id] = nil
  local size = 0
  for _ in pairs(lastAsk) do
    size = size + 1
  end
  print("forgot " .. player.id .. " size=" .. size)
end)
scores[21] = 5
`;

SCOREBOARD_CLIENT.javascript = `
const RESOURCE = 'tally';
kcds.core.Events.on(RESOURCE + ':scores', (payload) => {
  if (!Array.isArray(payload)) {
    return;
  }
  console.log('rows ' + JSON.stringify(payload));
});
kcds.core.Events.on('resourceStart', (name) => {
  if (name === RESOURCE) {
    kcds.core.Events.toServer(RESOURCE + ':scores.ask');
  }
});
`;

SCOREBOARD_CLIENT.lua = `
local RESOURCE = "tally"
kcds.core.Events.on(RESOURCE .. ":scores", function(payload)
  if type(payload) ~= "userdata" or payload.length == nil then
    return
  end
  local rows = {}
  for _, row in ipairs(payload) do
    table.insert(rows, row.name .. "=" .. row.score)
  end
  print("rows " .. table.concat(rows, ","))
end)
kcds.core.Events.on("resourceStart", function(name)
  if name == RESOURCE then
    kcds.core.Events.toServer(RESOURCE .. ":scores.ask")
  end
end)
`;

const SCOREBOARD_ROWS = {
  javascript: 'rows [{"name":"Henry","score":5},{"name":"Hans","score":0}]',
  lua: 'rows Henry=5,Hans=0',
  csharp: 'rows [{"name":"Henry","score":5},{"name":"Hans","score":0}]',
};

for (const language of LANGUAGES) {
  test(`networking (${language}): a client asks for the scoreboard at resourceStart and the server answers`, async (t) => {
    const host = await hostProcess(t, (root) => {
      serverResource(root, 'tally', SCOREBOARD_SERVER, {}, language);
    });
    const client = clientFixture(t);
    host.report(21, { nickname: 'Henry' });
    host.report(22, { nickname: 'Hans' });
    const pump = relay(host, client, 21);
    await client.runtime.apply(
      { resources: [client.resource('tally', SCOREBOARD_CLIENT, {}, 'tally', language)] },
      client.read,
    );
    await pump();
    await host.waitFor(
      (frame) => frame.op === 'event' && frame.player === 21 && frame.name === 'tally:scores',
      'scoreboard answer',
    );
    await pump();
    assert.ok(host.logLines('tally').includes('answer 21 emit=function'));
    assert.ok(host.frames.some((frame) => frame.op === 'event' && frame.player === 21));
    assert.deepEqual(
      client.linesOf('tally').filter((line) => line.startsWith('rows ')),
      [SCOREBOARD_ROWS[language]],
    );
    // A second ask inside 500 ms is throttled; disconnect clears the player's entry.
    host.clientEvent(21, 'tally:scores.ask', 'null');
    await host.waitFor((frame) => frame.text === 'throttled 21', 'throttle');
    host.send({ op: 'disconnect', peer: 21, id: 21 });
    await host.waitFor((frame) => frame.text === 'forgot 21 size=0', 'cleanup');
    assert.equal(await host.stop(), 0);
    assert.deepEqual(host.errors, []);
  });

  PAYLOADS_SERVER.javascript = `
      kcds.core.Events.on('consoleCommand', (command) => {
        if (command !== 'send') {
          return;
        }
        kcds.core.Events.broadcast('payloads:mixed', {
          spot: new kcds.math.Vector3(1, 2, 3),
          facing: kcds.math.Quaternion.identity(),
          list: [1, 'two', true, null],
          skip: undefined,
          run() {},
        });
        kcds.core.Events.broadcast('payloads:text', '"already json"');
      });
      kcds.core.Events.fromClient('payloads:back', (sender, payload) => {
        console.log('back ' + JSON.stringify(payload));
      });
      `;

  // A Lua list cannot hold nil, and a Lua table's function is left out as JavaScript's method is.
  PAYLOADS_SERVER.lua = `
      kcds.core.Events.on("consoleCommand", function(command)
        if command ~= "send" then
          return
        end
        kcds.core.Events.broadcast("payloads:mixed", {
          spot = kcds.math.Vector3.new(1, 2, 3),
          facing = kcds.math.Quaternion.identity(),
          list = { 1, "two", true },
          run = function() end,
        })
        kcds.core.Events.broadcast("payloads:text", '"already json"')
      end)
      kcds.core.Events.fromClient("payloads:back", function(sender, payload)
        print("back " .. json.encode(payload))
      end)
      `;

  PAYLOADS_CLIENT.javascript = `
    kcds.core.Events.on('payloads:mixed', (payload) => {
      console.log('mixed ' + JSON.stringify(payload));
      console.log('methods ' + typeof payload.spot.clone + ' ' + (payload.spot instanceof kcds.math.Vector3));
      console.log('fields ' + Object.keys(payload).join(','));
    });
    kcds.core.Events.on('payloads:text', (payload) => console.log('text ' + payload));
    kcds.core.Events.on('payloads:reply', () => {
      kcds.core.Events.toServer('payloads:back', { spot: new kcds.math.Vector3(4, 5, 6), missing: undefined });
      kcds.core.Events.toServer('payloads:back', 'hello');
      kcds.core.Events.toServer('payloads:back', { text: 'hello' });
    });
  `;

  PAYLOADS_CLIENT.lua = `
    kcds.core.Events.on("payloads:mixed", function(payload)
      local spot, facing = payload.spot, payload.facing
      local list = {}
      for _, value in ipairs(payload.list) do
        table.insert(list, tostring(value))
      end
      print("mixed spot " .. spot.x .. "," .. spot.y .. "," .. spot.z .. " facing " .. facing.w
        .. "," .. facing.x .. "," .. facing.y .. "," .. facing.z .. " list " .. table.concat(list, ","))
      print("methods " .. type(spot.clone) .. " " .. type(payload.run))
    end)
    kcds.core.Events.on("payloads:text", function(payload) print("text " .. payload) end)
    kcds.core.Events.on("payloads:reply", function()
      kcds.core.Events.toServer("payloads:back", { spot = kcds.math.Vector3.new(4, 5, 6), missing = nil })
      kcds.core.Events.toServer("payloads:back", "hello")
      kcds.core.Events.toServer("payloads:back", { text = "hello" })
    end)
  `;

  const PAYLOADS_SEEN = {
    javascript: [
      'mixed {"spot":{"x":1,"y":2,"z":3},"facing":{"w":1,"x":0,"y":0,"z":0},"list":[1,"two",true,null]}',
      'methods undefined false',
      'fields spot,facing,list',
    ],
    lua: ['mixed spot 1,2,3 facing 1,0,0,0 list 1,two,true', 'methods nil nil'],
    csharp: [
      'mixed {"spot":{"x":1,"y":2,"z":3},"facing":{"w":1,"x":0,"y":0,"z":0},"list":[1,"two",true,null]}',
      'methods null',
      'fields spot,facing,list',
    ],
  };

  test(`networking (${language}): payloads cross as JSON and arrive as plain parsed values`, async (t) => {
    const host = await hostProcess(t, (root) => {
      serverResource(root, 'payloads', PAYLOADS_SERVER, {}, language);
    });
    const client = clientFixture(t);
    host.report(31);
    const pump = relay(host, client, 31);
    await client.runtime.apply(
      { resources: [client.resource('payloads', PAYLOADS_CLIENT, {}, 'payloads', language)] },
      client.read,
    );
    host.send({ op: 'console', text: 'send' });
    await host.waitFor(
      (frame) => frame.op === 'event' && frame.name === 'payloads:text',
      'payloads',
    );
    await pump();
    const lines = client.linesOf('payloads');
    for (const line of PAYLOADS_SEEN[language]) {
      assert.ok(lines.includes(line), `${line}\n${lines.join('\n')}`);
    }
    assert.ok(lines.includes('text already json'));
    await client.runtime.services.emit('shared', 'payloads:reply', []);
    await pump();
    await host.waitFor((frame) => frame.text === 'back {"text":"hello"}', 'wrapped text');
    // A plain string that is not JSON text arrives as that string.
    assert.deepEqual(
      host.logLines('payloads').filter((line) => line.startsWith('back ')),
      ['back {"spot":{"x":4,"y":5,"z":6}}', 'back "hello"', 'back {"text":"hello"}'],
    );
    assert.ok(!host.logLines('host').some((line) => line.includes('malformed JSON payload')));
    assert.equal(await host.stop(), 0);
  });

  FLARE_SERVER.javascript = `
const MAX_REACH = 30;
function readPosition(payload) {
  if (typeof payload !== 'object' || payload === null) {
    return null;
  }
  const { x, y, z } = payload;
  if (typeof x !== 'number' || typeof y !== 'number' || typeof z !== 'number') {
    return null;
  }
  if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) {
    return null;
  }
  return new kcds.math.Vector3(x, y, z);
}
kcds.core.Events.fromClient('flares:light', (sender, payload) => {
  const player = sender;
  const target = readPosition(payload);
  if (!target || !player.ready) {
    console.log('refused ' + player.id + ' shape');
    return;
  }
  if (player.position.distance(target) > MAX_REACH) {
    console.log('refused ' + player.id + ' reach');
    return;
  }
  // The sender is the connection; an id inside the payload is never trusted.
  console.log('lit by ' + player.id + ' claimed ' + payload.playerId + ' at ' + target.toString());
});
kcds.core.Events.on('playerDied', () => console.log('native playerDied ran'));
kcds.core.Events.on('flares:internal', () => console.log('internal ran'));
`;

  FLARE_SERVER.lua = `
local MAX_REACH = 30
local function finite(value)
  return type(value) == "number" and value == value and math.abs(value) ~= math.huge
end
local function readPosition(payload)
  if type(payload) ~= "userdata" then
    return nil
  end
  local x, y, z = payload.x, payload.y, payload.z
  if not (finite(x) and finite(y) and finite(z)) then
    return nil
  end
  return kcds.math.Vector3.new(x, y, z)
end
kcds.core.Events.fromClient("flares:light", function(sender, payload)
  local player = sender
  local target = readPosition(payload)
  if not target or not player.ready then
    print("refused " .. player.id .. " shape")
    return
  end
  if player.position:distance(target) > MAX_REACH then
    print("refused " .. player.id .. " reach")
    return
  end
  -- The sender is the connection; an id inside the payload is never trusted.
  print("lit by " .. player.id .. " claimed " .. tostring(payload.playerId) .. " at " .. target:toString())
end)
kcds.core.Events.on("playerDied", function() print("native playerDied ran") end)
kcds.core.Events.on("flares:internal", function() print("internal ran") end)
`;

  test(`networking (${language}): validate everything a client sends; the sender cannot be forged`, async (t) => {
    const host = await hostProcess(t, (root) => {
      serverResource(root, 'flares', FLARE_SERVER, {}, language);
    });
    host.report(41, { position: { x: 100, y: 100, z: 10 } });
    host.report(42, { position: { x: 0, y: 0, z: 0 }, ready: false });
    // NaN cannot cross JSON: it arrives as null and fails the shape check.
    host.clientEvent(41, 'flares:light', JSON.stringify({ x: NaN, y: 1, z: 1 }));
    host.clientEvent(41, 'flares:light', JSON.stringify({ x: '100', y: 100, z: 10 }));
    host.clientEvent(41, 'flares:light', JSON.stringify({ x: 500, y: 100, z: 10 }));
    host.clientEvent(42, 'flares:light', JSON.stringify({ x: 1, y: 1, z: 1 }));
    host.clientEvent(41, 'flares:light', JSON.stringify({ x: 110, y: 100, z: 10, playerId: 42 }));
    // A client reaches only fromClient handlers: never a native event or a shared custom event.
    host.clientEvent(41, 'playerDied', '{}');
    host.clientEvent(41, 'flares:internal', '{}');
    host.clientEvent(41, 'flares:light', JSON.stringify({ x: 101, y: 100, z: 10 }));
    await host.waitFor((frame) => frame.text?.includes('Vector3(101, 100, 10)'), 'last flare');
    assert.deepEqual(
      host.logLines('flares').filter((line) => line !== 'started'),
      [
        'refused 41 shape',
        'refused 41 shape',
        'refused 41 reach',
        'refused 42 shape',
        'lit by 41 claimed 42 at Vector3(110, 100, 10)',
        `lit by 41 claimed ${shown(language, 'undefined')} at Vector3(101, 100, 10)`,
      ],
    );
    assert.equal(await host.stop(), 0);
    assert.deepEqual(host.errors, []);
  });
}
