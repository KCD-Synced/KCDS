'use strict';

// Dice scripts as a server resource would write them, run on the real ResourceHost: a /challenge
// command, table discovery, stopping a game, the turn log and a wager. Each scenario runs with its
// scripts in JavaScript, in Lua and in C#.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  LANGUAGES,
  serverResource,
  serverFixture,
  untilLogged,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { TABLES } = require('./dice/dice.cjs');

function outputOf(linesOf, name) {
  return linesOf(name).filter((line) => line !== 'started');
}

// Wraps code in a /challenge handler with `challenger` and `rival` in scope.
function onChallenge(code) {
  return {
    javascript: `
kcds.core.Events.on("playerCommand", (challenger, command) => {
  if (command !== "challenge") return;
  const rival = kcds.players.Player.all().find((candidate) => candidate.id !== challenger.id);
${code.javascript}
});
`,
    lua: `
kcds.core.Events.on("playerCommand", function(challenger, command)
  if command ~= "challenge" then
    return
  end
  local rival
  for _, candidate in ipairs(kcds.players.Player.all()) do
    if candidate.id ~= challenger.id then
      rival = candidate
      break
    end
  end
${code.lua}
end)
`,
    csharp: `
public class Tavern : Script
{
    public override void OnStart()
    {
        Events.On("playerCommand", (Player challenger, string command) =>
        {
            if (command != "challenge")
            {
                return;
            }
            var rival = Player.All().First((candidate) => candidate.Id != challenger.Id);
${code.csharp}
        });
    }

    // The seat's player, or null for 255 (no winner).
    private static Player? SeatOf(double seat, Player? seatA, Player? seatB)
    {
        if (seat == 0)
        {
            return seatA;
        }
        return seat == 1 ? seatB : null;
    }
}
`,
  };
}

// Starts the resource and reports two players at the first table, or far apart.
async function seatPlayers(t, name, code, language, apart = false) {
  const fixture = serverFixture(t);
  serverResource(fixture.root, name, code, {}, language);
  await fixture.host.startAll();
  const table = TABLES[0];
  const near = (offset) => ({
    x: table.position.x + offset,
    y: table.position.y,
    z: table.position.z,
  });
  const first = fixture.report(7, { position: near(0.5) });
  const second = fixture.report(8, { position: apart ? near(40) : near(-0.5) });
  return { ...fixture, api: fixture.host.makeAPI(), first, second };
}

function chatLines(sent) {
  return sent.filter((message) => message.op === 'chat').map((message) => message.text);
}

const CHALLENGE = onChallenge({
  javascript: `
const game = kcds.world.Dice.start(challenger.id, rival.id, { targetScore: 2000 });

kcds.core.Events.on("diceMatchEnd", (ended, seatA, seatB, winningSeat) => {
  if (ended !== game) return;
  const champion = [seatA, seatB][winningSeat];
  if (champion) kcds.core.Chat.sendToAll(\`Champion of the table: \${champion.nickname}\`);
});
`,
  lua: `
  local game = kcds.world.Dice.start(challenger.id, rival.id, { targetScore = 2000 })

  kcds.core.Events.on("diceMatchEnd", function(ended, seatA, seatB, winningSeat)
    if ended ~= game then
      return
    end
    local champion = ({ seatA, seatB })[winningSeat + 1]
    if champion then
      kcds.core.Chat.sendToAll("Champion of the table: " .. champion.nickname)
    end
  end)
`,
  csharp: `
            var game = Dice.Start(challenger.Id, rival.Id, new { targetScore = 2000 });
            Events.On("diceMatchEnd", (double ended, Player? seatA, Player? seatB, double winningSeat) =>
            {
                if (ended != game)
                {
                    return;
                }
                var champion = SeatOf(winningSeat, seatA, seatB);
                if (champion != null)
                {
                    Chat.SendToAll("Champion of the table: " + champion.Nickname);
                }
            });
`,
});

const REFUSAL = onChallenge({
  javascript: `
try {
  kcds.world.Dice.start(challenger.id, rival.id);
} catch (problem) {
  kcds.core.Chat.sendToPlayer(challenger, \`Cannot play: \${problem.message}\`);
}
`,
  lua: `
  local ok, problem = pcall(kcds.world.Dice.start, challenger.id, rival.id)
  if not ok then
    kcds.core.Chat.sendToPlayer(challenger, "Cannot play: " .. problem.message)
  end
`,
  csharp: `
            try
            {
                Dice.Start(challenger.Id, rival.Id);
            }
            catch (JsError problem)
            {
                Chat.SendToPlayer(challenger, "Cannot play: " + problem.Message);
            }
`,
});

const MARK_TABLES = {
  javascript: `
const zones = kcds.world.Dice.tables().map((spot) =>
  kcds.world.Area.create({
    id: "gambling-" + spot.id,
    type: "sphere",
    position: spot.position,
    radius: 4,
    labels: ["gambling"],
  }),
);
console.log(zones.length);
`,
  lua: `
local zones = {}
for _, spot in ipairs(kcds.world.Dice.tables()) do
  table.insert(zones, kcds.world.Area.create({
    id = "gambling-" .. spot.id,
    type = "sphere",
    position = spot.position,
    radius = 4,
    labels = { "gambling" },
  }))
end
print(#zones)
`,
  csharp: `
public class Zones : Script
{
    public override void OnStart()
    {
        var zones = new List<Area>();
        foreach (var spot in Dice.Tables())
        {
            zones.Add(Area.Create(new
            {
                id = "gambling-" + spot["id"],
                type = "sphere",
                position = spot["position"],
                radius = 4,
                labels = new[] { "gambling" },
            }));
        }
        Log.Info(zones.Count.ToString());
    }
}
`,
};

const CANCEL = onChallenge({
  javascript: `
kcds.core.Events.on("diceMatchEnd", (ended, seatA, seatB, winningSeat, why) => {
  console.log("seat " + winningSeat + ", " + why);
});
kcds.world.Dice.start(challenger.id, rival.id);
const running = kcds.world.Dice.gameOf(challenger.id);
kcds.world.Dice.stop(running);
const afterwards = kcds.world.Dice.gameOf(challenger.id);
console.log("now " + afterwards + ", again " + kcds.world.Dice.stop(running));
`,
  lua: `
  kcds.core.Events.on("diceMatchEnd", function(ended, seatA, seatB, winningSeat, why)
    print("seat " .. winningSeat .. ", " .. why)
  end)
  kcds.world.Dice.start(challenger.id, rival.id)
  local running = kcds.world.Dice.gameOf(challenger.id)
  kcds.world.Dice.stop(running)
  local afterwards = kcds.world.Dice.gameOf(challenger.id)
  print("now " .. afterwards .. ", again " .. tostring(kcds.world.Dice.stop(running)))
`,
  csharp: `
            Events.On("diceMatchEnd", (double ended, Player? seatA, Player? seatB, double winningSeat, string why) =>
            {
                Log.Info("seat " + winningSeat + ", " + why);
            });
            Dice.Start(challenger.Id, rival.Id);
            var running = Dice.GameOf(challenger.Id);
            Dice.Stop(running);
            var afterwards = Dice.GameOf(challenger.Id);
            var again = Dice.Stop(running);
            Log.Info("now " + afterwards + ", again " + (again ? "true" : "false"));
`,
});

const SCOREBOARD = onChallenge({
  javascript: `
const game = kcds.world.Dice.start(challenger.id, rival.id, {
  firstName: challenger.nickname,
  secondName: rival.nickname,
});
kcds.world.Dice.setNames(game, challenger.nickname, rival.nickname);
`,
  lua: `
  local game = kcds.world.Dice.start(challenger.id, rival.id, {
    firstName = challenger.nickname,
    secondName = rival.nickname,
  })
  kcds.world.Dice.setNames(game, challenger.nickname, rival.nickname)
`,
  csharp: `
            var game = Dice.Start(challenger.Id, rival.Id, new
            {
                firstName = challenger.Nickname,
                secondName = rival.Nickname,
            });
            Dice.SetNames(game, challenger.Nickname, rival.Nickname);
`,
});

const TURN_LOG = {
  javascript: `
const write = (...parts) => console.log(parts.join(" | "));
kcds.core.Events.on("diceTurnStart", (info) => write("begin", info.match, info.turn, info.player));
kcds.core.Events.on("diceRoll", (info) => write("dice", info.roll, info.faces.join(""), info.reason));
kcds.core.Events.on("diceMove", (info) => write("move", info.reason, info.points, info.heldMask));
kcds.core.Events.on("diceTurnEnd", (info) => write("finish", info.reason, info.totalScore));
`,
  lua: `
local function write(...)
  local parts = {}
  for index = 1, select("#", ...) do
    parts[index] = tostring((select(index, ...)))
  end
  print(table.concat(parts, " | "))
end
kcds.core.Events.on("diceTurnStart", function(info) write("begin", info.match, info.turn, info.player) end)
kcds.core.Events.on("diceRoll", function(info)
  write("dice", info.roll, table.concat(info.faces, ""), info.reason)
end)
kcds.core.Events.on("diceMove", function(info) write("move", info.reason, info.points, info.heldMask) end)
kcds.core.Events.on("diceTurnEnd", function(info) write("finish", info.reason, info.totalScore) end)
`,
  csharp: `
public class TurnLog : Script
{
    private static void Write(params object[] parts)
    {
        Log.Info(string.Join(" | ", parts));
    }

    public override void OnStart()
    {
        Events.On("diceTurnStart", (JsValue info) => Write("begin", info["match"], info["turn"], info["player"]));
        Events.On("diceRoll", (JsValue info) =>
        {
            Write("dice", info["roll"], string.Join("", info["faces"]), info["reason"]);
        });
        Events.On("diceMove", (JsValue info) => Write("move", info["reason"], info["points"], info["heldMask"]));
        Events.On("diceTurnEnd", (JsValue info) => Write("finish", info["reason"], info["totalScore"]));
    }
}
`,
};

const WAGER = {
  javascript: `
const ANTE = 50;
const bank = new Map();

kcds.core.Events.on("diceMatchStart", async (game, seatA, seatB) => {
  const first = await seatA.takeItem("money", ANTE);
  const second = await seatB.takeItem("money", ANTE);
  bank.set(game, first.removed + second.removed);
  console.log("collected " + bank.get(game));
});

kcds.core.Events.on("diceMatchEnd", (game, seatA, seatB, winningSeat) => {
  const prize = bank.get(game) ?? 0;
  bank.delete(game);
  const champion = [seatA, seatB][winningSeat];
  if (champion) {
    champion.giveItem("money", prize);
  } else {
    // Without a winner, each seat gets its share back.
    const share = Math.floor(prize / 2);
    seatA?.giveItem("money", share);
    seatB?.giveItem("money", share);
  }
  console.log("awarded " + prize);
});
`,
  lua: `
local ANTE = 50
local bank = {}

kcds.core.Events.on("diceMatchStart", function(game, seatA, seatB)
  local first
  return seatA:takeItem("money", ANTE)
    :next(function(taken)
      first = taken
      return seatB:takeItem("money", ANTE)
    end)
    :next(function(second)
      bank[game] = first.removed + second.removed
      print("collected " .. bank[game])
    end)
end)

kcds.core.Events.on("diceMatchEnd", function(game, seatA, seatB, winningSeat)
  local prize = bank[game] or 0
  bank[game] = nil
  local champion = ({ seatA, seatB })[winningSeat + 1]
  if champion then
    champion:giveItem("money", prize)
  else
    -- Without a winner, each seat gets its share back.
    local share = math.floor(prize / 2)
    if seatA then
      seatA:giveItem("money", share)
    end
    if seatB then
      seatB:giveItem("money", share)
    end
  end
  print("awarded " .. prize)
end)
`,
  csharp: `
public class Wager : Script
{
    private const double Ante = 50;
    private readonly Dictionary<double, double> bank = new();

    public override void OnStart()
    {
        Events.On("diceMatchStart", async (double game, Player seatA, Player seatB) =>
        {
            var first = await seatA.TakeItem("money", Ante);
            var second = await seatB.TakeItem("money", Ante);
            bank[game] = first["removed"].AsNumber() + second["removed"].AsNumber();
            Log.Info("collected " + bank[game]);
        });
        Events.On("diceMatchEnd", (double game, Player? seatA, Player? seatB, double winningSeat) =>
        {
            var prize = bank.GetValueOrDefault(game);
            bank.Remove(game);
            var champion = winningSeat == 0 ? seatA : winningSeat == 1 ? seatB : null;
            if (champion != null)
            {
                champion.GiveItem("money", prize);
            }
            else
            {
                // Without a winner, each seat gets its share back.
                var share = Math.Floor(prize / 2);
                seatA?.GiveItem("money", share);
                seatB?.GiveItem("money", share);
            }
            Log.Info("awarded " + prize);
        });
    }
}
`,
};

for (const language of LANGUAGES) {
  test(`dice (${language}): a command starts a game and the script announces the winner`, async (t) => {
    const { host, sent, first, second } = await seatPlayers(t, 'tavern', CHALLENGE, language);
    host.emit('playerCommand', first, 'challenge', []);
    const game = host.dice.byPlayer.get(first.id);
    assert.ok(game > 0);
    assert.equal(host.dice.matches.get(game).target, 2000);
    // The other player gives up, so the challenger wins.
    host.dice.clientMove(second.id, game, 'gaveUp', 0);
    assert.deepEqual(chatLines(sent), ['Champion of the table: Player 7']);
  });

  test(`dice (${language}): players apart cannot start, and the script can report the reason`, async (t) => {
    const { host, sent, first } = await seatPlayers(t, 'tavern', REFUSAL, language, true);
    host.emit('playerCommand', first, 'challenge', []);
    assert.deepEqual(chatLines(sent), [
      'Cannot play: Dice.start: both players must stand together at a dice table',
    ]);
  });

  test(`dice (${language}): a script can mark every table with an area`, async (t) => {
    const { root, host, linesOf } = serverFixture(t);
    serverResource(root, 'zones', MARK_TABLES, {}, language);
    await host.startAll();
    assert.deepEqual(outputOf(linesOf, 'zones'), [String(TABLES.length)]);
  });

  test(`dice (${language}): stopping a game has no winner and a second stop is refused`, async (t) => {
    const { host, linesOf, first } = await seatPlayers(t, 'tavern', CANCEL, language);
    host.emit('playerCommand', first, 'challenge', []);
    assert.deepEqual(outputOf(linesOf, 'tavern'), ['seat 255, stopped', 'now 0, again false']);
  });

  test(`dice (${language}): each client sees its own name first on the scoreboard`, async (t) => {
    const { host, sent, first } = await seatPlayers(t, 'tavern', SCOREBOARD, language);
    host.emit('playerCommand', first, 'challenge', []);
    const updates = sent.filter((message) => message.op === 'diceState');
    const namesFor = (kind) =>
      updates
        .filter((update) => update.kind === kind)
        .map((update) => [update.id, update.ownName, update.opponentName]);
    const seen = [
      [7, 'Player 7', 'Player 8'],
      [8, 'Player 8', 'Player 7'],
    ];
    assert.deepEqual(namesFor('start'), seen);
    assert.deepEqual(namesFor('names'), seen);
  });

  test(`dice (${language}): turn events arrive in order and a stop closes the open turn`, async (t) => {
    const { host, api, linesOf, first, second } = await seatPlayers(
      t,
      'turnlog',
      TURN_LOG,
      language,
    );
    const game = api.Dice.start(first.id, second.id);
    const state = host.dice.matches.get(game);
    const roller = state.players[state.seat];
    host.dice.clientMove(roller, game, 'throw', 0);
    const lines = outputOf(linesOf, 'turnlog');
    assert.equal(lines[0], `begin | ${game} | 1 | ${roller}`);
    assert.match(lines[1], /^dice \| 1 \| [1-6]{6} \| (rolled|bust)$/);
    if (lines[1].endsWith('bust')) {
      // A roll with no scoring dice passes the turn straight away.
      assert.equal(lines[2], 'finish | bust | 0');
      assert.match(lines[3], new RegExp(`^begin \\| ${game} \\| 2 \\| `));
    }
    const already = outputOf(linesOf, 'turnlog').length;
    api.Dice.stop(game);
    assert.deepEqual(outputOf(linesOf, 'turnlog').slice(already), ['finish | stopped | 0']);
  });

  test(`dice (${language}): a wager is collected at the start and goes to the winner`, async (t) => {
    const { host, api, linesOf, first, second } = await seatPlayers(t, 'wager', WAGER, language);
    api.Inventory.add(first, { item: 'money', amount: 100 });
    api.Inventory.add(second, { item: 'money', amount: 100 });
    host.playerInventories.flush();
    const game = api.Dice.start(first.id, second.id);
    await untilLogged({ linesOf }, 'wager', (line) => line.startsWith('collected'));
    assert.deepEqual(outputOf(linesOf, 'wager'), ['collected 100']);
    host.dice.clientMove(first.id, game, 'gaveUp', 0);
    host.playerInventories.flush();
    const purse = (player) =>
      api.Inventory.get(player)
        .items.filter((row) => row.name === 'money')
        .reduce((total, row) => total + row.amount, 0);
    assert.deepEqual([purse(first), purse(second)], [50, 150]);
  });
}
