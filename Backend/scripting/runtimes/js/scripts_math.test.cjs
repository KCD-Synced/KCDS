'use strict';

// Math in a resource script: placing a horse a few metres in front of a player, turned back
// towards them, and how vectors and quaternions behave. Each scenario runs with its scripts in
// JavaScript, in Lua and in C#.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  LANGUAGES,
  serverResource,
  serverFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const PLACEMENT = {};
const VECTORS = {};

PLACEMENT.lua = `
local Vector3, Quaternion = kcds.math.Vector3, kcds.math.Quaternion

-- Facing of a rotation, flattened onto the ground and normalised.
local function flatFacing(rotation)
  local north = Vector3.new(0, 1, 0)
  local pointing = rotation:rotateVector(north)
  local size = math.sqrt(pointing.x * pointing.x + pointing.y * pointing.y)
  if size < 1e-4 then
    return north
  end
  return Vector3.new(pointing.x / size, pointing.y / size, 0)
end

-- Yaw-only rotation whose forward (+Y) points along the ground direction (dx, dy).
local function headingTo(dx, dy)
  return Quaternion.fromAxisAngle(Vector3.new(0, 0, 1), math.atan(-dx, dy))
end

local function placeAhead(player, metres, lift)
  lift = lift or 0
  local ahead = flatFacing(player.rotation)
  local from = player.position
  return {
    position = Vector3.new(from.x + ahead.x * metres, from.y + ahead.y * metres, from.z + lift),
    rotation = headingTo(-ahead.x, -ahead.y),
  }
end

local function nearOnGround(a, b, metres)
  local dx, dy = a.x - b.x, a.y - b.y
  return dx * dx + dy * dy <= metres * metres
end

-- Three decimals, a whole number without its ".0".
local function round(value)
  local rounded = math.floor(value * 1000 + 0.5) / 1000 + 0.0
  if rounded == math.floor(rounded) then
    return string.format("%d", rounded)
  end
  return tostring(rounded)
end
local function show(v)
  return round(v.x) .. "," .. round(v.y) .. "," .. round(v.z)
end

kcds.core.Events.on("playerCommand", function(player, command)
  if command == "where" then
    local p = player.position
    kcds.core.Chat.sendToPlayer(player, string.format("%.1f, %.1f, height %.1f", p.x, p.y, p.z))
    return
  end
  if command == "horse" then
    if not player.ready then
      print("not ready")
      return
    end
    local spot = placeAhead(player, 3)
    local horse = kcds.characters.Horse.spawn(spot.position, spot.rotation)
    local back = horse.rotation:rotateVector(Vector3.new(0, 1, 0))
    print("horse at " .. show(horse.position) .. " facing " .. show(back))
    return
  end
  if command == "near" then
    local other
    for _, online in ipairs(kcds.players.Player.all()) do
      if online.id ~= player.id then
        other = online
        break
      end
    end
    print("ground " .. tostring(nearOnGround(player.position, other.position, 5))
      .. " straight " .. tostring(player.position:distance(other.position) <= 5))
  end
end)
`;

VECTORS.lua = `
local Vector3, Quaternion = kcds.math.Vector3, kcds.math.Quaternion
local function round(value)
  return math.floor(value * 1000 + 0.5) / 1000 + 0.0
end
local results = {}
local origin = Vector3.new(1, 2, 3)
local ahead = origin:clone():add(Vector3.new(0, 5, 0))
results.cloneKeepsOriginal = origin.y == 2 and ahead.y == 7
local v = Vector3.new(3, 4, 0)
results.inPlace = v:mul(2) == v and v.x == 6
results.numbers = type(v:dot(origin)) == "number" and v:distance(origin) > 0
-- length has no setter: the write fails and changes nothing.
pcall(function() v.length = 1 end)
results.lengthReadOnly = v.length == 10 and v.lengthSquared == 100
results.genericAxes = Vector3.up():toString() .. " " .. Vector3.forward():toString()
local q = Quaternion.new(1, 0, 0, 0)
results.identity = q:toString() == Quaternion.identity():toString()
local quarter = Quaternion.fromAxisAngle(Vector3.new(0, 0, 1), math.pi / 2)
local facing = quarter:rotateVector(Vector3.new(0, 1, 0))
results.facing = { round(facing.x), round(facing.y), round(facing.z) }
results.heading = round(quarter:toEuler().z)
-- fromEuler assumes Y is up; in this game Y is horizontal, so its yaw leaves +Y unchanged.
local yaw = Quaternion.fromEuler(0, math.pi / 2, 0):rotateVector(Vector3.new(0, 1, 0))
results.yawKeepsY = { round(yaw.x), round(yaw.y), round(yaw.z) }
local blend = Quaternion.identity()
results.slerpInPlace = blend:slerp(quarter, 0.5) == blend and blend:mul(quarter) == blend
-- Any {x, y, z} table is accepted as a position; Euler angles are degrees.
local horse = kcds.characters.Horse.spawn({ x = 4, y = 5, z = 6 }, Vector3.new(0, 0, 90))
results.horsePosition = type(horse.position.clone) == "function" and horse.position.x == 4
results.horseRotation = type(horse.rotation.slerp) == "function"
local horseFacing = horse.rotation:rotateVector(Vector3.new(0, 1, 0))
results.horseFacing = { round(horseFacing.x), round(horseFacing.y) }
print(json.encode(results))
`;

PLACEMENT.javascript = `
// Facing of a rotation, flattened onto the ground and normalised.
function flatFacing(rotation) {
  const north = new kcds.math.Vector3(0, 1, 0);
  const pointing = rotation.rotateVector(north);
  const size = Math.hypot(pointing.x, pointing.y);
  if (size < 1e-4) {
    return north;
  }
  return new kcds.math.Vector3(pointing.x / size, pointing.y / size, 0);
}

// Yaw-only rotation whose forward (+Y) points along the ground direction (dx, dy).
function headingTo(dx, dy) {
  return kcds.math.Quaternion.fromAxisAngle(new kcds.math.Vector3(0, 0, 1), Math.atan2(-dx, dy));
}

function placeAhead(player, metres, lift = 0) {
  const ahead = flatFacing(player.rotation);
  const from = player.position;
  return {
    position: new kcds.math.Vector3(from.x + ahead.x * metres, from.y + ahead.y * metres, from.z + lift),
    rotation: headingTo(-ahead.x, -ahead.y),
  };
}

function nearOnGround(a, b, metres) {
  const dx = a.x - b.x;
  const dy = a.y - b.y;
  return dx * dx + dy * dy <= metres * metres;
}

const round = (value) => Math.round(value * 1000) / 1000 + 0;
const show = (v) => [v.x, v.y, v.z].map(round).join(',');

kcds.core.Events.on('playerCommand', (player, command) => {
  if (command === 'where') {
    const { x, y, z } = player.position;
    kcds.core.Chat.sendToPlayer(player, x.toFixed(1) + ', ' + y.toFixed(1) + ', height ' + z.toFixed(1));
    return;
  }
  if (command === 'horse') {
    if (!player.ready) {
      console.log('not ready');
      return;
    }
    const spot = placeAhead(player, 3);
    const horse = kcds.characters.Horse.spawn(spot.position, spot.rotation);
    const back = horse.rotation.rotateVector(new kcds.math.Vector3(0, 1, 0));
    console.log('horse at ' + show(horse.position) + ' facing ' + show(back));
    return;
  }
  if (command === 'near') {
    const other = kcds.players.Player.all().find((p) => p.id !== player.id);
    console.log('ground ' + nearOnGround(player.position, other.position, 5) +
      ' straight ' + (player.position.distance(other.position) <= 5));
  }
});
`;

VECTORS.javascript = `
    const results = {};
    const origin = new kcds.math.Vector3(1, 2, 3);
    const ahead = origin.clone().add(new kcds.math.Vector3(0, 5, 0));
    results.cloneKeepsOriginal = origin.y === 2 && ahead.y === 7;
    const v = new kcds.math.Vector3(3, 4, 0);
    results.inPlace = v.mul(2) === v && v.x === 6;
    results.numbers = typeof v.dot(origin) === 'number' && v.distance(origin) > 0;
    v.length = 1;
    results.lengthReadOnly = v.length === 10 && v.lengthSquared === 100;
    results.genericAxes = kcds.math.Vector3.up().toString() + ' ' + kcds.math.Vector3.forward().toString();
    const q = new kcds.math.Quaternion(1, 0, 0, 0);
    results.identity = q.toString() === kcds.math.Quaternion.identity().toString();
    const quarter = kcds.math.Quaternion.fromAxisAngle(new kcds.math.Vector3(0, 0, 1), Math.PI / 2);
    const facing = quarter.rotateVector(new kcds.math.Vector3(0, 1, 0));
    results.facing = [facing.x, facing.y, facing.z].map((c) => Math.round(c * 1000) / 1000 + 0);
    results.heading = Math.round(quarter.toEuler().z * 1000) / 1000;
    // fromEuler assumes Y is up; in this game Y is horizontal, so its yaw leaves +Y unchanged.
    const yaw = kcds.math.Quaternion.fromEuler(0, Math.PI / 2, 0).rotateVector(new kcds.math.Vector3(0, 1, 0));
    results.yawKeepsY = [yaw.x, yaw.y, yaw.z].map((c) => Math.round(c * 1000) / 1000 + 0);
    const blend = kcds.math.Quaternion.identity();
    results.slerpInPlace = blend.slerp(quarter, 0.5) === blend && blend.mul(quarter) === blend;
    // Any {x, y, z} object is accepted as a position; Euler angles are degrees.
    const horse = kcds.characters.Horse.spawn({ x: 4, y: 5, z: 6 }, new kcds.math.Vector3(0, 0, 90));
    results.horsePosition = horse.position instanceof kcds.math.Vector3 && horse.position.x === 4;
    results.horseRotation = horse.rotation instanceof kcds.math.Quaternion;
    const horseFacing = horse.rotation.rotateVector(new kcds.math.Vector3(0, 1, 0));
    results.horseFacing = [horseFacing.x, horseFacing.y].map((c) => Math.round(c * 1000) / 1000 + 0);
    console.log(JSON.stringify(results));
    `;

PLACEMENT.csharp = `
using System.Globalization;

public class Stable : Script
{
    // Facing of a rotation, flattened onto the ground and normalised.
    private static Vector3 FlatFacing(Quaternion rotation)
    {
        var north = new Vector3(0, 1, 0);
        var pointing = rotation.RotateVector(north);
        var size = Math.Sqrt(pointing.X * pointing.X + pointing.Y * pointing.Y);
        if (size < 1e-4)
        {
            return north;
        }
        return new Vector3(pointing.X / size, pointing.Y / size, 0);
    }

    // Yaw-only rotation whose forward (+Y) points along the ground direction (dx, dy).
    private static Quaternion HeadingTo(double dx, double dy)
    {
        return Quaternion.FromAxisAngle(new Vector3(0, 0, 1), Math.Atan2(-dx, dy));
    }

    private static (Vector3 Position, Quaternion Rotation) PlaceAhead(Player player, double metres, double lift = 0)
    {
        var ahead = FlatFacing(player.Rotation);
        var from = player.Position;
        var position = new Vector3(from.X + ahead.X * metres, from.Y + ahead.Y * metres, from.Z + lift);
        return (position, HeadingTo(-ahead.X, -ahead.Y));
    }

    private static bool NearOnGround(Vector3 a, Vector3 b, double metres)
    {
        var dx = a.X - b.X;
        var dy = a.Y - b.Y;
        return dx * dx + dy * dy <= metres * metres;
    }

    // Three decimals, a whole number without its ".0".
    private static string Round(double value)
    {
        return (Math.Round(value * 1000) / 1000 + 0.0).ToString(CultureInfo.InvariantCulture);
    }

    private static string Show(Vector3 v)
    {
        return Round(v.X) + "," + Round(v.Y) + "," + Round(v.Z);
    }

    public override void OnStart()
    {
        Events.On("playerCommand", (Player player, string command) =>
        {
            if (command == "where")
            {
                var p = player.Position;
                var text = string.Format(CultureInfo.InvariantCulture, "{0:F1}, {1:F1}, height {2:F1}", p.X, p.Y, p.Z);
                Chat.SendToPlayer(player, text);
                return;
            }
            if (command == "horse")
            {
                if (!player.Ready)
                {
                    Log.Info("not ready");
                    return;
                }
                var spot = PlaceAhead(player, 3);
                var horse = Horse.Spawn(spot.Position, spot.Rotation);
                var back = horse.Rotation.RotateVector(new Vector3(0, 1, 0));
                Log.Info("horse at " + Show(horse.Position) + " facing " + Show(back));
                return;
            }
            if (command == "near")
            {
                var other = Player.All().First((online) => online.Id != player.Id);
                var ground = NearOnGround(player.Position, other.Position, 5);
                var straight = player.Position.Distance(other.Position) <= 5;
                Log.Info("ground " + (ground ? "true" : "false") + " straight " + (straight ? "true" : "false"));
            }
        });
    }
}
`;

VECTORS.csharp = `
public class Vectors : Script
{
    private static double Round(double value)
    {
        return Math.Round(value * 1000) / 1000 + 0.0;
    }

    public override void OnStart()
    {
        var results = new Dictionary<string, object>();
        var origin = new Vector3(1, 2, 3);
        var ahead = origin.Clone().Add(new Vector3(0, 5, 0));
        results["cloneKeepsOriginal"] = origin.Y == 2 && ahead.Y == 7;
        var v = new Vector3(3, 4, 0);
        results["inPlace"] = v.Mul(2) == v && v.X == 6;
        results["numbers"] = v.Dot(origin) is double && v.Distance(origin) > 0;
        // Length has no setter: writing it does not compile.
        var lengthWritable = typeof(Vector3).GetProperty("Length")!.CanWrite;
        results["lengthReadOnly"] = !lengthWritable && v.Length == 10 && v.LengthSquared == 100;
        results["genericAxes"] = Vector3.Up() + " " + Vector3.Forward();
        var q = new Quaternion(1, 0, 0, 0);
        results["identity"] = q.ToString() == Quaternion.Identity().ToString();
        var quarter = Quaternion.FromAxisAngle(new Vector3(0, 0, 1), Math.PI / 2);
        var facing = quarter.RotateVector(new Vector3(0, 1, 0));
        results["facing"] = new[] { Round(facing.X), Round(facing.Y), Round(facing.Z) };
        results["heading"] = Round(quarter.ToEuler().Z);
        // FromEuler assumes Y is up; in this game Y is horizontal, so its yaw leaves +Y unchanged.
        var yaw = Quaternion.FromEuler(0, Math.PI / 2, 0).RotateVector(new Vector3(0, 1, 0));
        results["yawKeepsY"] = new[] { Round(yaw.X), Round(yaw.Y), Round(yaw.Z) };
        var blend = Quaternion.Identity();
        results["slerpInPlace"] = blend.Slerp(quarter, 0.5) == blend && blend.Mul(quarter) == blend;
        // Any { X, Y, Z } object is accepted as a position; Euler angles are degrees.
        var horse = Horse.Spawn(new { X = 4, Y = 5, Z = 6 }, new Vector3(0, 0, 90));
        results["horsePosition"] = horse.Position is Vector3 && horse.Position.X == 4;
        results["horseRotation"] = horse.Rotation is Quaternion;
        var horseFacing = horse.Rotation.RotateVector(new Vector3(0, 1, 0));
        results["horseFacing"] = new[] { Round(horseFacing.X), Round(horseFacing.Y) };
        Log.Info(System.Text.Json.JsonSerializer.Serialize(results));
    }
}
`;

for (const language of LANGUAGES) {
  test(`math (${language}): place a horse in front of a player and turn it back towards them`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'stable', PLACEMENT, {}, language);
    await f.host.startAll();
    const waiting = f.report(81, { ready: false });
    // Chat from a player who is not ready is dropped by the host, so raise the command directly.
    await f.host.services.emit('shared', 'playerCommand', [waiting, 'horse', []]);
    assert.equal(f.host.horses.size, 0);
    // The player turned 90 degrees counterclockwise from +Y (Euler degrees about Z): facing -X.
    f.report(81, { position: { x: 100, y: 200, z: 30 } });
    waiting.rotation = new f.host.api.Vector3(0, 0, 90);
    f.host.receiveChat(81, '/where');
    f.host.receiveChat(81, '/horse');
    const chat = f.sent.filter((message) => message.op === 'chat').map((message) => message.text);
    assert.deepEqual(chat, ['100.0, 200.0, height 30.0']);
    assert.ok(f.linesOf('stable').includes('not ready'));
    assert.ok(f.linesOf('stable').includes('horse at 97,200,30 facing 1,0,0'));
    // Looking up does not shorten the ground facing.
    waiting.rotation = { x: 60, y: 0, z: 90 };
    f.host.receiveChat(81, '/horse');
    const placed = f.linesOf('stable').filter((line) => line.startsWith('horse at '));
    assert.deepEqual(placed, [
      'horse at 97,200,30 facing 1,0,0',
      'horse at 97,200,30 facing 1,0,0',
    ]);
    const spawns = f.sent.filter((message) => message.op === 'horseSpawn');
    assert.equal(spawns.length, 2);
    // Ground distance ignores height: a player on a roof is still near.
    f.report(82, { position: { x: 102, y: 201, z: 45 } });
    f.host.receiveChat(81, '/near');
    assert.ok(f.linesOf('stable').includes('ground true straight false'));
  });

  test(`math (${language}): vectors change in place, rotations take quaternions or Euler degrees`, async (t) => {
    const f = serverFixture(t);
    serverResource(f.root, 'vectors', VECTORS, {}, language);
    await f.host.startAll();
    const results = JSON.parse(f.linesOf('vectors').find((line) => line.startsWith('{')));
    assert.deepEqual(results, {
      cloneKeepsOriginal: true,
      inPlace: true,
      numbers: true,
      lengthReadOnly: true,
      genericAxes: 'Vector3(0, 1, 0) Vector3(0, 0, 1)',
      identity: true,
      facing: [-1, 0, 0],
      heading: 1.571,
      yawKeepsY: [0, 1, 0],
      slerpInPlace: true,
      horsePosition: true,
      horseRotation: true,
      horseFacing: [-1, 0],
    });
  });
}
