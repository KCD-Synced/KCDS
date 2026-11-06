'use strict';

// Shared state on entities as resource scripts use it.
// The server half is the trusted ResourceHost; the client half is the browser runtime fed with the
// server's per-view projections, as EntitySender delivers them. The wire itself is covered by
// Dev/testkit/state_change_roundtrip.py and Dev/testkit/guide_scenarios_roundtrip.py.
// Each scenario runs with its scripts in JavaScript and in Lua.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {
  GLOBAL_WORLD,
  LANGUAGES,
  ENTRY,
  codeIn,
  entryContent,
  serverResource,
  serverFixture,
  clientFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const TEAMS = {
  javascript: `
kcds.core.Events.on('playerSpawned', (player) => {
  player.state.set('team', 'ravens');
  player.state.set('score', 0);
  player.state.set('wanted', 3, { scope: 'owner' });
  player.state.set('joinedAt', 1700000000000, { scope: 'server' });
});
const changes = [];
kcds.core.Events.on('teams:watch', (id) => {
  const player = kcds.players.Player.getById(id);
  const stop = player.state.onChange('score', (key, value, previous) => {
    changes.push(key + ':' + String(previous) + '->' + String(value));
  });
  player.state.onChange(null, (key, value, previous) => {
    changes.push('any ' + key);
  });
  kcds.core.Events.on('teams:unwatch', () => stop());
});
kcds.core.Events.on('teams:report', () => console.log(JSON.stringify(changes)));
`,
  lua: `
kcds.core.Events.on("playerSpawned", function(player)
  player.state:set("team", "ravens")
  player.state:set("score", 0)
  player.state:set("wanted", 3, { scope = "owner" })
  player.state:set("joinedAt", 1700000000000, { scope = "server" })
end)
local changes = {}
kcds.core.Events.on("teams:watch", function(id)
  local player = kcds.players.Player.getById(id)
  local stop = player.state:onChange("score", function(key, value, previous)
    table.insert(changes, key .. ":" .. tostring(previous) .. "->" .. tostring(value))
  end)
  player.state:onChange(null, function(key, value, previous)
    table.insert(changes, "any " .. key)
  end)
  kcds.core.Events.on("teams:unwatch", function() stop() end)
end)
kcds.core.Events.on("teams:report", function()
  print(#changes == 0 and "[]" or json.encode(changes))
end)
`,
  csharp: `
public class Teams : Script
{
    private readonly List<string> changes = new();

    public override void OnStart()
    {
        Events.On("playerSpawned", (Player player) =>
        {
            player.State.Set("team", "ravens");
            player.State.Set("score", 0);
            player.State.Set("wanted", 3, new { Scope = "owner" });
            player.State.Set("joinedAt", 1700000000000, new { Scope = "server" });
        });
        Events.On("teams:watch", (int id) =>
        {
            var player = Player.GetById(id)!;
            var stop = player.State.OnChange("score", (string key, JsValue value, JsValue previous) =>
            {
                changes.Add(key + ":" + previous + "->" + value);
            });
            player.State.OnChange(Sdk.Null, (string key) => changes.Add("any " + key));
            Events.On("teams:unwatch", () => stop.Invoke());
        });
        Events.On("teams:report", () => Log.Info(System.Text.Json.JsonSerializer.Serialize(changes)));
    }
}
`,
};

const TEAMS_CLIENT = {
  javascript: `
kcds.core.Events.on('entityStateChange', (entity, key, value) => {
  const me = kcds.players.LocalPlayer;
  if (!me || entity.id !== me.id || key !== 'team') {
    return;
  }
  console.log('my team is now ' + String(value));
});
kcds.core.Events.on('teams:check', () => {
  const me = kcds.players.LocalPlayer;
  console.log('team ' + me.state.get('team') + ' wanted ' + me.state.get('wanted'));
  console.log('client set ' + typeof me.state.set + ' remove ' + typeof me.state.remove);
});
`,
  lua: `
kcds.core.Events.on("entityStateChange", function(entity, key, value)
  local me = kcds.players.LocalPlayer
  if not me or entity.id ~= me.id or key ~= "team" then
    return
  end
  print("my team is now " .. tostring(value))
end)
kcds.core.Events.on("teams:check", function()
  local me = kcds.players.LocalPlayer
  print("team " .. me.state:get("team") .. " wanted " .. me.state:get("wanted"))
  print("client set " .. type(me.state.set) .. " remove " .. type(me.state.remove))
end)
`,
  csharp: `
public class TeamsClient : Script
{
    public override void OnStart()
    {
        Events.On("entityStateChange", (JsValue entity, string key, JsValue value) =>
        {
            var me = LocalPlayer.Current;
            if (me == null || entity["id"].AsNumber() != me.Id || key != "team")
            {
                return;
            }
            Log.Info("my team is now " + value);
        });
        Events.On("teams:check", () =>
        {
            var me = LocalPlayer.Current!;
            Log.Info("team " + me.State.Get("team") + " wanted " + me.State.Get("wanted"));
            var state = me.State.AsValue();
            Log.Info("client set " + state["set"] + " remove " + state["remove"]);
        });
    }
}
`,
};

const SYNC = {
  javascript: `
kcds.core.Events.on('playerSpawned', (player) => {
  let woke = false;
  player.state.onChange('flag', () => {
    woke = true;
  });
  player.state.set('flag', true);
  console.log('woke before set returned ' + woke);
});
`,
  lua: `
kcds.core.Events.on("playerSpawned", function(player)
  local woke = false
  player.state:onChange("flag", function()
    woke = true
  end)
  player.state:set("flag", true)
  print("woke before set returned " .. tostring(woke))
end)
`,
  csharp: `
public class Sync : Script
{
    public override void OnStart()
    {
        Events.On("playerSpawned", (Player player) =>
        {
            var woke = false;
            player.State.OnChange("flag", () => { woke = true; });
            player.State.Set("flag", true);
            Log.Info("woke before set returned " + (woke ? "true" : "false"));
        });
    }
}
`,
};

// A second script for the reloaded resource.
const V2 = {
  javascript: '\nconsole.log("v2");',
  lua: '\nprint("v2")',
  csharp: '\npublic class V2 : Script { public override void OnStart() { Log.Info("v2"); } }',
};

// What a script prints for a missing value: JavaScript tells null from undefined, Lua has nil and
// C# null for both.
function shown(language, value) {
  if (value !== 'null' && value !== 'undefined') {
    return value;
  }
  if (language === 'lua') {
    return 'nil';
  }
  return language === 'csharp' ? 'null' : value;
}

// Builds a directory entry for the browser runtime from a server player handle.
function directoryEntry(player) {
  return {
    id: player.id,
    nativeId: player.id,
    kind: 'player',
    dimension: GLOBAL_WORLD,
    position: player.position.toJSON(),
    rotation: player.rotation.toJSON(),
  };
}

for (const language of LANGUAGES) {
  test(`state (${language}): team, score and scoped keys from write to the owning and other client`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'teams', TEAMS, {}, language);
    await f.host.startAll();
    const owner = f.report(71);
    const other = f.report(72);
    const state = owner.state;

    // Writes report whether anything changed.
    assert.equal(state.get('team'), 'ravens');
    assert.equal(state.set('team', 'ravens'), false);
    assert.equal(state.set('team', 'ravens', { scope: 'owner' }), true);
    assert.equal(state.set('team', 'ravens'), true);
    assert.throws(() => state.set('team', 'ravens', { scope: 'everyone' }), /Unknown state scope/);
    assert.throws(() => state.set('k'.repeat(65), 1), /64 UTF-8 bytes/);
    assert.throws(() => state.set('big', 'x'.repeat(4097)), /4096 bytes/);
    assert.equal(state.remove('nothing'), false);

    // onChange watches one key, or all with null; unrelated keys do not wake the score watcher.
    await f.host.services.emit('shared', 'teams:watch', [71]);
    state.set('score', 1);
    state.set('team', 'wolves');
    state.set('score', null);
    state.set('score', 2);
    state.remove('score');
    await f.host.services.emit('shared', 'teams:unwatch', []);
    state.set('score', 9);
    await f.host.services.emit('shared', 'teams:report', []);
    const report = JSON.parse(f.linesOf('teams').find((line) => line.startsWith('[')));
    assert.deepEqual(report, [
      'score:0->1',
      'any score',
      'any team',
      `score:1->${shown(language, 'null')}`,
      'any score',
      `score:${shown(language, 'null')}->2`,
      'any score',
      `score:2->${shown(language, 'undefined')}`,
      'any score',
      'any score',
    ]);

    // Booleans, numbers and strings stay themselves; objects come back as plain JSON copies.
    state.set('spawn', new f.host.api.Vector3(1, 2, 3));
    assert.deepEqual(state.get('spawn'), { x: 1, y: 2, z: 3 });
    assert.equal(state.get('spawn') instanceof f.host.api.Vector3, false);

    // Each client receives broadcast keys, owner keys only for its own entity, never server keys.
    const ownerView = f.host.stateStore.project(owner.id, true).map((entry) => entry.key);
    const otherView = f.host.stateStore.project(owner.id, false).map((entry) => entry.key);
    assert.deepEqual(ownerView.sort(), ['score', 'spawn', 'team', 'wanted']);
    assert.deepEqual(otherView.sort(), ['score', 'spawn', 'team']);
    assert.equal(state.get('joinedAt'), 1700000000000);

    // The owning client reads its own keys through LocalPlayer and cannot write.
    const client = clientFixture(t);
    await client.runtime.apply(
      {
        resources: [client.resource('teams', TEAMS_CLIENT, {}, 'teams', language)],
      },
      client.read,
    );
    client.runtime.entities.updateDirectory({
      localPlayerId: owner.id,
      entities: [directoryEntry(owner), directoryEntry(other)],
    });
    const deliver = () => {
      client.runtime.entities.applyState({
        id: owner.id,
        nativeId: owner.id,
        entries: f.host.stateStore.project(owner.id, true),
      });
    };
    deliver();
    await client.runtime.services.emit('shared', 'teams:check', []);
    state.set('team', 'ravens');
    deliver();
    assert.deepEqual(
      client.linesOf('teams').filter((line) => line !== 'client resource started'),
      [
        'my team is now wolves',
        'team wolves wanted 3',
        `client set ${shown(language, 'undefined')} remove ${shown(language, 'undefined')}`,
        'my team is now ravens',
      ],
    );

    // Narrowing a key's scope drops it from the clients that lose it.
    assert.equal(state.set('score', 9, { scope: 'owner' }), true);
    assert.equal(
      f.host.stateStore.project(owner.id, false).some((entry) => entry.key === 'score'),
      false,
    );
  });

  test(`state (${language}): keys belong to the entity; a reload drops subscriptions, a disconnect drops keys`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'teams', TEAMS, {}, language);
    await f.host.startAll();
    const player = f.report(73);
    await f.host.services.emit('shared', 'teams:watch', [73]);
    player.state.set('score', 5);
    const reloaded = { [language]: codeIn(TEAMS, language) + V2[language] };
    const content = entryContent('teams', reloaded, language);
    fs.writeFileSync(path.join(f.root, 'teams', ENTRY[language]), content);
    await f.host.ensure('teams');
    // The score kept in the bag survives the reload; the old onChange subscription is gone.
    assert.equal(player.state.get('score'), 5);
    player.state.set('score', 6);
    await f.host.services.emit('shared', 'teams:report', []);
    const reports = f.linesOf('teams').filter((line) => line.startsWith('['));
    assert.deepEqual(JSON.parse(reports[reports.length - 1]), []);
    // A disconnecting player takes the bag along.
    f.host.disconnect(73);
    assert.throws(() => player.state.get('score'), /no longer exists/);
  });

  test(`state (${language}): a write wakes onChange in the same tick on the server`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'sync', SYNC, {}, language);
    await f.host.startAll();
    f.report(74);
    assert.ok(f.linesOf('sync').includes('woke before set returned true'));
  });
}
