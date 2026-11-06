'use strict';

// Gathering scripts as a resource would write them, on the real ResourceHost. A harvest here is
// the report a player's client sends when the picking animation ends.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  LANGUAGES,
  shown,
  serverResource,
  serverFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

// The same scripts in Lua; each scenario runs with both.
const LUA = {
  SLOW_GROWTH: `
kcds.world.Gathering.setRegrowth(1, 3)

kcds.core.Events.on("gatheringHarvested", function(gatherer, result)
  print(gatherer.nickname .. " got " .. result.amount .. " x " .. result.item)
end)
`,
  GUARDED_FIELD: `
local PROTECTED_DIMENSION = 0

kcds.core.Events.on("gatheringHarvest", function(gatherer, attempt)
  local licensed = gatherer.nickname:sub(1, #"[Licensed]") == "[Licensed]"
  if attempt.dimension ~= PROTECTED_DIMENSION or licensed then
    return true
  end
  kcds.core.Chat.sendToPlayer(gatherer, "You need a licence for this field.")
  return false
end)
`,
  LEDGER: `
local totals = {}

kcds.core.Events.on("gatheringHarvested", function(gatherer, result)
  totals[gatherer.id] = (totals[gatherer.id] or 0) + result.amount
end)
kcds.core.Events.on("playerCommand", function(asker, command)
  if command == "tally" then
    print("total " .. totals[asker.id])
  end
end)
`,
  REAL_TIME_GROWTH: `
local REAL_SECONDS = 90
local gameHours = (REAL_SECONDS * kcds.world.World.timeScale) / 3600
kcds.world.Gathering.setRegrowth(gameHours, gameHours)
print(json.encode(kcds.world.Gathering.regrowTime))
kcds.world.Gathering.resetRegrowth()
print("reset " .. tostring(kcds.world.Gathering.regrowTime))
print("reversed " .. tostring(kcds.world.Gathering.setRegrowth(3, 1)))
`,
  REPLANT: `
kcds.core.Events.on("playerCommand", function(gardener, command)
  if command ~= "replant" then
    return
  end
  local here = kcds.world.Gathering.respawn(gardener.position, 20, gardener.dimension)
  local elsewhere = kcds.world.Gathering.regrowEverywhere(gardener.dimension)
  print("near " .. here .. ", rest " .. elsewhere)
end)
`,
};

// And in C#.
const CSHARP = {
  SLOW_GROWTH: `
public class SlowGrowth : Script
{
    public override void OnStart()
    {
        Gathering.SetRegrowth(1, 3);
        Events.On("gatheringHarvested", (Player gatherer, JsValue result) =>
        {
            Log.Info(gatherer.Nickname + " got " + result["amount"] + " x " + result["item"]);
        });
    }
}
`,
  GUARDED_FIELD: `
public class GuardedField : Script
{
    private const double ProtectedDimension = 0;

    public override void OnStart()
    {
        Events.On("gatheringHarvest", (Player gatherer, JsValue attempt) =>
        {
            var licensed = gatherer.Nickname.StartsWith("[Licensed]");
            if (attempt["dimension"].AsNumber() != ProtectedDimension || licensed)
            {
                return true;
            }
            Chat.SendToPlayer(gatherer, "You need a licence for this field.");
            return false;
        });
    }
}
`,
  LEDGER: `
public class Totals : Script
{
    private readonly Dictionary<double, double> totals = new();

    public override void OnStart()
    {
        Events.On("gatheringHarvested", (Player gatherer, JsValue result) =>
        {
            totals[gatherer.Id] = totals.GetValueOrDefault(gatherer.Id) + result["amount"].AsNumber();
        });
        Events.On("playerCommand", (Player asker, string command) =>
        {
            if (command == "tally")
            {
                Log.Info("total " + totals[asker.Id]);
            }
        });
    }
}
`,
  REAL_TIME_GROWTH: `
public class RealTimeGrowth : Script
{
    private const double RealSeconds = 90;

    public override void OnStart()
    {
        var gameHours = RealSeconds * World.TimeScale / 3600;
        Gathering.SetRegrowth(gameHours, gameHours);
        Log.Info(Gathering.RegrowTime.ToJson());
        Gathering.ResetRegrowth();
        Log.Info("reset " + Gathering.RegrowTime);
        Log.Info("reversed " + (Gathering.SetRegrowth(3, 1) ? "true" : "false"));
    }
}
`,
  REPLANT: `
public class Replant : Script
{
    public override void OnStart()
    {
        Events.On("playerCommand", (Player gardener, string command) =>
        {
            if (command != "replant")
            {
                return;
            }
            var here = Gathering.Respawn(gardener.Position, 20, gardener.Dimension);
            var elsewhere = Gathering.RegrowEverywhere(gardener.Dimension);
            Log.Info("near " + here + ", rest " + elsewhere);
        });
    }
}
`,
};
const { KINDS } = require('./gathering/gathering.cjs');

const HERB = KINDS.get(12);
const FIELD = { x: 2354.8, y: 1975.3, z: 98.7 };

function shifted(dx) {
  return { x: FIELD.x + dx, y: FIELD.y, z: FIELD.z };
}

async function startWithPlayer(t, name, code, language) {
  const fixture = serverFixture(t);
  serverResource(fixture.root, name, code, {}, language);
  await fixture.host.startAll();
  const player = fixture.report(7, { position: shifted(1) });
  return { ...fixture, api: fixture.host.makeAPI(), player };
}

function harvestReport(request, position = FIELD) {
  return {
    request,
    item: HERB.item,
    amount: 2,
    kind: HERB.id,
    position: [position.x, position.y, position.z],
    radius: 1.5,
    plants: 1,
  };
}

function outputOf(linesOf, name) {
  return linesOf(name).filter((line) => line !== 'started');
}

function regrowing(host, world) {
  return [...host.gathering.picksOf(world).values()];
}

const SLOW_GROWTH = `
kcds.world.Gathering.setRegrowth(1, 3);

kcds.core.Events.on("gatheringHarvested", (gatherer, result) => {
  console.log(gatherer.nickname + " got " + result.amount + " x " + result.item);
});
`;

for (const language of LANGUAGES) {
  test(`gathering (${language}): a harvest fires an event and the plant regrows in the set time`, async (t) => {
    const code = { javascript: SLOW_GROWTH, lua: LUA.SLOW_GROWTH, csharp: CSHARP.SLOW_GROWTH };
    const { host, linesOf, player } = await startWithPlayer(t, 'garden', code, language);
    assert.equal(host.gathering.report(player, harvestReport(1)), true);
    assert.deepEqual(outputOf(linesOf, 'garden'), [`Player 7 got 2 x ${HERB.item}`]);
    const [plant] = regrowing(host, player.dimension);
    const hours = (plant.due - host.worldState.clock.seconds()) / 3600;
    assert.ok(hours >= 1 && hours <= 3);
  });
}

const GUARDED_FIELD = `
const PROTECTED_DIMENSION = 0;

kcds.core.Events.on("gatheringHarvest", (gatherer, attempt) => {
  const licensed = gatherer.nickname.startsWith("[Licensed]");
  if (attempt.dimension !== PROTECTED_DIMENSION || licensed) return true;
  kcds.core.Chat.sendToPlayer(gatherer, "You need a licence for this field.");
  return false;
});
`;

for (const language of LANGUAGES) {
  test(`gathering (${language}): a script can refuse a harvest in one dimension`, async (t) => {
    const code = {
      javascript: GUARDED_FIELD,
      lua: LUA.GUARDED_FIELD,
      csharp: CSHARP.GUARDED_FIELD,
    };
    const { host, api, sent, player } = await startWithPlayer(t, 'garden', code, language);
    assert.equal(host.gathering.report(player, harvestReport(1)), true);
    host.gathering.lastPick.clear();
    api.Player.getById(player.id).setDimension(0);
    assert.equal(host.gathering.report(player, harvestReport(2, shifted(2))), false);
    const chats = sent.filter((message) => message.op === 'chat').map((message) => message.text);
    assert.deepEqual(chats, ['You need a licence for this field.']);
    assert.equal(regrowing(host, 0).length, 0);
    const verdicts = sent.filter((message) => message.op === 'gatherVerdict');
    assert.deepEqual(
      verdicts.map((verdict) => verdict.granted),
      [true, false],
    );
  });
}

const LEDGER = `
const totals = {};

kcds.core.Events.on("gatheringHarvested", (gatherer, result) => {
  totals[gatherer.id] = (totals[gatherer.id] || 0) + result.amount;
});
kcds.core.Events.on("playerCommand", (asker, command) => {
  if (command === "tally") console.log("total " + totals[asker.id]);
});
`;

for (const language of LANGUAGES) {
  test(`gathering (${language}): a script can keep a running total`, async (t) => {
    const code = { javascript: LEDGER, lua: LUA.LEDGER, csharp: CSHARP.LEDGER };
    const { host, linesOf, player } = await startWithPlayer(t, 'garden', code, language);
    assert.equal(host.gathering.report(player, harvestReport(1)), true);
    host.gathering.lastPick.clear();
    assert.equal(host.gathering.report(player, harvestReport(2, shifted(2))), true);
    host.emit('playerCommand', player, 'tally', []);
    assert.deepEqual(outputOf(linesOf, 'garden'), ['total 4']);
  });
}

const REAL_TIME_GROWTH = `
const REAL_SECONDS = 90;
const gameHours = (REAL_SECONDS * kcds.world.World.timeScale) / 3600;
kcds.world.Gathering.setRegrowth(gameHours, gameHours);
console.log(JSON.stringify(kcds.world.Gathering.regrowTime));
kcds.world.Gathering.resetRegrowth();
console.log("reset " + kcds.world.Gathering.regrowTime);
console.log("reversed " + kcds.world.Gathering.setRegrowth(3, 1));
`;

for (const language of LANGUAGES) {
  test(`gathering (${language}): regrowth from real seconds, reset, and a refused range`, async (t) => {
    const code = {
      javascript: REAL_TIME_GROWTH,
      lua: LUA.REAL_TIME_GROWTH,
      csharp: CSHARP.REAL_TIME_GROWTH,
    };
    const { api, linesOf } = await startWithPlayer(t, 'garden', code, language);
    const hours = (90 * api.World.timeScale) / 3600;
    assert.deepEqual(outputOf(linesOf, 'garden'), [
      JSON.stringify({ minHours: hours, maxHours: hours }),
      `reset ${shown(language, 'null')}`,
      'reversed false',
    ]);
  });
}

const REPLANT = `
kcds.core.Events.on("playerCommand", (gardener, command) => {
  if (command !== "replant") return;
  const here = kcds.world.Gathering.respawn(gardener.position, 20, gardener.dimension);
  const elsewhere = kcds.world.Gathering.regrowEverywhere(gardener.dimension);
  console.log("near " + here + ", rest " + elsewhere);
});
`;

for (const language of LANGUAGES) {
  test(`gathering (${language}): plants regrow near a position, then in the whole dimension`, async (t) => {
    const code = { javascript: REPLANT, lua: LUA.REPLANT, csharp: CSHARP.REPLANT };
    const { host, sent, linesOf, player } = await startWithPlayer(t, 'garden', code, language);
    assert.equal(host.gathering.report(player, harvestReport(1)), true);
    const distant = shifted(100);
    host.gathering.lastPick.clear();
    player.position = { x: distant.x + 1, y: distant.y, z: distant.z };
    assert.equal(host.gathering.report(player, harvestReport(2, distant)), true);
    player.position = shifted(1);
    sent.length = 0;
    host.emit('playerCommand', player, 'replant', []);
    assert.deepEqual(outputOf(linesOf, 'garden'), ['near 1, rest 1']);
    const grown = sent.filter((message) => message.op === 'gatherPlants');
    assert.deepEqual(
      grown.map((message) => message.grown),
      [[1], [2]],
    );
    assert.equal(regrowing(host, player.dimension).length, 0);
  });
}
