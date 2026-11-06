'use strict';

// Crafting scripts as a resource would write them: cook pot supplies, alchemy knowledge that lasts
// between sessions, and teaching smithing recipes; plus how the server judges a finished piece.
// The scripted scenarios run with their scripts in JavaScript, in Lua and in C#

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  LANGUAGES,
  serverResource,
  serverFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const LUA = {
  SOUP_KITCHEN: `
local kettle = kcds.world.CookPot.all()[1]
if kettle then
  kcds.world.CookPot.refill(kettle.guid, 8)
  print("servings " .. kcds.world.CookPot.get(kettle.guid).portions)
end
`,
  FEAST: `
local kettle = kcds.world.CookPot.all()[1]
local function describe()
  local now = kcds.world.CookPot.get(kettle.guid)
  return now.portions .. "/" .. now.capacity .. "/" .. tostring(now.enabled) .. "/" .. now.state
end
kcds.world.CookPot.setEnabled(kettle.guid, false)
kcds.world.CookPot.refill(kettle.guid, 12)
kcds.world.CookPot.setEnabled(kettle.guid, true)
print(describe())
kcds.world.CookPot.setState(kettle.guid, "half")
print(describe())
`,
  NOTEBOOK: `
local notebooks = {}

kcds.core.Events.on("playerInventoryReady", function(student)
  local learned = notebooks[student.nickname]
  if not learned then
    return
  end
  for name, value in pairs(learned) do
    kcds.world.Crafting.grantKnowledge(student, name, value)
  end
end)

kcds.core.Events.on("playerDisconnect", function(student)
  local learned = kcds.world.Crafting.knowledge(student)
  notebooks[student.nickname] = learned
  print("noted " .. json.encode(learned))
end)
`,
  APPRENTICE: `
kcds.core.Events.on("playerInventoryReady", function(apprentice)
  local lessons = kcds.world.Crafting.recipes(apprentice, "smithing")
  local learned = 0
  for _, lesson in ipairs(lessons) do
    if lesson.perk and apprentice:addPerk(lesson.perk) then
      learned = learned + 1
    end
  end
  for _, lesson in ipairs(lessons) do
    for _, part in ipairs(lesson.ingredients) do
      if not part.quest then
        apprentice:giveItem(part.name, part.amount)
      end
    end
  end
  print("learned " .. learned)
end)
`,
};

// And in C#.
const CSHARP = {
  SOUP_KITCHEN: `
public class SoupKitchen : Script
{
    public override void OnStart()
    {
        var kettle = CookPot.All()[0];
        if (!kettle.IsNull)
        {
            CookPot.Refill(kettle["guid"], 8);
            Log.Info("servings " + CookPot.Get(kettle["guid"])["portions"]);
        }
    }
}
`,
  FEAST: `
public class Feast : Script
{
    public override void OnStart()
    {
        var kettle = CookPot.All()[0];
        string Describe()
        {
            var now = CookPot.Get(kettle["guid"]);
            return now["portions"] + "/" + now["capacity"] + "/" + now["enabled"] + "/" + now["state"];
        }
        CookPot.SetEnabled(kettle["guid"], false);
        CookPot.Refill(kettle["guid"], 12);
        CookPot.SetEnabled(kettle["guid"], true);
        Log.Info(Describe());
        CookPot.SetState(kettle["guid"], "half");
        Log.Info(Describe());
    }
}
`,
  NOTEBOOK: `
public class Notebook : Script
{
    private readonly Dictionary<string, JsValue> notebooks = new();

    public override void OnStart()
    {
        Events.On("playerInventoryReady", (Player student) =>
        {
            if (!notebooks.TryGetValue(student.Nickname, out var learned))
            {
                return;
            }
            foreach (var name in learned.Keys)
            {
                Crafting.GrantKnowledge(student, name, learned[name]);
            }
        });
        Events.On("playerDisconnect", (Player student) =>
        {
            var learned = Crafting.Knowledge(student);
            notebooks[student.Nickname] = learned;
            Log.Info("noted " + learned.ToJson());
        });
    }
}
`,
  APPRENTICE: `
public class Apprentice : Script
{
    public override void OnStart()
    {
        Events.On("playerInventoryReady", (Player apprentice) =>
        {
            var lessons = Crafting.Recipes(apprentice, "smithing");
            var learned = 0;
            foreach (var lesson in lessons)
            {
                var perk = lesson["perk"];
                if (!perk.IsNull && apprentice.AddPerk(perk))
                {
                    learned += 1;
                }
            }
            foreach (var lesson in lessons)
            {
                foreach (var part in lesson["ingredients"])
                {
                    if (!part["quest"])
                    {
                        apprentice.GiveItem(part["name"], part["amount"]);
                    }
                }
            }
            Log.Info("learned " + learned);
        });
    }
}
`,
};
const recipes = require('./recipes/recipes.cjs');
const { progressionCatalog } = require('./progression/catalog.cjs');

function outputOf(linesOf, name) {
  return linesOf(name).filter((line) => line !== 'started');
}

const SOUP_KITCHEN = `
const [kettle] = kcds.world.CookPot.all();
if (kettle) {
  kcds.world.CookPot.refill(kettle.guid, 8);
  console.log("servings " + kcds.world.CookPot.get(kettle.guid).portions);
}
`;

for (const language of LANGUAGES) {
  test(`cook pot (${language}): a script sets how many servings a pot holds`, async (t) => {
    const { root, host, linesOf } = serverFixture(t);
    const code = {
      javascript: SOUP_KITCHEN,
      lua: LUA.SOUP_KITCHEN,
      csharp: CSHARP.SOUP_KITCHEN,
    };
    serverResource(root, 'kitchen', code, {}, language);
    await host.startAll();
    assert.deepEqual(outputOf(linesOf, 'kitchen'), ['servings 8']);
    const kettle = host.makeAPI().CookPot.all()[0];
    assert.deepEqual([kettle.portions, kettle.capacity, kettle.state], [8, 8, 'full']);
  });
}

const FEAST = `
const [kettle] = kcds.world.CookPot.all();
const describe = () => {
  const now = kcds.world.CookPot.get(kettle.guid);
  return [now.portions, now.capacity, now.enabled, now.state].join("/");
};
kcds.world.CookPot.setEnabled(kettle.guid, false);
kcds.world.CookPot.refill(kettle.guid, 12);
kcds.world.CookPot.setEnabled(kettle.guid, true);
console.log(describe());
kcds.world.CookPot.setState(kettle.guid, "half");
console.log(describe());
`;

for (const language of LANGUAGES) {
  test(`cook pot (${language}): refilling while disabled, then choosing a state`, async (t) => {
    const { root, host, linesOf } = serverFixture(t);
    const code = { javascript: FEAST, lua: LUA.FEAST, csharp: CSHARP.FEAST };
    serverResource(root, 'kitchen', code, {}, language);
    await host.startAll();
    assert.deepEqual(outputOf(linesOf, 'kitchen'), ['12/12/true/full', '2/4/true/half']);
  });
}

const NOTEBOOK = `
const notebooks = {};

kcds.core.Events.on("playerInventoryReady", (student) => {
  const learned = notebooks[student.nickname];
  if (!learned) return;
  for (const name of Object.keys(learned)) {
    kcds.world.Crafting.grantKnowledge(student, name, learned[name]);
  }
});

kcds.core.Events.on("playerDisconnect", (student) => {
  const learned = kcds.world.Crafting.knowledge(student);
  notebooks[student.nickname] = learned;
  console.log("noted " + JSON.stringify(learned));
});
`;

for (const language of LANGUAGES) {
  test(`alchemy (${language}): knowledge kept at disconnect is taught again next session`, async (t) => {
    const { root, host, report, linesOf, sent } = serverFixture(t);
    const code = { javascript: NOTEBOOK, lua: LUA.NOTEBOOK, csharp: CSHARP.NOTEBOOK };
    serverResource(root, 'notebook', code, {}, language);
    await host.startAll();
    const potion = recipes.define().get('potion_hair_o_dog');
    const reportPerks = (perks) => {
      const tracks = progressionCatalog.tracks().map(() => [5, 0, 0]);
      host.updateProgression(7, { tracks, mainPoints: 0, perks });
    };
    report(7);
    reportPerks([potion.perks[0]]);
    host.disconnect(7);
    assert.deepEqual(outputOf(linesOf, 'notebook'), ['noted {"potion_hair_o_dog":536870911}']);
    const returning = report(7);
    reportPerks([]);
    host.emit('playerInventoryReady', returning);
    const orders = sent.filter(
      (message) => message.op === 'progressionOrder' && message.kind === 'addPerk',
    );
    assert.deepEqual(orders.at(-1), {
      op: 'progressionOrder',
      id: 7,
      kind: 'addPerk',
      perk: potion.perks[0],
    });
  });
}

const APPRENTICE = `
kcds.core.Events.on("playerInventoryReady", (apprentice) => {
  const lessons = kcds.world.Crafting.recipes(apprentice, "smithing");
  const withPerk = lessons.filter((lesson) => lesson.perk);
  const learned = withPerk.filter((lesson) => apprentice.addPerk(lesson.perk));
  for (const lesson of lessons) {
    const supplies = lesson.ingredients.filter((part) => !part.quest);
    supplies.forEach((part) => apprentice.giveItem(part.name, part.amount));
  }
  console.log("learned " + learned.length);
});
`;

for (const language of LANGUAGES) {
  test(`smithing (${language}): a script teaches all recipes and hands out materials`, async (t) => {
    const { root, host, report, linesOf, sent } = serverFixture(t);
    const code = { javascript: APPRENTICE, lua: LUA.APPRENTICE, csharp: CSHARP.APPRENTICE };
    serverResource(root, 'apprentice', code, {}, language);
    await host.startAll();
    const player = report(7);
    const tracks = progressionCatalog.tracks().map(() => [5, 0, 0]);
    host.updateProgression(7, { tracks, mainPoints: 0, perks: [] });
    host.emit('playerInventoryReady', player);
    const smithing = recipes.define().all('smithing');
    const perks = new Set(smithing.map((recipe) => recipe.perks[0]));
    const orders = sent.filter(
      (message) => message.op === 'progressionOrder' && message.kind === 'addPerk',
    );
    assert.deepEqual(new Set(orders.map((message) => message.perk)), perks);
    assert.ok(outputOf(linesOf, 'apprentice')[0].startsWith('learned '));
  });
}

test('smithing: craftingCompleting sees quality and xp; a refusal fails the piece', () => {
  const { CraftingSessions } = require('./crafting/crafting.cjs');
  const events = [];
  const sessions = new CraftingSessions({
    emit: (name, player, payload) => {
      events.push({ name, payload });
      return name !== 'craftingCompleting' || payload.quality <= 0.95;
    },
    sendVerdict: () => {},
    give: (player, units) => units.map((unit, index) => ({ id: `b${index}`, amount: unit.amount })),
    remove: () => true,
  });
  const smith = { id: 7 };
  const horseshoe = recipes.define().get('r_horseshoe');
  sessions.observe(smith, 'smith S1997.1200,2498.7451,115.0716', 0, 1000);
  const [iron] = horseshoe.ingredients;
  sessions.inventoryChanged(smith, {
    revision: 1,
    reason: 'drop',
    items: [
      {
        id: 'iron',
        before: { id: 'iron', item: iron.item, amount: 1, quality: 1, metadata: {} },
        after: null,
      },
    ],
  });
  sessions.inventoryChanged(smith, {
    revision: 2,
    reason: 'loot',
    items: [
      {
        id: 'shoe',
        before: null,
        after: { id: 'shoe', item: horseshoe.products[0].item, amount: 1, quality: 1 },
      },
    ],
  });
  const completing = events.find((event) => event.name === 'craftingCompleting').payload;
  assert.equal(completing.xp, recipes.SMITHING_FINISHING_XP);
  assert.equal(completing.recipe, 'r_horseshoe');
  const ended = events.at(-1);
  assert.equal(ended.name, 'craftingEnded');
  assert.equal(ended.payload.outcome, completing.quality > 0.95 ? 'failed' : 'success');
});
