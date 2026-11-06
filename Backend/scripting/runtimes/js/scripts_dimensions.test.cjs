'use strict';

// Dimensions in resource scripts: arenas, who sees whom, and cleaning up.
// Who sees what is decided by the native server; Dev/testkit/player_visibility_roundtrip.py and
// Dev/testkit/guide_scenarios_roundtrip.py check that with real clients. This suite checks the
// script side of the arena walkthrough: worlds, requests, chat and events. Each scenario runs with
// its scripts in JavaScript, in Lua and in C#.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  GLOBAL_WORLD,
  LANGUAGES,
  serverResource,
  serverFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const ARENA = {};
const MOVER = {
  javascript: `
    kcds.core.Events.on('playerCommand', (player, command, args) => {
      if (command === 'world') {
        player.setDimension(Number(args[0]));
        console.log('reads ' + player.dimension);
      }
    });
    `,
  lua: `
    kcds.core.Events.on("playerCommand", function(player, command, args)
      if command == "world" then
        player:setDimension(tonumber(args[1]))
        print("reads " .. player.dimension)
      end
    end)
    `,
  csharp: `
public class Mover : Script
{
    public override void OnStart()
    {
        Events.On("playerCommand", (Player player, string command, string[] args) =>
        {
            if (command == "world")
            {
                player.SetDimension(double.Parse(args[0]));
                Log.Info("reads " + player.Dimension);
            }
        });
    }
}
`,
};

ARENA.csharp = `
public class Arena : Script
{
    private const double GlobalWorld = 4294967295;
    private double nextWorld = 1;
    private readonly Dictionary<double, List<double>> arenas = new();

    private double OpenArena(Player[] players, Vector3 centre)
    {
        var world = nextWorld;
        nextWorld += 1;
        foreach (var player in players)
        {
            player.SetDimension(world);
        }
        arenas[world] = players.Select((player) => player.Id).ToList();
        // Horses take no world at spawn: spawn, then move it.
        var banner = Horse.Spawn(centre, null, null, "arena " + world);
        banner.SetDimension(world);
        return world;
    }

    private void CloseArena(double world)
    {
        foreach (var id in arenas.GetValueOrDefault(world) ?? new List<double>())
        {
            Player.GetById(id)?.SetDimension(GlobalWorld);
        }
        arenas.Remove(world);
        foreach (var horse in Horse.All())
        {
            if (horse.Dimension == world)
            {
                horse.Destroy();
            }
        }
        Log.Info("closed " + world);
    }

    // Chat is not split by worlds: answer only the players in the speaker's world.
    private static void SayInWorld(Player speaker, string text)
    {
        foreach (var player in Player.All())
        {
            if (player.Dimension == speaker.Dimension)
            {
                Chat.SendToPlayer(player, text);
            }
        }
    }

    public override void OnStart()
    {
        Events.On("playerCommand", (Player player, string command, string[] args) =>
        {
            if (command == "duel")
            {
                var rival = Player.GetById(double.Parse(args[0]))!;
                var world = OpenArena(new[] { player, rival }, player.Position);
                Log.Info("arena " + world + " worlds " + player.Dimension + "," + rival.Dimension);
                return;
            }
            if (command == "arena-say")
            {
                SayInWorld(player, string.Join(" ", args));
                return;
            }
            if (command == "hint")
            {
                // null lifts the restriction.
                player.ShowOnlyTo(Player.GetById(double.Parse(args[0])));
                return;
            }
            if (command == "bad-world")
            {
                try
                {
                    player.SetDimension(-1);
                }
                catch (JsError problem)
                {
                    Log.Info("refused " + problem.Message);
                }
            }
        });
        Events.On("playerChat", (Player player, string text) =>
        {
            Log.Info("chat event from world " + player.Dimension);
        });
        Events.On("playerDisconnect", (Player player) =>
        {
            foreach (var (world, ids) in arenas.ToList())
            {
                var left = ids.Where((id) => id != player.Id).ToList();
                if (left.Count == 0)
                {
                    CloseArena(world);
                }
                else
                {
                    arenas[world] = left;
                }
            }
        });
    }
}
`;

ARENA.lua = `
local GLOBAL_WORLD = 4294967295
local nextWorld = 1
local arenas = {}

local function openArena(players, centre)
  local world = nextWorld
  nextWorld = nextWorld + 1
  local ids = {}
  for _, player in ipairs(players) do
    player:setDimension(world)
    table.insert(ids, player.id)
  end
  arenas[world] = ids
  -- Horses take no world at spawn: spawn, then move it.
  local banner = kcds.characters.Horse.spawn(centre, nil, nil, "arena " .. world)
  banner:setDimension(world)
  return world
end

local function closeArena(world)
  for _, id in ipairs(arenas[world] or {}) do
    local player = kcds.players.Player.getById(id)
    if player then
      player:setDimension(GLOBAL_WORLD)
    end
  end
  arenas[world] = nil
  for _, horse in ipairs(kcds.characters.Horse.all()) do
    if horse.dimension == world then
      horse:destroy()
    end
  end
  print("closed " .. world)
end

-- Chat is not split by worlds: answer only the players in the speaker's world.
local function sayInWorld(speaker, text)
  for _, player in ipairs(kcds.players.Player.all()) do
    if player.dimension == speaker.dimension then
      kcds.core.Chat.sendToPlayer(player, text)
    end
  end
end

kcds.core.Events.on("playerCommand", function(player, command, args)
  if command == "duel" then
    local rival = kcds.players.Player.getById(tonumber(args[1]))
    local world = openArena({ player, rival }, player.position)
    print("arena " .. world .. " worlds " .. player.dimension .. "," .. rival.dimension)
    return
  end
  if command == "arena-say" then
    sayInWorld(player, table.concat(args, " "))
    return
  end
  if command == "hint" then
    -- null lifts the restriction (nil would be "not given").
    player:showOnlyTo(kcds.players.Player.getById(tonumber(args[1])) or null)
    return
  end
  if command == "bad-world" then
    local ok, problem = pcall(function() player:setDimension(-1) end)
    if not ok then
      print("refused " .. problem.message)
    end
  end
end)
kcds.core.Events.on("playerChat", function(player, text)
  print("chat event from world " .. player.dimension)
end)
kcds.core.Events.on("playerDisconnect", function(player)
  for world, ids in pairs(arenas) do
    local left = {}
    for _, id in ipairs(ids) do
      if id ~= player.id then
        table.insert(left, id)
      end
    end
    if #left == 0 then
      closeArena(world)
    else
      arenas[world] = left
    end
  end
end)
`;

ARENA.javascript = `
const GLOBAL_WORLD = 4294967295;
let nextWorld = 1;
const arenas = new Map();

function openArena(players, centre) {
  const world = nextWorld;
  nextWorld += 1;
  for (const player of players) {
    player.setDimension(world);
  }
  arenas.set(world, players.map((player) => player.id));
  // Horses take no world at spawn: spawn, then move it.
  const banner = kcds.characters.Horse.spawn(centre, undefined, undefined, 'arena ' + world);
  banner.setDimension(world);
  return world;
}

function closeArena(world) {
  for (const id of arenas.get(world) ?? []) {
    kcds.players.Player.getById(id)?.setDimension(GLOBAL_WORLD);
  }
  arenas.delete(world);
  for (const horse of kcds.characters.Horse.all()) {
    if (horse.dimension === world) {
      horse.destroy();
    }
  }
  console.log('closed ' + world);
}

// Chat is not split by worlds: answer only the players in the speaker's world.
function sayInWorld(speaker, text) {
  for (const player of kcds.players.Player.all()) {
    if (player.dimension === speaker.dimension) {
      kcds.core.Chat.sendToPlayer(player, text);
    }
  }
}

kcds.core.Events.on('playerCommand', (player, command, args) => {
  if (command === 'duel') {
    const rival = kcds.players.Player.getById(Number(args[0]));
    const world = openArena([player, rival], player.position);
    console.log('arena ' + world + ' worlds ' + player.dimension + ',' + rival.dimension);
    return;
  }
  if (command === 'arena-say') {
    sayInWorld(player, args.join(' '));
    return;
  }
  if (command === 'hint') {
    player.showOnlyTo(kcds.players.Player.getById(Number(args[0])) ?? null);
    return;
  }
  if (command === 'bad-world') {
    try {
      player.setDimension(-1);
    } catch (error) {
      console.log('refused ' + error.message);
    }
  }
});
kcds.core.Events.on('playerChat', (player, text) => {
  console.log('chat event from world ' + player.dimension);
});
kcds.core.Events.on('playerDisconnect', (player) => {
  for (const [world, ids] of arenas) {
    const left = ids.filter((id) => id !== player.id);
    if (left.length === 0) {
      closeArena(world);
    } else {
      arenas.set(world, left);
    }
  }
});
`;

for (const language of LANGUAGES) {
  test(`dimensions (${language}): an arena moves two players into a world and cleans up after itself`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'arena', ARENA, {}, language);
    await f.host.startAll();
    const henry = f.report(91, { position: { x: 1, y: 2, z: 3 } });
    const hans = f.report(92);
    const watcher = f.report(93);
    assert.equal(henry.dimension, GLOBAL_WORLD);

    f.host.receiveChat(91, '/duel 92');
    assert.ok(f.linesOf('arena').includes('arena 1 worlds 1,1'));
    const worldRequests = f.sent.filter((message) => message.op === 'entityWorld');
    assert.deepEqual(
      worldRequests.map((message) => [message.id, message.dimension]),
      [
        [91, 1],
        [92, 1],
      ],
    );
    const [banner] = f.host.horses.values();
    assert.equal(banner.dimension, 1);
    assert.deepEqual(
      f.sent.filter((message) => message.op === 'horseWorld').map((message) => message.dimension),
      [1],
    );

    // Chat and events are not split: the plain chat line goes to everyone, the handler still runs.
    f.host.receiveChat(91, 'good luck');
    const relay = f.sent.filter((message) => message.op === 'chat' && message.text === 'good luck');
    assert.deepEqual(
      relay.map((message) => message.target),
      [0],
    );
    assert.ok(f.linesOf('arena').includes('chat event from world 1'));
    f.host.receiveChat(91, '/arena-say round one');
    const scoped = f.sent.filter(
      (message) => message.op === 'chat' && message.text === 'round one',
    );
    assert.deepEqual(scoped.map((message) => message.target).sort(), [91, 92]);

    // Private visibility: one entity shown to one player, then the restriction lifted.
    f.host.receiveChat(93, '/hint 91');
    f.host.receiveChat(93, '/hint 0');
    assert.deepEqual(
      f.sent
        .filter((message) => message.op === 'entityVisibleTo')
        .map((message) => [message.id, message.playerId]),
      [
        [93, 91],
        [93, 0],
      ],
    );
    f.host.receiveChat(93, '/bad-world');
    assert.ok(f.linesOf('arena').includes('refused Invalid dimension'));
    assert.equal(watcher.dimension, GLOBAL_WORLD);

    // Both duelists leave: the last one closes the arena, the survivors' worlds return to global.
    f.host.disconnect(92);
    assert.equal(henry.dimension, 1);
    f.host.disconnect(91);
    assert.ok(f.linesOf('arena').includes('closed 1'));
    assert.equal(f.host.horses.size, 0);
  });

  test(`dimensions (${language}): reads settle on the authoritative world the server reports`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'mover', MOVER, {}, language);
    await f.host.startAll();
    const player = f.report(94);
    f.host.receiveChat(94, '/world 5');
    const request = f.sent.find((message) => message.op === 'entityWorld');
    // A periodic report produced before the server applied the request does not undo the read.
    f.report(94, { dimension: GLOBAL_WORLD });
    assert.equal(player.dimension, 5);
    f.host.confirmPlayerWorld({ id: 94, request: request.request, dimension: 5 });
    f.report(94, { dimension: 5 });
    assert.equal(player.dimension, 5);
    assert.ok(f.linesOf('mover').includes('reads 5'));
  });
}
