'use strict';

// Inventory scripts as a resource would write them: keeping items across a reconnect, paying and
// trading, losing items on death, custom items, consumables, and item use on the client. Each
// scenario runs with its scripts in JavaScript, in Lua and in C#.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  LANGUAGES,
  clientFixture,
  delay,
  serverResource,
  serverFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

function outputOf(linesOf, name) {
  return linesOf(name).filter((line) => line !== 'started');
}

const rowSummary = ({ id, name, amount, quality }) => [id, name, amount, quality];

const LOCKER = {
  javascript: `
const locker = new Map();

function toSnapshot(rows) {
  return rows.map((row) => ({
    id: row.id,
    item: row.item,
    amount: row.amount,
    metadata: row.metadata,
    equipped: row.equipped,
  }));
}

kcds.core.Events.on("playerConnect", (visitor) => {
  const stored = locker.get(visitor.nickname);
  if (!stored) return;
  const outcome = kcds.players.Inventory.set(visitor, stored);
  console.log(outcome.ok ? "returned to " + visitor.nickname : "cannot return: " + outcome.code);
});

kcds.core.Events.on("playerDisconnect", (visitor) => {
  const current = kcds.players.Inventory.get(visitor);
  if (!current) return;
  locker.set(visitor.nickname, {
    items: toSnapshot(current.items),
    quickslots: current.quickslots,
  });
  console.log("stored for " + visitor.nickname + ": " + current.items.length);
});
`,
  lua: `
local locker = {}

local function toSnapshot(rows)
  local snapshot = {}
  for _, row in ipairs(rows) do
    table.insert(snapshot, {
      id = row.id,
      item = row.item,
      amount = row.amount,
      metadata = row.metadata,
      equipped = row.equipped,
    })
  end
  return snapshot
end

kcds.core.Events.on("playerConnect", function(visitor)
  local stored = locker[visitor.nickname]
  if not stored then
    return
  end
  local outcome = kcds.players.Inventory.set(visitor, stored)
  print(outcome.ok and "returned to " .. visitor.nickname or "cannot return: " .. outcome.code)
end)

kcds.core.Events.on("playerDisconnect", function(visitor)
  local current = kcds.players.Inventory.get(visitor)
  if not current then
    return
  end
  locker[visitor.nickname] = {
    items = toSnapshot(current.items),
    quickslots = current.quickslots,
  }
  print("stored for " .. visitor.nickname .. ": " .. #current.items)
end)
`,
  csharp: `
public class Locker : Script
{
    private readonly Dictionary<string, object> locker = new();

    private static List<object> ToSnapshot(JsValue rows)
    {
        return rows
            .Select((row) => (object)new
            {
                id = row["id"],
                item = row["item"],
                amount = row["amount"],
                metadata = row["metadata"],
                equipped = row["equipped"],
            })
            .ToList();
    }

    public override void OnStart()
    {
        Events.On("playerConnect", (Player visitor) =>
        {
            if (!locker.TryGetValue(visitor.Nickname, out var stored))
            {
                return;
            }
            var outcome = Inventory.Set(visitor, stored);
            Log.Info(outcome["ok"] ? "returned to " + visitor.Nickname : "cannot return: " + outcome["code"]);
        });
        Events.On("playerDisconnect", (Player visitor) =>
        {
            var current = Inventory.Get(visitor);
            if (current.IsNull)
            {
                return;
            }
            locker[visitor.Nickname] = new
            {
                items = ToSnapshot(current["items"]),
                quickslots = current["quickslots"],
            };
            Log.Info("stored for " + visitor.Nickname + ": " + current["items"].Length);
        });
    }
}
`,
};

const MARKET = {
  javascript: `
kcds.core.Events.on("playerInventoryChanged", (owner, update) => {
  for (const entry of update.items) {
    const was = entry.before ? entry.before.amount : 0;
    const is = entry.after ? entry.after.amount : 0;
    console.log([owner.nickname, update.reason, was, is].join(" / "));
  }
});

kcds.core.Events.on("market:run", (buyerId, sellerId) => {
  const buyer = kcds.players.Player.getById(buyerId);
  const seller = kcds.players.Player.getById(sellerId);
  const view = kcds.players.Inventory.get(buyer);
  for (const entry of view.items) {
    console.log("has " + entry.name + " x" + entry.amount + " q" + entry.quality);
  }

  // A second removal at the same revision must be refused.
  const purse = view.items.find((entry) => entry.name === "money");
  const fee = kcds.players.Inventory.remove(buyer, {
    units: [{ id: purse.id, amount: 50 }],
    revision: view.revision,
  });
  console.log("fee " + fee.ok);
  const repeat = kcds.players.Inventory.remove(buyer, {
    units: [{ id: purse.id, amount: 1 }],
    revision: view.revision,
  });
  console.log("repeat " + repeat.code);

  const latest = kcds.players.Inventory.get(buyer);
  const blade = latest.items.find((entry) => entry.equipped > 0);
  const { health, ...kept } = blade.metadata;
  const repair = kcds.players.Inventory.editRow(buyer, {
    id: blade.id,
    metadata: { ...kept, condition: 0.75 },
    revision: latest.revision,
  });
  console.log("repair " + repair.ok);

  const fruit = kcds.players.Inventory.get(buyer).items.find((entry) => entry.name === "apple");
  const gift = kcds.players.Inventory.transfer(buyer, seller, {
    units: [{ id: fruit.id, amount: 1 }],
  });
  console.log("gift row " + gift.items[0].id);
  const missing = kcds.players.Inventory.transfer(buyer, seller, {
    units: [{ id: "no-such-row", amount: 1 }],
  });
  console.log("missing " + missing.code);
});
`,
  lua: `
local Inventory = kcds.players.Inventory

local function find(rows, matches)
  for _, row in ipairs(rows) do
    if matches(row) then
      return row
    end
  end
  return nil
end

kcds.core.Events.on("playerInventoryChanged", function(owner, update)
  for _, entry in ipairs(update.items) do
    local was = entry.before and entry.before.amount or 0
    local is = entry.after and entry.after.amount or 0
    print(table.concat({ owner.nickname, update.reason, was, is }, " / "))
  end
end)

kcds.core.Events.on("market:run", function(buyerId, sellerId)
  local buyer = kcds.players.Player.getById(buyerId)
  local seller = kcds.players.Player.getById(sellerId)
  local view = Inventory.get(buyer)
  for _, entry in ipairs(view.items) do
    print("has " .. entry.name .. " x" .. entry.amount .. " q" .. entry.quality)
  end

  -- A second removal at the same revision must be refused.
  local purse = find(view.items, function(entry) return entry.name == "money" end)
  local fee = Inventory.remove(buyer, {
    units = { { id = purse.id, amount = 50 } },
    revision = view.revision,
  })
  print("fee " .. tostring(fee.ok))
  local again = Inventory.remove(buyer, {
    units = { { id = purse.id, amount = 1 } },
    revision = view.revision,
  })
  print("repeat " .. again.code)

  local latest = Inventory.get(buyer)
  local blade = find(latest.items, function(entry) return entry.equipped > 0 end)
  local kept = {}
  for key, value in pairs(blade.metadata) do
    if key ~= "health" then
      kept[key] = value
    end
  end
  kept.condition = 0.75
  local repair = Inventory.editRow(buyer, { id = blade.id, metadata = kept, revision = latest.revision })
  print("repair " .. tostring(repair.ok))

  local fruit = find(Inventory.get(buyer).items, function(entry) return entry.name == "apple" end)
  local gift = Inventory.transfer(buyer, seller, { units = { { id = fruit.id, amount = 1 } } })
  print("gift row " .. gift.items[1].id)
  local missing = Inventory.transfer(buyer, seller, { units = { { id = "no-such-row", amount = 1 } } })
  print("missing " .. missing.code)
end)
`,
  csharp: `
public class Market : Script
{
    public override void OnStart()
    {
        Events.On("playerInventoryChanged", (Player owner, JsValue update) =>
        {
            foreach (var entry in update["items"])
            {
                var was = entry["before"].IsNull ? 0 : entry["before"]["amount"].AsNumber();
                var now = entry["after"].IsNull ? 0 : entry["after"]["amount"].AsNumber();
                Log.Info(string.Join(" / ", owner.Nickname, update["reason"], was, now));
            }
        });
        Events.On("market:run", (double buyerId, double sellerId) =>
        {
            var buyer = Player.GetById(buyerId)!;
            var seller = Player.GetById(sellerId)!;
            var view = Inventory.Get(buyer);
            foreach (var entry in view["items"])
            {
                Log.Info("has " + entry["name"] + " x" + entry["amount"] + " q" + entry["quality"]);
            }

            // A second removal at the same revision must be refused.
            var purse = view["items"].First((entry) => entry["name"].AsString() == "money");
            var fee = Inventory.Remove(buyer, new
            {
                units = new[] { new { id = purse["id"], amount = 50 } },
                revision = view["revision"],
            });
            Log.Info("fee " + fee["ok"]);
            var again = Inventory.Remove(buyer, new
            {
                units = new[] { new { id = purse["id"], amount = 1 } },
                revision = view["revision"],
            });
            Log.Info("repeat " + again["code"]);

            var latest = Inventory.Get(buyer);
            var blade = latest["items"].First((entry) => entry["equipped"].AsNumber() > 0);
            var kept = new Dictionary<string, object?>();
            foreach (var key in blade["metadata"].Keys)
            {
                if (key != "health")
                {
                    kept[key] = blade["metadata"][key];
                }
            }
            kept["condition"] = 0.75;
            var repair = Inventory.EditRow(buyer, new
            {
                id = blade["id"],
                metadata = kept,
                revision = latest["revision"],
            });
            Log.Info("repair " + repair["ok"]);

            var fruit = Inventory.Get(buyer)["items"].First((entry) => entry["name"].AsString() == "apple");
            var gift = Inventory.Transfer(buyer, seller, new
            {
                units = new[] { new { id = fruit["id"], amount = 1 } },
            });
            Log.Info("gift row " + gift["items"][0]["id"]);
            var missing = Inventory.Transfer(buyer, seller, new
            {
                units = new[] { new { id = "no-such-row", amount = 1 } },
            });
            Log.Info("missing " + missing["code"]);
        });
    }
}
`,
};

const GRAVEYARD = {
  javascript: `
kcds.core.Events.on("playerDied", (fallen) => {
  const remains = fallen.dropAll({ keepEquipped: true });
  if (remains) console.log("remains of " + fallen.nickname + " in " + remains.id);
});
`,
  lua: `
kcds.core.Events.on("playerDied", function(fallen)
  local remains = fallen:dropAll({ keepEquipped = true })
  if remains then
    print("remains of " .. fallen.nickname .. " in " .. remains.id)
  end
end)
`,
  csharp: `
public class Graveyard : Script
{
    public override void OnStart()
    {
        Events.On("playerDied", (Player fallen) =>
        {
            var remains = fallen.DropAll(new { keepEquipped = true });
            if (remains != null)
            {
                Log.Info("remains of " + fallen.Nickname + " in " + remains.Id);
            }
        });
    }
}
`,
};

const GUILD = {
  javascript: `
const brick = kcds.players.Items.register("guild:brick", {
  name: "Guild brick",
  description: "Building material with the guild's stamp.",
  visual: "bsmt_woodExotic",
  weight: 2,
  price: 12,
});
const permit = kcds.players.Items.register("guild:permit", {
  name: "Permit",
  visual: "a54ab5ef-d4a2-4929-9045-1a1efde935c5",
  weight: 0.1,
  price: 1,
  stackable: false,
  usable: true,
  useLabel: "Inspect",
});

kcds.core.Events.on("playerCustomItemUse", (holder, entry, revision) => {
  if (entry.name === "guild:permit") {
    console.log("inspected " + entry.id + " @" + revision + " by " + holder.nickname);
  }
});

kcds.core.Events.on("guild:hire", (id) => {
  const worker = kcds.players.Player.getById(id);
  const bricks = kcds.players.Inventory.add(worker, { item: brick, amount: 1 });
  if (!bricks.ok) console.log("no brick: " + bricks.code);
  const issued = kcds.players.Inventory.add(worker, {
    item: permit,
    amount: 1,
    metadata: {
      custom: {
        name: "Mason's permit",
        description: "Lets the holder work on the town walls.",
        data: { permitNumber: 17 },
      },
    },
  });
  console.log("issued " + issued.ok);
});
`,
  lua: `
local brick = kcds.players.Items.register("guild:brick", {
  name = "Guild brick",
  description = "Building material with the guild's stamp.",
  visual = "bsmt_woodExotic",
  weight = 2,
  price = 12,
})
local permit = kcds.players.Items.register("guild:permit", {
  name = "Permit",
  visual = "a54ab5ef-d4a2-4929-9045-1a1efde935c5",
  weight = 0.1,
  price = 1,
  stackable = false,
  usable = true,
  useLabel = "Inspect",
})

kcds.core.Events.on("playerCustomItemUse", function(holder, entry, revision)
  if entry.name == "guild:permit" then
    print("inspected " .. entry.id .. " @" .. revision .. " by " .. holder.nickname)
  end
end)

kcds.core.Events.on("guild:hire", function(id)
  local worker = kcds.players.Player.getById(id)
  local bricks = kcds.players.Inventory.add(worker, { item = brick, amount = 1 })
  if not bricks.ok then
    print("no brick: " .. bricks.code)
  end
  local issued = kcds.players.Inventory.add(worker, {
    item = permit,
    amount = 1,
    metadata = {
      custom = {
        name = "Mason's permit",
        description = "Lets the holder work on the town walls.",
        data = { permitNumber = 17 },
      },
    },
  })
  print("issued " .. tostring(issued.ok))
end)
`,
  csharp: `
public class Guild : Script
{
    public override void OnStart()
    {
        var brick = Items.Register("guild:brick", new
        {
            name = "Guild brick",
            description = "Building material with the guild's stamp.",
            visual = "bsmt_woodExotic",
            weight = 2,
            price = 12,
        });
        var permit = Items.Register("guild:permit", new
        {
            name = "Permit",
            visual = "a54ab5ef-d4a2-4929-9045-1a1efde935c5",
            weight = 0.1,
            price = 1,
            stackable = false,
            usable = true,
            useLabel = "Inspect",
        });

        Events.On("playerCustomItemUse", (Player holder, JsValue entry, double revision) =>
        {
            if (entry["name"].AsString() == "guild:permit")
            {
                Log.Info("inspected " + entry["id"] + " @" + revision + " by " + holder.Nickname);
            }
        });

        Events.On("guild:hire", (double id) =>
        {
            var worker = Player.GetById(id)!;
            var bricks = Inventory.Add(worker, new { item = brick, amount = 1 });
            if (!bricks["ok"])
            {
                Log.Info("no brick: " + bricks["code"]);
            }
            var issued = Inventory.Add(worker, new
            {
                item = permit,
                amount = 1,
                metadata = new
                {
                    custom = new
                    {
                        name = "Mason's permit",
                        description = "Lets the holder work on the town walls.",
                        data = new { permitNumber = 17 },
                    },
                },
            });
            Log.Info("issued " + issued["ok"]);
        });
    }
}
`,
};

const REMEDY = {
  javascript: `
const TONIC = "b38c34b7-6016-4f64-9ba2-65e1ce31d4a1";
const TONIC_HEALING = 15;

kcds.core.Events.on("playerConsumptionEffects", (_drinker, drink) => {
  return drink.item === TONIC ? false : undefined;
});

kcds.core.Events.on("playerConsumed", (drinker, drink) => {
  if (drink.item !== TONIC || drink.nativeEffects) return;
  const healed = drinker.health + TONIC_HEALING;
  drinker.setHealth(healed > drinker.maxHealth ? drinker.maxHealth : healed);
});

kcds.core.Events.on("playerStatsChanged", (subject, updates) => {
  const poison = updates.find((update) => update.stat === "poisoning");
  if (poison) console.log("poison " + poison.previous + " to " + poison.current);
});
`,
  lua: `
local TONIC = "b38c34b7-6016-4f64-9ba2-65e1ce31d4a1"
local TONIC_HEALING = 15

kcds.core.Events.on("playerConsumptionEffects", function(_drinker, drink)
  if drink.item == TONIC then
    return false
  end
end)

kcds.core.Events.on("playerConsumed", function(drinker, drink)
  if drink.item ~= TONIC or drink.nativeEffects then
    return
  end
  local healed = drinker.health + TONIC_HEALING
  drinker:setHealth(math.min(healed, drinker.maxHealth))
end)

kcds.core.Events.on("playerStatsChanged", function(subject, updates)
  for _, update in ipairs(updates) do
    if update.stat == "poisoning" then
      print("poison " .. update.previous .. " to " .. update.current)
      return
    end
  end
end)
`,
  // A handler answers false to take the game's effects away, null to leave them.
  csharp: `
public class Remedy : Script
{
    private const string Tonic = "b38c34b7-6016-4f64-9ba2-65e1ce31d4a1";
    private const double TonicHealing = 15;

    public override void OnStart()
    {
        Events.On("playerConsumptionEffects", (Player drinker, JsValue drink) =>
        {
            return drink["item"].AsString() == Tonic ? false : (bool?)null;
        });
        Events.On("playerConsumed", (Player drinker, JsValue drink) =>
        {
            if (drink["item"].AsString() != Tonic || drink["nativeEffects"])
            {
                return;
            }
            drinker.SetHealth(Math.Min(drinker.Health + TonicHealing, drinker.MaxHealth));
        });
        Events.On("playerStatsChanged", (Player subject, JsValue updates) =>
        {
            foreach (var update in updates)
            {
                if (update["stat"].AsString() == "poisoning")
                {
                    Log.Info("poison " + update["previous"] + " to " + update["current"]);
                    return;
                }
            }
        });
    }
}
`,
};

const PANTRY = {
  javascript: `
const watcher = kcds.players.Inventory.whenUsed("apple", (action) => {
  kcds.ui.Hud.showNotification("Using row " + action.id);
  const request = { id: action.id, revision: action.revision };
  kcds.core.Events.toServer("pantry:use", JSON.stringify(request));
});
kcds.core.Events.on("pantry:close", () => {
  console.log("removed " + kcds.players.Inventory.stopWhenUsed(watcher));
});
`,
  lua: `
local watcher = kcds.players.Inventory.whenUsed("apple", function(action)
  kcds.ui.Hud.showNotification("Using row " .. action.id)
  local request = { id = action.id, revision = action.revision }
  kcds.core.Events.toServer("pantry:use", json.encode(request))
end)
kcds.core.Events.on("pantry:close", function()
  print("removed " .. tostring(kcds.players.Inventory.stopWhenUsed(watcher)))
end)
`,
  csharp: `
public class Pantry : Script
{
    public override void OnStart()
    {
        var watcher = Inventory.WhenUsed("apple", (JsValue action) =>
        {
            Hud.ShowNotification("Using row " + action["id"]);
            var request = new { id = action["id"].AsString(), revision = action["revision"].AsNumber() };
            Events.ToServer("pantry:use", System.Text.Json.JsonSerializer.Serialize(request));
        });
        Events.On("pantry:close", () =>
        {
            Log.Info("removed " + (Inventory.StopWhenUsed(watcher) ? "true" : "false"));
        });
    }
}
`,
};

function characterSheet() {
  const vitals = {
    health: 50,
    stamina: 100,
    exhaust: 100,
    hunger: 100,
    bleeding: 0,
    sleeping: 0,
    consciousness: 1,
    drunkenness: 0,
    poisoning: 0,
  };
  const skills = {
    fencing: 0,
    survival: 0,
    defense: 0,
    sword: 0,
    heavyWeapons: 0,
    marksmanship: 0,
    dagger: 0,
    largeWeapons: 0,
    unarmed: 0,
  };
  const abilities = {
    maxHealth: 100,
    maxStamina: 100,
    staminaCap: 100,
    maxExhaust: 100,
    maxHunger: 100,
    level: 1,
    strength: 1,
    agility: 1,
    vitality: 1,
    skills,
    scaledSkills: skills,
  };
  return { vitals, abilities };
}

for (const language of LANGUAGES) {
  test(`inventory (${language}): items stored at disconnect return at the next connect`, async (t) => {
    const { root, host, report, linesOf } = serverFixture(t);
    serverResource(root, 'locker', LOCKER, {}, language);
    await host.startAll();
    const api = host.makeAPI();
    const player = report(7);
    api.Inventory.add(player, {
      item: 'longswordBroad',
      amount: 1,
      metadata: { quality: 3, condition: 0.5 },
    });
    api.Inventory.add(player, { item: 'apple', amount: 5 });
    host.playerInventories.flush();
    const before = api.Inventory.get(player).items.map(rowSummary);
    host.disconnect(7);
    const returning = report(7);
    host.playerInventories.flush();
    assert.deepEqual(api.Inventory.get(returning).items.map(rowSummary), before);
    assert.deepEqual(outputOf(linesOf, 'locker'), [
      'stored for Player 7: 2',
      'returned to Player 7',
    ]);
  });

  test(`inventory (${language}): reads, revisions, property changes and transfers`, async (t) => {
    const { root, host, report, linesOf } = serverFixture(t);
    serverResource(root, 'market', MARKET, {}, language);
    await host.startAll();
    const api = host.makeAPI();
    const buyer = report(7);
    const seller = report(8);
    api.Inventory.set(buyer, {
      items: [
        { item: 'longswordBroad', amount: 1, equipped: 1 },
        { item: 'apple', amount: 3 },
        { item: 'money', amount: 120 },
      ],
    });
    host.playerInventories.flush();
    host.emit('market:run', 7, 8);
    host.playerInventories.flush();
    const lines = linesOf('market');
    assert.ok(
      lines.some((line) => /^has longswordBroad x1 q\d$/.test(line)),
      lines.join('\n'),
    );
    assert.ok(lines.includes('has apple x3 q1'));
    assert.ok(lines.includes('fee true'));
    assert.ok(lines.includes('repeat staleRevision'));
    assert.ok(lines.includes('repair true'));
    assert.ok(lines.some((line) => /^gift row /.test(line)));
    assert.ok(lines.includes('missing unknownItem'));
    assert.ok(lines.includes('Player 7 / transfer / 3 / 2'));
    assert.ok(lines.includes('Player 8 / transfer / 0 / 1'));
    assert.deepEqual(
      api.Inventory.get(seller).items.map((row) => [row.name, row.amount]),
      [['apple', 1]],
    );
  });

  test(`inventory (${language}): on death a script can leave unworn items in a container`, async (t) => {
    const { root, host, report, linesOf } = serverFixture(t);
    serverResource(root, 'graveyard', GRAVEYARD, {}, language);
    await host.startAll();
    const api = host.makeAPI();
    const player = report(7);
    api.Inventory.set(player, {
      items: [
        { item: 'longswordBroad', amount: 1, equipped: 1 },
        { item: 'apple', amount: 3 },
      ],
    });
    host.playerDied(7);
    host.playerInventories.flush();
    const [line] = outputOf(linesOf, 'graveyard');
    assert.match(line, /^remains of Player 7 in \d+$/);
    assert.deepEqual(
      api.Inventory.get(player).items.map((row) => row.name),
      ['longswordBroad'],
    );
  });

  test(`custom items (${language}): registering, giving with custom data, and handling use`, async (t) => {
    const { root, host, report, linesOf } = serverFixture(t);
    serverResource(root, 'guild', GUILD, {}, language);
    await host.startAll();
    const player = report(7);
    host.emit('guild:hire', 7);
    host.playerInventories.flush();
    const state = host.makeAPI().Inventory.get(player);
    const permitRow = state.items.find((row) => row.name === 'guild:permit');
    assert.deepEqual(
      state.items.map((row) => [row.name, row.amount]),
      [
        ['guild:brick', 1],
        ['guild:permit', 1],
      ],
    );
    assert.equal(permitRow.metadata.custom.name, "Mason's permit");
    assert.equal(host.customItems.use(player, permitRow.id, state.revision), true);
    assert.deepEqual(outputOf(linesOf, 'guild'), [
      'issued true',
      `inspected ${permitRow.id} @${state.revision} by Player 7`,
    ]);
  });

  test(`consumables (${language}): a script replaces the game effects of a drink`, async (t) => {
    const { root, host, sent, report, linesOf } = serverFixture(t);
    serverResource(root, 'remedy', REMEDY, {}, language);
    await host.startAll();
    const { vitals, abilities } = characterSheet();
    const player = report(7, { vitals, abilities });
    host.makeAPI().Inventory.set(player, {
      items: [{ id: 'bottle', item: 'b38c34b7-6016-4f64-9ba2-65e1ce31d4a1', amount: 2 }],
    });
    host.playerInventories.consume(player, { request: 1, row: 'bottle' });
    assert.deepEqual(
      sent.filter((event) => event.op === 'inventoryConsumeReply').map((event) => event.verdict),
      ['plain'],
    );
    host.playerInventories.consumed(player, { request: 1, used: true });
    host.playerInventories.flush();
    const change = sent.filter((event) => event.op === 'setStat').at(-1);
    assert.ok(change, 'the script set health');
    assert.equal(change.value, 65);
    report(7, { vitals: { ...vitals, health: 65, poisoning: 0.4 }, abilities });
    assert.deepEqual(outputOf(linesOf, 'remedy'), ['poison 0 to 0.4']);
  });

  test(`item use (${language}): a client handler takes the use, asks the server, and can be removed`, async (t) => {
    const f = clientFixture(t);
    await f.runtime.apply(
      { resources: [f.resource('pantry', PANTRY, {}, 'pantry', language)] },
      f.read,
    );
    await delay(20);
    f.runtime.hud.receiveHost({ ready: true });
    const overrides = f.sent.filter((message) => message.op === 'inventoryUseOverrides').at(-1);
    assert.equal(overrides.classes.length, 1);
    const action = {
      item: overrides.classes[0],
      uid: '144115188075855872',
      id: 'row-1',
      revision: 3,
      amount: 1,
      action: 'secondary',
    };
    f.runtime.inventory.receiveUse(action);
    await delay(20);
    assert.deepEqual(
      f.sent.filter((message) => message.op === 'hud').map((message) => message.args),
      [['Using row row-1']],
    );
    const requests = f.sent.filter((message) => message.op === 'event');
    assert.deepEqual(
      requests.map((message) => message.name),
      ['pantry:use'],
    );
    assert.deepEqual(JSON.parse(requests[0].payload), { id: 'row-1', revision: 3 });
    await f.runtime.services.emit('shared', 'pantry:close', []);
    assert.deepEqual(f.linesOf('pantry').at(-1), 'removed true');
    assert.deepEqual(
      f.sent.filter((message) => message.op === 'inventoryUseOverrides').at(-1).classes,
      [],
    );
    f.runtime.inventory.receiveUse(action);
    await delay(20);
    assert.equal(f.sent.filter((message) => message.op === 'event').length, 1);
  });
}
