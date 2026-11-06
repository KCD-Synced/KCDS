'use strict';

// Client UI scripts in the production client runtime: what each call sends to the game
// and how it reacts. The game's side is covered by live testing. Each scenario runs with its
// script in JavaScript, in Lua and in C#.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  LANGUAGES,
  clientFixture,
  delay,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { HOST_VERSION } = require('../../../../Client/resources/native_host_movie.cjs');

async function start(f, name, source, language) {
  await f.runtime.apply({ resources: [f.resource(name, source, {}, name, language)] }, f.read);
  await delay(20);
}

function lastScreenFx(f) {
  return f.sent.filter((message) => message.op === 'screenFx').at(-1)?.values;
}

// What scripts print besides the runtime's own start line.
function printed(f, name) {
  return f.linesOf(name).filter((line) => line !== 'client resource started');
}

const DRUNK = {
  javascript: `
    const grade = kcds.ui.Hud.startEffect('colorGrade', { saturation: 0.7, contrast: 0.9, yellow: 0.05 }, { fadeInMs: 2000, fadeOutMs: 3000 });
    const blur = kcds.ui.Hud.startEffect('blur', { amount: 0.4 }, { fadeInMs: 2000, fadeOutMs: 3000 });
    kcds.ui.Hud.changeEffect(blur, { amount: 0.6 }, { fadeMs: 2000 });
    console.log('handles ' + (grade > 0) + ' ' + (blur > 0) + ' active ' + kcds.ui.Hud.effectRunning(grade));
    kcds.core.Events.on('drunk:sober', () => {
      console.log('removed ' + kcds.ui.Hud.endEffect(grade));
    });
    `,
  lua: `
    local Hud = kcds.ui.Hud
    local grade = Hud.startEffect("colorGrade", { saturation = 0.7, contrast = 0.9, yellow = 0.05 },
      { fadeInMs = 2000, fadeOutMs = 3000 })
    local blur = Hud.startEffect("blur", { amount = 0.4 }, { fadeInMs = 2000, fadeOutMs = 3000 })
    Hud.changeEffect(blur, { amount = 0.6 }, { fadeMs = 2000 })
    print("handles " .. tostring(grade > 0) .. " " .. tostring(blur > 0) .. " active "
      .. tostring(Hud.effectRunning(grade)))
    kcds.core.Events.on("drunk:sober", function()
      print("removed " .. tostring(Hud.endEffect(grade)))
    end)
    `,
};

const HIT = {
  javascript: `
    const blood = kcds.ui.Hud.startEffect('screenBlood', { amount: 1 }, { fadeInMs: 80, durationMs: 250, fadeOutMs: 900 });
    setTimeout(() => console.log('after ' + kcds.ui.Hud.effectRunning(blood)), 1400);
    `,
  lua: `
    local blood = kcds.ui.Hud.startEffect("screenBlood", { amount = 1 },
      { fadeInMs = 80, durationMs = 250, fadeOutMs = 900 })
    setTimeout(function() print("after " .. tostring(kcds.ui.Hud.effectRunning(blood))) end, 1400)
    `,
};

const LIST = {
  javascript: `console.log(JSON.stringify(kcds.ui.Hud.effectList().map((effect) => [effect.name, effect.params.map((p) => p.name)])));`,
  lua: `
    local listed = {}
    for _, effect in ipairs(kcds.ui.Hud.effectList()) do
      local names = {}
      for _, param in ipairs(effect.params) do
        table.insert(names, param.name)
      end
      table.insert(listed, { effect.name, names })
    end
    print(json.encode(listed))
    `,
};

const DEMO = {
  javascript: `
    let enabled = true;
    const counter = kcds.ui.ActionHint.create({ key: 'companion_interaction', text: 'Count' }, () => {
      console.log('counted');
    });
    kcds.ui.ActionHint.create({ key: 'attack_special', text: 'Lock the counter', mode: 'hold', holdDuration: 1.5 }, (hint) => {
      enabled = !enabled;
      kcds.ui.ActionHint.setEnabled(counter, enabled);
      kcds.ui.ActionHint.setText(hint, enabled ? 'Lock the counter' : 'Unlock the counter');
    });
    console.log('ids ' + counter);
    `,
  lua: `
    local ActionHint = kcds.ui.ActionHint
    local enabled = true
    local counter = ActionHint.create({ key = "companion_interaction", text = "Count" }, function()
      print("counted")
    end)
    ActionHint.create({ key = "attack_special", text = "Lock the counter", mode = "hold", holdDuration = 1.5 },
      function(hint)
        enabled = not enabled
        ActionHint.setEnabled(counter, enabled)
        ActionHint.setText(hint, enabled and "Lock the counter" or "Unlock the counter")
      end)
    print("ids " .. counter)
    `,
};

const KEYS = {
  javascript: `
    try {
      kcds.ui.ActionHint.create({ key: 'use', text: 'x' }, () => {});
    } catch (error) {
      console.log('threw ' + error.name);
    }
    console.log(kcds.ui.ActionHint.keys().join(','));
    `,
  lua: `
    local ok, problem = pcall(kcds.ui.ActionHint.create, { key = "use", text = "x" }, function() end)
    if not ok then
      print("threw " .. problem.name)
    end
    print(table.concat(kcds.ui.ActionHint.keys(), ","))
    `,
};

const MODE = {
  javascript: `
    kcds.core.Events.on('mapOpened', () => console.log('map opened'));
    kcds.core.Events.on('mapClosed', () => console.log('map closed'));
    kcds.core.Events.on('mapWaypointSet', (position, mapId) => {
      kcds.core.Events.toServer('my-mode:waypoint', { x: position.x, y: position.y, z: position.z, mapId });
    });
    kcds.core.Events.on('mode:categories', () => {
      kcds.world.WorldMap.setMapCategory('Shop', false);
      console.log('shop ' + kcds.world.WorldMap.mapCategoryShown('Shop'));
    });
    kcds.core.Events.on('mode:travel', () => {
      if (kcds.world.Travel.active) {
        const where = kcds.world.Travel.position;
        console.log('travelling, now at ' + where.x.toFixed(0) + ', ' + where.y.toFixed(0));
      }
    });
    `,
  lua: `
    kcds.core.Events.on("mapOpened", function() print("map opened") end)
    kcds.core.Events.on("mapClosed", function() print("map closed") end)
    kcds.core.Events.on("mapWaypointSet", function(position, mapId)
      kcds.core.Events.toServer("my-mode:waypoint",
        { x = position.x, y = position.y, z = position.z, mapId = mapId })
    end)
    kcds.core.Events.on("mode:categories", function()
      kcds.world.WorldMap.setMapCategory("Shop", false)
      print("shop " .. tostring(kcds.world.WorldMap.mapCategoryShown("Shop")))
    end)
    kcds.core.Events.on("mode:travel", function()
      if kcds.world.Travel.active then
        local where = kcds.world.Travel.position
        print(string.format("travelling, now at %.0f, %.0f", where.x, where.y))
      end
    end)
    `,
};

const ROUND = {
  javascript: `
    kcds.ui.Key.bind('g', () => {
      kcds.ui.Hud.showBanner('G pressed');
    });
    kcds.ui.Key.bind('e', 'down', () => {
      kcds.core.Events.toServer('my-mode:interact', { alt: kcds.ui.Key.isDown('lshift') });
    });
    function bindFromConfig(name, handler) {
      try {
        return kcds.ui.Key.bind(name, handler);
      } catch {
        console.log("'" + name + "' is not a key name");
        return false;
      }
    }
    bindFromConfig('f13', () => {});
    const KEY = 'z';
    kcds.ui.Key.bind(KEY, () => kcds.core.Events.toServer('my-mode:ready'));
    kcds.core.Events.on('round:labels', () => {
      kcds.ui.Hud.showBanner('Press ' + kcds.ui.Key.getLabel(KEY) + ' when you are ready', 5000);
    });
    kcds.core.Events.on('my-mode:round.countdown', () => {
      kcds.ui.Controls.disable();
      console.log('enabled ' + kcds.ui.Controls.isEnabled());
      setTimeout(() => {
        kcds.ui.Controls.enable();
        console.log('enabled ' + kcds.ui.Controls.isEnabled());
      }, 100);
    });
    `,
  lua: `
    local Key, Hud, Controls = kcds.ui.Key, kcds.ui.Hud, kcds.ui.Controls
    Key.bind("g", function()
      Hud.showBanner("G pressed")
    end)
    Key.bind("e", "down", function()
      kcds.core.Events.toServer("my-mode:interact", { alt = Key.isDown("lshift") })
    end)
    local function bindFromConfig(name, handler)
      local ok, bound = pcall(Key.bind, name, handler)
      if not ok then
        print("'" .. name .. "' is not a key name")
        return false
      end
      return bound
    end
    bindFromConfig("f13", function() end)
    local KEY = "z"
    Key.bind(KEY, function() kcds.core.Events.toServer("my-mode:ready") end)
    kcds.core.Events.on("round:labels", function()
      Hud.showBanner("Press " .. Key.getLabel(KEY) .. " when you are ready", 5000)
    end)
    kcds.core.Events.on("my-mode:round.countdown", function()
      Controls.disable()
      print("enabled " .. tostring(Controls.isEnabled()))
      setTimeout(function()
        Controls.enable()
        print("enabled " .. tostring(Controls.isEnabled()))
      end, 100)
    end)
    `,
};

const NOTICES = {
  javascript: `
    kcds.core.Events.on('my-mode:notify', (payload) => {
      if (typeof payload !== 'object' || payload === null) return;
      const text = payload.text;
      if (typeof text === 'string') kcds.ui.Hud.showNotification(text);
    });
    function announce(text) {
      if (kcds.ui.Hud.inConversation()) {
        kcds.ui.Hud.showBanner(text);
      } else {
        kcds.ui.Hud.showNotification(text);
      }
    }
    kcds.core.Events.on('notices:go', () => {
      kcds.ui.Hud.showBanner('Round starts in 10 seconds', 5000);
      kcds.ui.Hud.notifyPerk('perk_alchemist', 'Master of the Market');
      kcds.ui.Hud.notifyXp('str', 'Strength');
      announce('outside');
      kcds.ui.Hud.setHudPart('Stats', false);
      kcds.ui.Hud.setHudPart('Subtitles', false);
      console.log('stats ' + kcds.ui.Hud.hudPartShown('Stats'));
      kcds.ui.Hud.setHudPart('Stats', true);
      kcds.ui.Compass.visible = false;
      console.log('compass ' + kcds.ui.Compass.visible);
    });
    kcds.core.Events.on('notices:dialogue', () => announce('inside'));
    `,
  lua: `
    local Hud = kcds.ui.Hud
    kcds.core.Events.on("my-mode:notify", function(payload)
      if type(payload) ~= "userdata" then
        return
      end
      local text = payload.text
      if type(text) == "string" then
        Hud.showNotification(text)
      end
    end)
    local function announce(text)
      if Hud.inConversation() then
        Hud.showBanner(text)
      else
        Hud.showNotification(text)
      end
    end
    kcds.core.Events.on("notices:go", function()
      Hud.showBanner("Round starts in 10 seconds", 5000)
      Hud.notifyPerk("perk_alchemist", "Master of the Market")
      Hud.notifyXp("str", "Strength")
      announce("outside")
      Hud.setHudPart("Stats", false)
      Hud.setHudPart("Subtitles", false)
      print("stats " .. tostring(Hud.hudPartShown("Stats")))
      Hud.setHudPart("Stats", true)
      kcds.ui.Compass.visible = false
      print("compass " .. tostring(kcds.ui.Compass.visible))
    end)
    kcds.core.Events.on("notices:dialogue", function() announce("inside") end)
    `,
};

const CHATTY = {
  javascript: `
    kcds.core.Events.on('chatMessage', (message) => {
      if (typeof message !== 'object' || message === null) return;
      const { author, text } = message;
      if (typeof text !== 'string') return;
      console.log(typeof author === 'string' && author.length > 0 ? author + ': ' + text : text);
    });
    kcds.core.Events.on('chatSend', (text) => {
      if (typeof text !== 'string') return;
      if (text === '/hidehud') {
        kcds.ui.Compass.visible = false;
        return false;
      }
    });
    kcds.core.Events.on('chatty:help', () => kcds.core.Chat.send('/help'));
    `,
  lua: `
    kcds.core.Events.on("chatMessage", function(message)
      if type(message) ~= "userdata" then
        return
      end
      local author, text = message.author, message.text
      if type(text) ~= "string" then
        return
      end
      if type(author) == "string" and #author > 0 then
        print(author .. ": " .. text)
      else
        print(text)
      end
    end)
    kcds.core.Events.on("chatSend", function(text)
      if text == "/hidehud" then
        kcds.ui.Compass.visible = false
        return false
      end
    end)
    kcds.core.Events.on("chatty:help", function() kcds.core.Chat.send("/help") end)
    `,
};

const READER = {
  javascript: `
    kcds.core.Events.on('bookOpened', (id) => console.log('opened ' + id + ' open=' + kcds.ui.Book.currentBook()));
    kcds.core.Events.on('bookClosed', (id, reason) => console.log('closed ' + id + ' ' + reason));
    kcds.core.Events.on('reader:letter', () => {
      const id = kcds.ui.Book.open({
        style: 'letter',
        visual: 3,
        pages: ['<heading>Note for the smith</heading><paragraph>The order is ready.</paragraph>'],
      });
      if (id === null) console.log('refused ' + kcds.ui.Book.lastFailure());
      else console.log('letter ' + id);
    });
    kcds.core.Events.on('reader:ledger', () => {
      const id = kcds.ui.Book.open({
        style: 'plainBook',
        pages: [
          '<title>Forge records</title><paragraph>Work planned for today.</paragraph>',
          '<heading>Iron</heading><paragraph>Six bars for the gate.</paragraph>',
        ],
      });
      console.log(id === null ? 'refused ' + kcds.ui.Book.lastFailure() : 'ledger ' + id);
    });
    kcds.core.Events.on('reader:blur', () => console.log('legibility ' + kcds.ui.Book.setReadable(0.35)));
    `,
  lua: `
    local Book = kcds.ui.Book
    kcds.core.Events.on("bookOpened", function(id) print("opened " .. id .. " open=" .. Book.currentBook()) end)
    kcds.core.Events.on("bookClosed", function(id, reason) print("closed " .. id .. " " .. reason) end)
    kcds.core.Events.on("reader:letter", function()
      local id = Book.open({
        style = "letter",
        visual = 3,
        pages = { "<heading>Note for the smith</heading><paragraph>The order is ready.</paragraph>" },
      })
      if id == nil then
        print("refused " .. Book.lastFailure())
      else
        print("letter " .. id)
      end
    end)
    kcds.core.Events.on("reader:ledger", function()
      local id = Book.open({
        style = "plainBook",
        pages = {
          "<title>Forge records</title><paragraph>Work planned for today.</paragraph>",
          "<heading>Iron</heading><paragraph>Six bars for the gate.</paragraph>",
        },
      })
      print(id == nil and "refused " .. Book.lastFailure() or "ledger " .. id)
    end)
    kcds.core.Events.on("reader:blur", function() print("legibility " .. tostring(Book.setReadable(0.35))) end)
    `,
};

const FIRST_SCREEN = {
  javascript: `
    const screen = kcds.ui.NativeUI.createScreen();
    screen.on('ready', () => {
      const root = screen.root;
      if (!root) return;
      const panel = root.createClip();
      if (!panel) return;
      panel.invoke('beginFill', [0x1a1109, 92]);
      panel.invoke('moveTo', [0, 0]);
      panel.invoke('lineTo', [840, 0]);
      panel.invoke('lineTo', [840, 300]);
      panel.invoke('lineTo', [0, 300]);
      panel.invoke('endFill');
      panel.set({ x: 540, y: 390 });
      const label = panel.createText({ x: 32, y: 28, width: 776, height: 244 });
      label?.setTextStyle({ font: 'DisplayFont', size: 34, color: 0xe7e0cf });
      label?.setText('KCDS');
      panel.on('press', () => console.log('clicked'));
      console.log('focus ' + screen.focus());
    });
    screen.on('input', (action) => {
      if (action === 'kcds_ui_back') {
        screen.blur();
        console.log('hidden ' + screen.setVisible(false) + ' ' + screen.hasFocus());
      }
    });
    `,
  lua: `
    local screen = kcds.ui.NativeUI.createScreen()
    screen:on("ready", function()
      local root = screen.root
      if not root then
        return
      end
      local panel = root:createClip()
      if not panel then
        return
      end
      panel:invoke("beginFill", { 0x1a1109, 92 })
      panel:invoke("moveTo", { 0, 0 })
      panel:invoke("lineTo", { 840, 0 })
      panel:invoke("lineTo", { 840, 300 })
      panel:invoke("lineTo", { 0, 300 })
      panel:invoke("endFill")
      panel:set({ x = 540, y = 390 })
      local label = panel:createText({ x = 32, y = 28, width = 776, height = 244 })
      if label then
        label:setTextStyle({ font = "DisplayFont", size = 34, color = 0xe7e0cf })
        label:setText("KCDS")
      end
      panel:on("press", function() print("clicked") end)
      print("focus " .. tostring(screen:focus()))
    end)
    screen:on("input", function(action)
      if action == "kcds_ui_back" then
        screen:blur()
        print("hidden " .. tostring(screen:setVisible(false)) .. " " .. tostring(screen:hasFocus()))
      end
    end)
    `,
};

const ART = {
  javascript: `
    const menuScreen = kcds.ui.NativeUI.createScreen({ library: 'Menu' });
    menuScreen.on('ready', () => {
      const button = menuScreen.root?.attach('BasicButton');
      button?.set({ x: 200, y: 200 });
      console.log('button ' + (button !== null));
    });
    for (const library of kcds.ui.NativeUI.libraries()) {
      console.log((library.name || '(default)') + ': ' + library.exports.length + ' exports');
    }
    `,
  lua: `
    local menuScreen = kcds.ui.NativeUI.createScreen({ library = "Menu" })
    menuScreen:on("ready", function()
      local root = menuScreen.root
      local button = root and root:attach("BasicButton")
      if button then
        button:set({ x = 200, y = 200 })
      end
      print("button " .. tostring(button ~= nil))
    end)
    for _, library in ipairs(kcds.ui.NativeUI.libraries()) do
      local name = library.name
      if name == nil or name == "" then
        name = "(default)"
      end
      print(name .. ": " .. #library.exports .. " exports")
    end
    `,
};

const DRIVE = {
  javascript: `
    const menu = kcds.ui.NativeUI.element('Menu', 47001);
    menu.show();
    menu.call('ClearAll', [0]);
    menu.call('PreparePage', [0, 400, 6, 'KCDS', 1]);
    menu.call('AddBasicButton', ['first', 0, 'Do the thing', 'A tooltip', false]);
    menu.call('ShowPage', ['']);
    menu.on('OnButton', (args) => console.log('pressed ' + String(args[0])));
    `,
  lua: `
    local menu = kcds.ui.NativeUI.element("Menu", 47001)
    menu:show()
    menu:call("ClearAll", { 0 })
    menu:call("PreparePage", { 0, 400, 6, "KCDS", 1 })
    menu:call("AddBasicButton", { "first", 0, "Do the thing", "A tooltip", false })
    menu:call("ShowPage", { "" })
    menu:on("OnButton", function(args) print("pressed " .. tostring(args[1])) end)
    `,
};

const MAPLESS = {
  javascript: `
    kcds.ui.NativeUI.setMenuEnabled('map', false);
    console.log('enabled ' + kcds.ui.NativeUI.isMenuEnabled('map'));
    kcds.ui.NativeUI.setMenuEnabled('map', true);
    console.log('enabled ' + kcds.ui.NativeUI.isMenuEnabled('map'));
    `,
  lua: `
    kcds.ui.NativeUI.setMenuEnabled("map", false)
    print("enabled " .. tostring(kcds.ui.NativeUI.isMenuEnabled("map")))
    kcds.ui.NativeUI.setMenuEnabled("map", true)
    print("enabled " .. tostring(kcds.ui.NativeUI.isMenuEnabled("map")))
    `,
};

// The C# scripts. JavaScript prints true and false lower-case; C# scripts say so.
const CSHARP_TEXT = `
    private static string Text(bool value)
    {
        return value ? "true" : "false";
    }
`;

DRUNK.csharp = `
public class Drunk : Script
{
${CSHARP_TEXT}
    public override void OnStart()
    {
        var grade = Hud.StartEffect(
            "colorGrade",
            new { saturation = 0.7, contrast = 0.9, yellow = 0.05 },
            new { fadeInMs = 2000, fadeOutMs = 3000 });
        var blur = Hud.StartEffect("blur", new { amount = 0.4 }, new { fadeInMs = 2000, fadeOutMs = 3000 });
        Hud.ChangeEffect(blur, new { amount = 0.6 }, new { fadeMs = 2000 });
        Log.Info("handles " + Text(grade > 0) + " " + Text(blur > 0) + " active " + Text(Hud.EffectRunning(grade)));
        Events.On("drunk:sober", () =>
        {
            Log.Info("removed " + Text(Hud.EndEffect(grade)));
        });
    }
}
`;

HIT.csharp = `
public class Hit : Script
{
${CSHARP_TEXT}
    public override void OnStart()
    {
        var blood = Hud.StartEffect("screenBlood", new { amount = 1 }, new { fadeInMs = 80, durationMs = 250, fadeOutMs = 900 });
        Timers.SetTimeout(() => Log.Info("after " + Text(Hud.EffectRunning(blood))), 1400);
    }
}
`;

LIST.csharp = `
public class List : Script
{
    public override void OnStart()
    {
        var listed = Hud.EffectList()
            .Select((effect) => new object[]
            {
                effect["name"].AsString()!,
                effect["params"].Select((param) => param["name"].AsString()).ToArray(),
            })
            .ToList();
        Log.Info(System.Text.Json.JsonSerializer.Serialize(listed));
    }
}
`;

DEMO.csharp = `
public class Demo : Script
{
    public override void OnStart()
    {
        var enabled = true;
        var counter = ActionHint.Create(new { key = "companion_interaction", text = "Count" }, () =>
        {
            Log.Info("counted");
        });
        var lockHint = new { key = "attack_special", text = "Lock the counter", mode = "hold", holdDuration = 1.5 };
        ActionHint.Create(lockHint, (JsValue hint) =>
        {
            enabled = !enabled;
            ActionHint.SetEnabled(counter, enabled);
            ActionHint.SetText(hint, enabled ? "Lock the counter" : "Unlock the counter");
        });
        Log.Info("ids " + counter);
    }
}
`;

KEYS.csharp = `
public class Keys : Script
{
    public override void OnStart()
    {
        try
        {
            ActionHint.Create(new { key = "use", text = "x" }, () => { });
        }
        catch (JsError problem)
        {
            Log.Info("threw " + problem.Name);
        }
        Log.Info(string.Join(",", ActionHint.Keys()));
    }
}
`;

MODE.csharp = `
using System.Globalization;

public class Mode : Script
{
${CSHARP_TEXT}
    public override void OnStart()
    {
        Events.On("mapOpened", () => Log.Info("map opened"));
        Events.On("mapClosed", () => Log.Info("map closed"));
        Events.On("mapWaypointSet", (Vector3 position, double mapId) =>
        {
            Events.ToServer("my-mode:waypoint", new { x = position.X, y = position.Y, z = position.Z, mapId });
        });
        Events.On("mode:categories", () =>
        {
            WorldMap.SetMapCategory("Shop", false);
            Log.Info("shop " + Text(WorldMap.MapCategoryShown("Shop")));
        });
        Events.On("mode:travel", () =>
        {
            if (Travel.Active)
            {
                var where = Travel.Position;
                var text = string.Format(CultureInfo.InvariantCulture, "travelling, now at {0:F0}, {1:F0}", where.X, where.Y);
                Log.Info(text);
            }
        });
    }
}
`;

ROUND.csharp = `
public class Round : Script
{
    private const string ReadyKey = "z";

${CSHARP_TEXT}
    private static bool BindFromConfig(string name, Action handler)
    {
        try
        {
            return Key.Bind(name, handler);
        }
        catch (JsError)
        {
            Log.Info("'" + name + "' is not a key name");
            return false;
        }
    }

    public override void OnStart()
    {
        Key.Bind("g", new Action(() => Hud.ShowBanner("G pressed")));
        Key.Bind("e", "down", () =>
        {
            Events.ToServer("my-mode:interact", new { alt = Key.IsDown("lshift") });
        });
        BindFromConfig("f13", () => { });
        Key.Bind(ReadyKey, new Action(() => Events.ToServer("my-mode:ready")));
        Events.On("round:labels", () =>
        {
            Hud.ShowBanner("Press " + Key.GetLabel(ReadyKey) + " when you are ready", 5000);
        });
        Events.On("my-mode:round.countdown", () =>
        {
            Controls.Disable();
            Log.Info("enabled " + Text(Controls.IsEnabled()));
            Timers.SetTimeout(() =>
            {
                Controls.Enable();
                Log.Info("enabled " + Text(Controls.IsEnabled()));
            }, 100);
        });
    }
}
`;

NOTICES.csharp = `
public class Notices : Script
{
${CSHARP_TEXT}
    private static void Announce(string text)
    {
        if (Hud.InConversation())
        {
            Hud.ShowBanner(text);
        }
        else
        {
            Hud.ShowNotification(text);
        }
    }

    public override void OnStart()
    {
        Events.On("my-mode:notify", (JsValue payload) =>
        {
            var text = payload["text"];
            if (text.Type == "string")
            {
                Hud.ShowNotification(text);
            }
        });
        Events.On("notices:go", () =>
        {
            Hud.ShowBanner("Round starts in 10 seconds", 5000);
            Hud.NotifyPerk("perk_alchemist", "Master of the Market");
            Hud.NotifyXp("str", "Strength");
            Announce("outside");
            Hud.SetHudPart("Stats", false);
            Hud.SetHudPart("Subtitles", false);
            Log.Info("stats " + Text(Hud.HudPartShown("Stats")));
            Hud.SetHudPart("Stats", true);
            Compass.Visible = false;
            Log.Info("compass " + Text(Compass.Visible));
        });
        Events.On("notices:dialogue", () => Announce("inside"));
    }
}
`;

CHATTY.csharp = `
public class Chatty : Script
{
    public override void OnStart()
    {
        Events.On("chatMessage", (JsValue message) =>
        {
            var author = message["author"];
            var text = message["text"];
            if (text.Type != "string")
            {
                return;
            }
            var named = author.Type == "string" && author.AsString()!.Length > 0;
            Log.Info(named ? author + ": " + text : text.ToString());
        });
        // Answering false keeps a client-only command from the server; null lets the line go.
        Events.On("chatSend", (JsValue text) =>
        {
            if (text.Type == "string" && text.AsString() == "/hidehud")
            {
                Compass.Visible = false;
                return false;
            }
            return (bool?)null;
        });
        Events.On("chatty:help", () => Chat.Send("/help"));
    }
}
`;

READER.csharp = `
public class Reader : Script
{
${CSHARP_TEXT}
    public override void OnStart()
    {
        Events.On("bookOpened", (double id) => Log.Info("opened " + id + " open=" + Book.CurrentBook()));
        Events.On("bookClosed", (double id, string reason) => Log.Info("closed " + id + " " + reason));
        Events.On("reader:letter", () =>
        {
            var id = Book.Open(new
            {
                style = "letter",
                visual = 3,
                pages = new[] { "<heading>Note for the smith</heading><paragraph>The order is ready.</paragraph>" },
            });
            Log.Info(id == null ? "refused " + Book.LastFailure() : "letter " + id);
        });
        Events.On("reader:ledger", () =>
        {
            var id = Book.Open(new
            {
                style = "plainBook",
                pages = new[]
                {
                    "<title>Forge records</title><paragraph>Work planned for today.</paragraph>",
                    "<heading>Iron</heading><paragraph>Six bars for the gate.</paragraph>",
                },
            });
            Log.Info(id == null ? "refused " + Book.LastFailure() : "ledger " + id);
        });
        Events.On("reader:blur", () => Log.Info("legibility " + Text(Book.SetReadable(0.35))));
    }
}
`;

FIRST_SCREEN.csharp = `
public class FirstScreen : Script
{
${CSHARP_TEXT}
    public override void OnStart()
    {
        var screen = NativeUI.CreateScreen();
        screen.On("ready", () =>
        {
            var root = screen.Root;
            if (root == null)
            {
                return;
            }
            var panel = root.CreateClip();
            if (panel == null)
            {
                return;
            }
            panel.Invoke("beginFill", new[] { 0x1a1109, 92 });
            panel.Invoke("moveTo", new[] { 0, 0 });
            panel.Invoke("lineTo", new[] { 840, 0 });
            panel.Invoke("lineTo", new[] { 840, 300 });
            panel.Invoke("lineTo", new[] { 0, 300 });
            panel.Invoke("endFill");
            panel.Set(new { x = 540, y = 390 });
            var label = panel.CreateText(new { x = 32, y = 28, width = 776, height = 244 });
            label?.SetTextStyle(new { font = "DisplayFont", size = 34, color = 0xe7e0cf });
            label?.SetText("KCDS");
            panel.On("press", () => Log.Info("clicked"));
            Log.Info("focus " + Text(screen.Focus()));
        });
        screen.On("input", (string action) =>
        {
            if (action == "kcds_ui_back")
            {
                screen.Blur();
                Log.Info("hidden " + Text(screen.SetVisible(false)) + " " + Text(screen.HasFocus()));
            }
        });
    }
}
`;

ART.csharp = `
public class Art : Script
{
${CSHARP_TEXT}
    public override void OnStart()
    {
        var menuScreen = NativeUI.CreateScreen(new { library = "Menu" });
        menuScreen.On("ready", () =>
        {
            var button = menuScreen.Root?.Attach("BasicButton");
            button?.Set(new { x = 200, y = 200 });
            Log.Info("button " + Text(button != null));
        });
        foreach (var library in NativeUI.Libraries())
        {
            var name = library["name"].AsString();
            var shown = string.IsNullOrEmpty(name) ? "(default)" : name;
            Log.Info(shown + ": " + library["exports"].Length + " exports");
        }
    }
}
`;

DRIVE.csharp = `
public class Drive : Script
{
    public override void OnStart()
    {
        var menu = NativeUI.Element("Menu", 47001);
        menu.Show();
        menu.Call("ClearAll", new[] { 0 });
        menu.Call("PreparePage", new object[] { 0, 400, 6, "KCDS", 1 });
        menu.Call("AddBasicButton", new object[] { "first", 0, "Do the thing", "A tooltip", false });
        menu.Call("ShowPage", new[] { "" });
        menu.On("OnButton", (JsValue args) => Log.Info("pressed " + args[0]));
    }
}
`;

MAPLESS.csharp = `
public class Mapless : Script
{
${CSHARP_TEXT}
    public override void OnStart()
    {
        NativeUI.SetMenuEnabled("map", false);
        Log.Info("enabled " + Text(NativeUI.IsMenuEnabled("map")));
        NativeUI.SetMenuEnabled("map", true);
        Log.Info("enabled " + Text(NativeUI.IsMenuEnabled("map")));
    }
}
`;

// The game side of NativeUI answers through the synchronous channel; here a stand-in game.
function nativeGame(f, extra = () => undefined) {
  const asked = [];
  f.runtime.sync.call = (request) => {
    asked.push(request);
    const special = extra(request);
    if (special !== undefined) {
      return special;
    }
    if (request.action === 'get' && request.member === 'kcdsVersion') {
      return HOST_VERSION;
    }
    if (request.action === 'invoke' && request.method === 'kcdsStage') {
      return '1920,1080';
    }
    if (request.op === 'elementInfo') {
      return { valid: true, visible: true };
    }
    return true;
  };
  return asked;
}

for (const language of LANGUAGES) {
  test(`screen-effects (${language}): a drunk grade and blur ease in and sober up on removal`, async (t) => {
    const f = clientFixture(t);
    await start(f, 'drunk', DRUNK, language);
    assert.ok(
      f.linesOf('drunk').includes('handles true true active true'),
      f.linesOf('drunk').join('\n'),
    );
    // Fades start from the neutral picture.
    assert.ok(lastScreenFx(f).Global_User_Saturation > 0.95);
    await delay(2100);
    assert.equal(lastScreenFx(f).Global_User_Saturation, 0.7);
    assert.equal(lastScreenFx(f).Global_User_Contrast, 0.9);
    assert.equal(lastScreenFx(f).FilterBlurring_Amount, 0.6);
  });

  test(`screen-effects (${language}): one flash of blood is up fast and gone on its own`, async (t) => {
    const f = clientFixture(t);
    await start(f, 'hit', HIT, language);
    // screenBlood is a red cast (magenta and yellow) that also darkens (screen_effects.cjs).
    await delay(150);
    assert.equal(lastScreenFx(f).Global_User_ColorM, 0.35);
    await delay(1400);
    assert.equal(lastScreenFx(f).Global_User_ColorM, 0);
    assert.equal(lastScreenFx(f).Global_User_Brightness, 1);
    assert.ok(f.linesOf('hit').includes('after false'));
  });

  test(`screen-effects (${language}): the list names every effect with its parameters`, async (t) => {
    const f = clientFixture(t);
    await start(f, 'list', LIST, language);
    const listed = JSON.parse(f.linesOf('list').find((line) => line.startsWith('[')));
    assert.deepEqual(listed, [
      [
        'colorGrade',
        ['saturation', 'brightness', 'contrast', 'cyan', 'magenta', 'yellow', 'darkness', 'hue'],
      ],
      ['blur', ['amount']],
      ['ghosting', ['amount']],
      ['screenBlood', ['amount']],
    ]);
  });

  test(`action-hints (${language}): the lock toggle greys the counter out and back`, async (t) => {
    const f = clientFixture(t);
    await start(f, 'demo', DEMO, language);
    const shown = () => f.sent.filter((message) => message.op === 'actionHints').at(-1).hints;
    const counter = shown().find((hint) => hint.key === 'companion_interaction');
    const lock = shown().find((hint) => hint.key === 'attack_special');
    assert.deepEqual([lock.mode, lock.holdDuration], ['hold', 1.5]);
    f.runtime.actionHints.receiveFired({ id: counter.id });
    f.runtime.actionHints.receiveFired({ id: lock.id });
    assert.equal(shown().find((hint) => hint.id === counter.id).enabled, false);
    assert.equal(shown().find((hint) => hint.id === lock.id).text, 'Unlock the counter');
    f.runtime.actionHints.receiveFired({ id: counter.id });
    assert.deepEqual(
      f.linesOf('demo').filter((line) => line === 'counted'),
      ['counted'],
    );
  });

  test(`action-hints (${language}): any other key throws and keys() lists the controls`, async (t) => {
    const f = clientFixture(t);
    await start(f, 'keys', KEYS, language);
    assert.ok(f.linesOf('keys').includes('threw RangeError'), f.linesOf('keys').join('\n'));
    assert.ok(
      f
        .linesOf('keys')
        .includes(
          'primary_action,attack_special,torch,companion_interaction,distract,jump,script_action',
        ),
    );
  });

  test(`map (${language}): a HUD view hides while the map is up and the marker goes to the server`, async (t) => {
    const f = clientFixture(t);
    await start(f, 'mode', MODE, language);
    f.runtime.worldMap.receiveState({ open: true, waypoint: null, inWorld: true });
    f.runtime.worldMap.receiveEvent({
      name: 'mapWaypointSet',
      where: { x: -420.5, y: 812, z: 31, mapId: 0 },
      moved: false,
    });
    f.runtime.worldMap.receiveState({
      open: false,
      waypoint: { x: -420.5, y: 812, z: 31, mapId: 0 },
      inWorld: true,
      travel: { active: true, x: 1200.4, y: 300.6, z: 40 },
    });
    await f.runtime.services.emit('shared', 'mode:categories', []);
    await f.runtime.services.emit('shared', 'mode:travel', []);
    await delay(20);
    assert.deepEqual(printed(f, 'mode'), [
      'map opened',
      'map closed',
      'shop false',
      'travelling, now at 1200, 301',
    ]);
    const relayed = f.sent.find(
      (message) => message.op === 'event' && message.name === 'my-mode:waypoint',
    );
    assert.deepEqual(JSON.parse(relayed.payload), { x: -420.5, y: 812, z: 31, mapId: 0 });
  });

  test(`input (${language}): binds, a layout label in the info text and a countdown hold`, async (t) => {
    const f = clientFixture(t);
    f.runtime.hud.receiveHost({ ready: true });
    await start(f, 'round', ROUND, language);
    f.runtime.input.receiveLabels({ labels: { z: 'Y' } });
    f.runtime.input.receiveEdges({ edges: [['g', 'down']], down: ['g'] });
    f.runtime.input.receiveEdges({ edges: [['e', 'down']], down: ['e', 'lshift'] });
    await f.runtime.services.emit('shared', 'round:labels', []);
    await f.runtime.services.emit('shared', 'my-mode:round.countdown', []);
    await delay(150);
    const infos = f.sent.filter((message) => message.op === 'hud' && message.call === 'showBanner');
    assert.deepEqual(
      infos.map((message) => message.args.slice(0, 1).concat(message.args[2])),
      [
        ['G pressed', 3000],
        ['Press Y when you are ready', 5000],
      ],
    );
    const interact = f.sent.find(
      (message) => message.op === 'event' && message.name === 'my-mode:interact',
    );
    assert.deepEqual(JSON.parse(interact.payload), { alt: true });
    assert.deepEqual(printed(f, 'round'), [
      "'f13' is not a key name",
      'enabled false',
      'enabled true',
    ]);
  });

  test(`hud (${language}): notices, toasts, dialogue-aware announce and hidden elements`, async (t) => {
    const f = clientFixture(t);
    f.runtime.hud.receiveHost({ ready: true, dialogue: false });
    await start(f, 'notices', NOTICES, language);
    await f.runtime.services.emit('shared', 'my-mode:notify', [
      { text: 'The gates open at dawn.' },
    ]);
    await f.runtime.services.emit('shared', 'notices:go', []);
    f.runtime.hud.receiveHost({ ready: true, dialogue: true });
    await f.runtime.services.emit('shared', 'notices:dialogue', []);
    await delay(20);
    const calls = f.sent
      .filter((message) => message.op === 'hud')
      .map((message) => [message.call, message.args[0]]);
    assert.deepEqual(calls, [
      ['showNotification', 'The gates open at dawn.'],
      ['showBanner', 'Round starts in 10 seconds'],
      ['notifyPerk', 'perk_alchemist'],
      ['notifyXp', 'str'],
      ['showNotification', 'outside'],
      ['showBanner', 'inside'],
    ]);
    const hidden = f.sent.filter((message) => message.op === 'hudHidden').at(-1).elements;
    assert.deepEqual(hidden, ['Compass', 'Subtitles']);
    assert.ok(f.linesOf('notices').includes('stats false'));
    assert.ok(f.linesOf('notices').includes('compass false'));
  });

  test(`chat (${language}): read lines, send a command, keep a client-only command local`, async (t) => {
    const f = clientFixture(t);
    await start(f, 'chatty', CHATTY, language);
    f.runtime.chat.receiveMessage({ author: 'Henry', text: 'hello', color: 0 });
    f.runtime.chat.receiveMessage({ author: '', text: 'Server restart in 5 minutes.', color: 0 });
    f.runtime.chat.receiveTyped({ text: '/hidehud' });
    f.runtime.chat.receiveTyped({ text: 'hi all' });
    await f.runtime.services.emit('shared', 'chatty:help', []);
    await delay(20);
    assert.deepEqual(printed(f, 'chatty'), ['Henry: hello', 'Server restart in 5 minutes.']);
    assert.deepEqual(
      f.sent.filter((message) => message.op === 'chatOut').map((message) => message.text),
      ['hi all', '/help'],
    );
    assert.deepEqual(f.sent.filter((message) => message.op === 'hudHidden').at(-1).elements, [
      'Compass',
    ]);
  });

  test(`books (${language}): a letter and a ledger open, report their events, and refusals say why`, async (t) => {
    const f = clientFixture(t);
    f.runtime.book.receiveHost({ inWorld: true, canRead: true });
    await start(f, 'reader', READER, language);
    await f.runtime.services.emit('shared', 'reader:letter', []);
    await f.runtime.services.emit('shared', 'reader:ledger', []);
    f.runtime.book.receiveEvent({ id: 1, name: 'opened' });
    await f.runtime.services.emit('shared', 'reader:blur', []);
    f.runtime.book.receiveEvent({ id: 1, name: 'closed', reason: 'player' });
    f.runtime.book.receiveHost({ inWorld: true, canRead: false });
    await f.runtime.services.emit('shared', 'reader:ledger', []);
    await delay(20);
    assert.deepEqual(printed(f, 'reader'), [
      'letter 1',
      'refused A book is already open',
      'opened 1 open=1',
      'legibility true',
      'closed 1 player',
      'refused The player cannot read here',
    ]);
    const opened = f.sent.find((message) => message.op === 'bookOpen');
    assert.equal(opened.style, 2);
    assert.equal(opened.type, 5);
    assert.equal(opened.visual, 3);
  });

  test(`native-ui (${language}): a first screen draws, styles text, takes focus and closes on back`, async (t) => {
    const f = clientFixture(t);
    const asked = nativeGame(f);
    await start(f, 'first', FIRST_SCREEN, language);
    await delay(300);
    assert.ok(f.linesOf('first').includes('focus true'), f.linesOf('first').join('\n'));
    const draw = asked.find((request) => request.action === 'draw');
    assert.deepEqual(
      draw.commands.map((command) => command.method),
      ['beginFill', 'moveTo', 'lineTo', 'lineTo', 'lineTo', 'endFill'],
    );
    assert.ok(
      asked.some((request) => request.method === 'kcdsFormat' && request.args[1] === 'DisplayFont'),
    );
    assert.ok(asked.some((request) => request.member === 'text' && request.value === 'KCDS'));
    f.runtime.nativeUI.receiveEdges({ edges: [['escape', 'down']] });
    await delay(20);
    assert.ok(f.linesOf('first').includes('hidden true false'));
  });

  test(`native-ui (${language}): the game artwork attaches and libraries lists the exports`, async (t) => {
    const f = clientFixture(t);
    const asked = nativeGame(f, (request) =>
      request.op === 'elementLibrary'
        ? { name: request.name, movie: `Libs/UI/${request.name}.gfx`, exports: ['BasicButton'] }
        : undefined,
    );
    await start(f, 'art', ART, language);
    await delay(300);
    assert.deepEqual(printed(f, 'art'), ['(default): 1 exports', 'hud: 1 exports', 'button true']);
    assert.ok(
      asked.some((request) => request.action === 'attach' && request.symbol === 'BasicButton'),
    );
  });

  test(`native-ui (${language}): a private Menu copy is built and its button event arrives`, async (t) => {
    const f = clientFixture(t);
    const asked = nativeGame(f);
    await start(f, 'drive', DRIVE, language);
    assert.deepEqual(
      asked.filter((request) => request.op === 'elementCall').map((request) => request.fn),
      ['ClearAll', 'PreparePage', 'AddBasicButton', 'ShowPage'],
    );
    f.runtime.nativeUI.receiveElementEvent({
      name: 'Menu',
      instance: 47001,
      event: 'OnButton',
      args: ['first'],
    });
    await delay(20);
    assert.ok(f.linesOf('drive').includes('pressed first'));
  });

  test(`native-ui (${language}): turning off the map holds until this resource gives it back`, async (t) => {
    const f = clientFixture(t);
    nativeGame(f);
    await start(f, 'mapless', MAPLESS, language);
    assert.deepEqual(printed(f, 'mapless'), ['enabled false', 'enabled true']);
    assert.deepEqual(
      f.sent.filter((message) => message.op === 'nativeMenus').map((message) => message.disabled),
      [[6], []],
    );
  });
}
