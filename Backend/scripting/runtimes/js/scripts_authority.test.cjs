'use strict';

// Who decides what, in resource scripts: entity snapshots, forced transforms, server-owned state
// and client requests. Each scenario runs with its scripts in JavaScript, in Lua and
// in C#.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  GLOBAL_WORLD,
  LANGUAGES,
  shown,
  serverResource,
  serverFixture,
  clientFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const WATCH = {
  javascript: `
    kcds.core.Events.on('playerConnect', (player) => {
      console.log('connect ready=' + player.ready + ' at ' + player.position.toString());
    });
    kcds.core.Events.on('playerSpawned', (player) => {
      console.log('spawned ready=' + player.ready + ' at ' + player.position.toString());
    });
    function nearEnough(a, b, metres) {
      if (!a.ready || !b.ready) {
        return false;
      }
      return a.position.distance(b.position) <= metres;
    }
    kcds.core.Events.on('watch:near', (a, b) => {
      console.log('near ' + nearEnough(kcds.players.Player.getById(a), kcds.players.Player.getById(b), 10));
    });
    kcds.core.Events.on('watch:lift', (id) => {
      const player = kcds.players.Player.getById(id);
      const above = player.position.clone();
      above.z += 20;
      player.position = above;
      console.log('assigned reads ' + player.position.z);
    });
    `,
  lua: `
    local Player = kcds.players.Player
    kcds.core.Events.on("playerConnect", function(player)
      print("connect ready=" .. tostring(player.ready) .. " at " .. player.position:toString())
    end)
    kcds.core.Events.on("playerSpawned", function(player)
      print("spawned ready=" .. tostring(player.ready) .. " at " .. player.position:toString())
    end)
    local function nearEnough(a, b, metres)
      if not a.ready or not b.ready then
        return false
      end
      return a.position:distance(b.position) <= metres
    end
    kcds.core.Events.on("watch:near", function(a, b)
      print("near " .. tostring(nearEnough(Player.getById(a), Player.getById(b), 10)))
    end)
    kcds.core.Events.on("watch:lift", function(id)
      local player = Player.getById(id)
      local above = player.position:clone()
      above.z = above.z + 20
      player.position = above
      print("assigned reads " .. player.position.z)
    end)
    `,
  csharp: `
public class Watch : Script
{
    private static string Ready(Player player)
    {
        return player.Ready ? "true" : "false";
    }

    private static bool NearEnough(Player a, Player b, double metres)
    {
        if (!a.Ready || !b.Ready)
        {
            return false;
        }
        return a.Position.Distance(b.Position) <= metres;
    }

    public override void OnStart()
    {
        Events.On("playerConnect", (Player player) =>
        {
            Log.Info("connect ready=" + Ready(player) + " at " + player.Position);
        });
        Events.On("playerSpawned", (Player player) =>
        {
            Log.Info("spawned ready=" + Ready(player) + " at " + player.Position);
        });
        Events.On("watch:near", (double a, double b) =>
        {
            var near = NearEnough(Player.GetById(a)!, Player.GetById(b)!, 10);
            Log.Info("near " + (near ? "true" : "false"));
        });
        Events.On("watch:lift", (double id) =>
        {
            var player = Player.GetById(id)!;
            var above = player.Position.Clone();
            above.Z += 20;
            player.Position = above;
            Log.Info("assigned reads " + player.Position.Z);
        });
    }
}
`,
};

const TEAMS = {
  javascript: `
    const TEAMS = new Set(['ravens', 'wolves']);
    kcds.core.Events.fromClient('teams:join', (sender, payload) => {
      const player = sender;
      const team = typeof payload === 'object' && payload !== null ? payload.team : undefined;
      if (typeof team !== 'string' || !TEAMS.has(team)) {
        console.log('refused ' + player.id);
        return;
      }
      player.state.set('team', team);
      console.log('joined ' + player.id + ' ' + player.state.get('team'));
    });
    `,
  lua: `
    local TEAMS = { ravens = true, wolves = true }
    kcds.core.Events.fromClient("teams:join", function(sender, payload)
      local player = sender
      local team = type(payload) == "userdata" and payload.team or nil
      if type(team) ~= "string" or not TEAMS[team] then
        print("refused " .. player.id)
        return
      end
      player.state:set("team", team)
      print("joined " .. player.id .. " " .. player.state:get("team"))
    end)
    `,
  csharp: `
public class Teams : Script
{
    private static readonly HashSet<string> Names = new() { "ravens", "wolves" };

    public override void OnStart()
    {
        Events.FromClient("teams:join", (Player sender, JsValue payload) =>
        {
            var player = sender;
            var team = payload["team"];
            if (team.Type != "string" || !Names.Contains(team.AsString()!))
            {
                Log.Info("refused " + player.Id);
                return;
            }
            player.State.Set("team", team);
            Log.Info("joined " + player.Id + " " + player.State.Get("team"));
        });
    }
}
`,
};

const TEAMS_CLIENT = {
  javascript: `
          kcds.core.Events.on('teams:try', (otherId) => {
            const me = kcds.players.LocalPlayer;
            console.log('team ' + me.state.get('team') + ' set ' + typeof me.state.set);
            try {
              new kcds.players.Entity(otherId).position = { x: 1, y: 1, z: 1 };
            } catch (error) {
              console.log('other ' + error.message);
            }
            me.position = { x: 2, y: 2, z: 2 };
          });
          `,
  lua: `
          kcds.core.Events.on("teams:try", function(otherId)
            local me = kcds.players.LocalPlayer
            print("team " .. me.state:get("team") .. " set " .. type(me.state.set))
            local ok, problem = pcall(function()
              kcds.players.Entity.new(otherId).position = { x = 1, y = 1, z = 1 }
            end)
            if not ok then
              print("other " .. problem.message)
            end
            me.position = { x = 2, y = 2, z = 2 }
          end)
          `,
  csharp: `
public class TeamsClient : Script
{
    public override void OnStart()
    {
        Events.On("teams:try", (double otherId) =>
        {
            var me = LocalPlayer.Current!;
            Log.Info("team " + me.State.Get("team") + " set " + me.State.AsValue()["set"]);
            try
            {
                new Entity(otherId).Position = new Vector3(1, 1, 1);
            }
            catch (JsError problem)
            {
                Log.Info("other " + problem.Message);
            }
            me.Position = new Vector3(2, 2, 2);
        });
    }
}
`,
};

for (const language of LANGUAGES) {
  test(`authority (${language}): a player is a snapshot of the last native report`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'watch', WATCH, {}, language);
    await f.host.startAll();
    // Until the first pose arrives the position is the world origin and ready is false.
    const henry = f.host.updatePlayer({ id: 101, ready: false });
    f.report(102, { position: { x: 3, y: 0, z: 0 } });
    await f.host.services.emit('shared', 'watch:near', [101, 102]);
    f.report(101, { position: { x: 0, y: 0, z: 0 } });
    await f.host.services.emit('shared', 'watch:near', [101, 102]);
    assert.deepEqual(
      f.linesOf('watch').filter((line) => line !== 'started'),
      [
        'connect ready=false at Vector3(0, 0, 0)',
        'connect ready=true at Vector3(3, 0, 0)',
        'spawned ready=true at Vector3(3, 0, 0)',
        'near false',
        'spawned ready=true at Vector3(0, 0, 0)',
        'near true',
      ],
    );
    // Assigning the position asks the native server to move the player ...
    await f.host.services.emit('shared', 'watch:lift', [101]);
    assert.ok(f.linesOf('watch').includes('assigned reads 20'), f.linesOf('watch').join('\n'));
    const moves = f.sent.filter((message) => message.op === 'entityPosition');
    assert.deepEqual(moves, [{ op: 'entityPosition', id: 101, position: { x: 0, y: 0, z: 20 } }]);
    // ... and the next report from the owner is what the server reads afterwards.
    f.report(101, { position: { x: 0, y: 0, z: 0.5 } });
    assert.equal(henry.position.z, 0.5);
  });

  test(`authority (${language}): clients ask, the server decides and writes the state it owns`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'teams', TEAMS, {}, language);
    await f.host.startAll();
    const henry = f.report(103);
    await f.host.services.emit('client', 'teams:join', [henry, { team: 'pirates' }]);
    await f.host.services.emit('client', 'teams:join', [henry, { team: 'wolves', id: 999 }]);
    assert.deepEqual(
      f.linesOf('teams').filter((line) => line !== 'started'),
      ['refused 103', 'joined 103 wolves'],
    );

    // The client reads the state and may only move its own player; it never writes state.
    const client = clientFixture(t);
    await client.runtime.apply(
      { resources: [client.resource('teams', TEAMS_CLIENT, {}, 'teams', language)] },
      client.read,
    );
    const entry = (id) => {
      return {
        id,
        nativeId: id,
        kind: 'player',
        dimension: GLOBAL_WORLD,
        position: { x: 0, y: 0, z: 0 },
        rotation: { w: 1, x: 0, y: 0, z: 0 },
      };
    };
    client.runtime.entities.updateDirectory({
      localPlayerId: 103,
      entities: [entry(103), entry(104)],
    });
    client.runtime.entities.applyState({
      id: 103,
      nativeId: 103,
      entries: f.host.stateStore.project(103, true),
    });
    await client.runtime.services.emit('shared', 'teams:try', [104]);
    const missing = shown(language, 'undefined');
    assert.deepEqual(
      client.linesOf('teams').filter((line) => line !== 'client resource started'),
      [`team wolves set ${missing}`, 'other A client script can only move the local player'],
    );
    assert.deepEqual(
      client.sent.filter((message) => message.op === 'entityTransform'),
      [{ op: 'entityTransform', id: 103, field: 'position', x: 2, y: 2, z: 2 }],
    );
  });
}
