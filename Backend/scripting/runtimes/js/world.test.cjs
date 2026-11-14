'use strict';

// The world clock and weather: the time the server keeps, the setters, the ops sent
// to the players' games and the events.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

// A started host whose world clock runs on a time the test moves by hand.
async function started(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const state = f.host.worldState;
  const real = { ms: 1000000 };
  state.now = () => real.ms;
  state.clock.now = state.now;
  state.clock.at = real.ms;
  state.lastSent = real.ms;
  state.weather.wetAt = state.clock.seconds();
  const events = [];
  f.host.on('worldDayChange', (day) => events.push(['day', day]));
  f.host.on('worldWeatherChange', (preset, previous, seconds) =>
    events.push(['weather', preset, previous, seconds]),
  );
  const ops = (...names) => f.sent.filter((message) => names.includes(message.op));
  // Moves the real time on by this many seconds; the timer's wake-up is run by hand.
  const advance = (seconds) => {
    real.ms += seconds * 1000;
  };
  return { ...f, state, events, ops, advance, World: f.host.api.World };
}

test('a new server starts at nine in the morning of day 0 at the game pace', async (t) => {
  const { World } = await started(t);
  assert.equal(World.day, 0);
  assert.equal(World.hour, 9);
  assert.equal(World.timeScale, 15);
  assert.equal(World.weather, 'cloudless_sunny');
  assert.equal(World.lastWeather, 'cloudless_sunny');
  assert.equal(World.weatherFade, false);
  assert.equal(World.weatherLeft, 0);
  assert.equal(World.rainStrength, 0);
  assert.equal(World.rainLevel, 1);
  assert.equal(World.windSpeed, 0);
  assert.equal(World.wetness, 0);
  assert.equal(World.puddles, 0);
  assert.equal(typeof World.addInteraction, 'function');
});

test('the clock runs from the real time and is read when asked', async (t) => {
  const { World, advance } = await started(t);
  // 60 real seconds at 15 game seconds each is a quarter of an hour
  advance(60);
  assert.equal(World.hour, 9.25);
  // A further 3000 real seconds is 12.5 game hours, which makes it 21:45 on day 0
  advance(3000);
  assert.equal(World.day, 0);
  assert.equal(World.hour, 21.75);
});

test('setTimeScale changes the speed from where the clock stands and 0 freezes it', async (t) => {
  const { World, advance } = await started(t);
  advance(60);
  assert.equal(World.setTimeScale(60), true);
  assert.equal(World.timeScale, 60);
  assert.equal(World.hour, 9.25);
  advance(60);
  assert.equal(World.hour, 10.25);
  assert.equal(World.setTimeScale(0), true);
  advance(1000);
  assert.equal(World.hour, 10.25);
  assert.equal(World.setTimeScale(-1), false);
  assert.equal(World.setTimeScale(201), false);
  assert.equal(World.setTimeScale(NaN), false);
  assert.equal(World.setTimeScale('15'), false);
  assert.equal(World.timeScale, 0);
});

test('setHour winds to the next such hour, into tomorrow when it has passed', async (t) => {
  const { World, events } = await started(t);
  assert.equal(World.setHour(12.5), true);
  assert.equal(World.hour, 12.5);
  assert.equal(World.day, 0);
  assert.equal(World.setHour(6), true);
  assert.equal(World.hour, 6);
  assert.equal(World.day, 1);
  assert.equal(World.setHour(24), false);
  assert.equal(World.setHour(-1), false);
  assert.equal(World.setHour('6'), false);
  assert.deepEqual(events, [['day', 1]]);
});

test('setTime restores an absolute time and never goes back', async (t) => {
  const { World, events } = await started(t);
  assert.equal(World.setTime(3, 7.75), true);
  assert.equal(World.day, 3);
  assert.equal(World.hour, 7.75);
  // One event for the jump, carrying the day that began
  assert.deepEqual(events, [['day', 3]]);
  assert.equal(World.setTime(3, 7), false);
  assert.equal(World.setTime(2, 20), false);
  assert.equal(World.setTime(4, 24), false);
  assert.equal(World.setTime(4.5, 1), false);
  assert.equal(World.setTime(-1, 1), false);
  assert.equal(World.setTime(5, 0), true);
  assert.deepEqual(events, [
    ['day', 3],
    ['day', 5],
  ]);
  // Within the day nothing is raised
  assert.equal(World.setTime(5, 3), true);
  assert.equal(events.length, 2);
});

test('the day changes as the clock walks over midnight, noticed by the timer', async (t) => {
  const { state, World, events, advance } = await started(t);
  assert.notEqual(state.timer, null);
  // 15 hours to midnight at 15 game seconds per real second is 3600 real seconds
  advance(3600);
  assert.equal(World.day, 1);
  assert.equal(World.hour, 0);
  state.wake();
  assert.deepEqual(events, [['day', 1]]);
  state.wake();
  assert.deepEqual(events, [['day', 1]]);
});

test('the timer is set for the next midnight at the latest, never per tick', async (t) => {
  const { state, World } = await started(t);
  const delays = [];
  const original = global.setTimeout;
  global.setTimeout = (fn, delay, ...rest) => {
    delays.push(delay);
    return original(fn, delay, ...rest);
  };
  t.after(() => {
    global.setTimeout = original;
  });
  state.schedule();
  // The correction is due in 30 s, long before the midnight
  assert.deepEqual(delays, [30000]);
  World.setTime(0, 23.999);
  // A game second per... about 3.6 game seconds to midnight at 15 is under a second
  assert.equal(delays.length, 2);
  assert.equal(delays[1] >= 250 && delays[1] <= 30000, true);
  World.setTimeScale(0);
  assert.equal(state.timer, null);
});

test('players hear the time and weather when their world is ready, and nobody before', async (t) => {
  const { report, ops } = await started(t);
  report(1);
  assert.deepEqual(ops('worldTime'), [{ op: 'worldTime', id: 1, day: 0, hour: 9, timeScale: 15 }]);
  assert.deepEqual(ops('worldWeather'), [
    {
      op: 'worldWeather',
      id: 1,
      preset: 'cloudless_sunny',
      previous: 'cloudless_sunny',
      seconds: 0,
      remaining: 0,
      rainStrength: 0,
      rainLevel: 1,
      wind: { x: 0, y: 0, z: 0 },
      snow: { amount: 0, frost: 0, freezing: 0, flakes: 0, flakeSize: 1, storm: 0 },
      fog: 0,
    },
  ]);
  // A player whose world is not ready is sent nothing
  report(2, { ready: false });
  assert.equal(ops('worldTime').length, 1);
});

test('a player who becomes ready gets the weather once more a few seconds later', async (t) => {
  const { report, ops, host } = await started(t);
  const weatherTo = (id) => ops('worldWeather').filter((message) => message.id === id).length;
  t.mock.timers.enable({ apis: ['setTimeout'] });
  report(1);
  assert.equal(weatherTo(1), 1);
  t.mock.timers.tick(3999);
  assert.equal(weatherTo(1), 1);
  t.mock.timers.tick(1);
  assert.equal(weatherTo(1), 2);
  assert.deepEqual(ops('worldWeather')[1], ops('worldWeather')[0]);
  // Not sent again to a player who is no longer ready or has left by then.
  report(2);
  report(2, { ready: false });
  host.players.delete(1);
  t.mock.timers.tick(4000);
  assert.equal(weatherTo(1), 2);
  assert.equal(weatherTo(2), 1);
  // Nor after the world stops.
  report(3);
  host.worldState.stop();
  t.mock.timers.tick(4000);
  assert.equal(weatherTo(3), 1);
});

test('a change of time is sent to every ready player once', async (t) => {
  const { report, ops, World, sent } = await started(t);
  report(1);
  report(2);
  report(3, { ready: false });
  sent.length = 0;
  World.setHour(15);
  assert.deepEqual(
    ops('worldTime').map((op) => [op.id, op.hour]),
    [
      [1, 15],
      [2, 15],
    ],
  );
  sent.length = 0;
  // A refused change tells nobody
  World.setHour(99);
  World.setTimeScale(-3);
  assert.deepEqual(sent, []);
  World.setTimeScale(30);
  assert.deepEqual(
    ops('worldTime').map((op) => [op.id, op.timeScale]),
    [
      [1, 30],
      [2, 30],
    ],
  );
});

test('a running clock is restated at most every 30 seconds and a frozen one never', async (t) => {
  const { report, ops, state, advance, sent } = await started(t);
  report(1);
  sent.length = 0;
  advance(10);
  state.wake();
  advance(10);
  state.wake();
  assert.deepEqual(sent, []);
  advance(10);
  state.wake();
  assert.equal(ops('worldTime').length, 1);
  assert.equal(ops('worldTime')[0].hour, 9 + (30 * 15) / 3600);
  advance(20);
  state.wake();
  assert.equal(ops('worldTime').length, 1);
  advance(10);
  state.wake();
  assert.equal(ops('worldTime').length, 2);
  state.clock.setScale(0);
  state.schedule();
  assert.equal(state.timer, null);
});

test('setWeather blends, raises the event and refuses a second blend meanwhile', async (t) => {
  const { World, events, report, ops, sent, advance } = await started(t);
  report(1);
  sent.length = 0;
  assert.equal(World.setWeather('overcast_rain', 600), true);
  assert.equal(World.weather, 'overcast_rain');
  assert.equal(World.lastWeather, 'cloudless_sunny');
  assert.equal(World.weatherFade, true);
  assert.equal(World.weatherLeft, 600);
  assert.deepEqual(events, [['weather', 'overcast_rain', 'cloudless_sunny', 600]]);
  const op = ops('worldWeather')[0];
  assert.equal(op.preset, 'overcast_rain');
  assert.equal(op.previous, 'cloudless_sunny');
  assert.equal(op.seconds, 600);
  assert.equal(op.remaining, 600);
  assert.equal(World.setWeather('storm', 10), false);
  // 20 real seconds at 15 is 300 game seconds
  advance(20);
  assert.equal(World.weatherLeft, 300);
  assert.equal(World.setWeather('storm'), false);
  advance(20);
  assert.equal(World.weatherFade, false);
  assert.equal(World.lastWeather, 'overcast_rain');
  assert.equal(World.setWeather('storm'), true);
  assert.equal(World.weather, 'storm');
  assert.equal(World.weatherFade, false);
  assert.deepEqual(events[1], ['weather', 'storm', 'overcast_rain', 0]);
  assert.equal(events.length, 2);
});

test('setWeather refuses names and lengths out of range', async (t) => {
  const { World, events } = await started(t);
  assert.equal(World.setWeather(''), false);
  assert.equal(World.setWeather('has space'), false);
  assert.equal(World.setWeather(7), false);
  assert.equal(World.setWeather('x'.repeat(65)), false);
  assert.equal(World.setWeather('storm', -1), false);
  assert.equal(World.setWeather('storm', 21601), false);
  assert.equal(World.setWeather('storm', NaN), false);
  assert.equal(World.weather, 'cloudless_sunny');
  assert.deepEqual(events, []);
  assert.equal(World.setWeather('storm', 21600), true);
});

test('a frozen clock holds a blend and winding past its end settles it', async (t) => {
  const { World, advance } = await started(t);
  World.setWeather('fog', 3600);
  World.setTimeScale(0);
  advance(1000);
  assert.equal(World.weatherLeft, 3600);
  World.setTimeScale(15);
  World.setHour(11);
  assert.equal(World.weatherFade, false);
});

test('setSnow takes its parts in range, tells every ready player once per change', async (t) => {
  const { World, report, ops, sent } = await started(t);
  report(1);
  sent.length = 0;
  const calm = { flakeSize: 1, storm: 0 };
  assert.deepEqual(World.snow, { amount: 0, frost: 0, freezing: 0, flakes: 0, ...calm });
  assert.equal(World.setSnow({ amount: 11 }), false);
  assert.equal(World.setSnow({ frost: 2 }), false);
  assert.equal(World.setSnow({ flakes: 1.5 }), false);
  assert.equal(World.setSnow({ flakeSize: 0 }), false);
  assert.equal(World.setSnow({ flakeSize: 6 }), false);
  assert.equal(World.setSnow({ storm: 1.5 }), false);
  assert.equal(World.setSnow(null), false);
  assert.deepEqual(sent, []);
  assert.equal(World.setSnow({ amount: 3, frost: 0.4, flakes: 200 }), true);
  assert.deepEqual(World.snow, { amount: 3, frost: 0.4, freezing: 0, flakes: 200, ...calm });
  assert.equal(ops('worldWeather').length, 1);
  assert.deepEqual(ops('worldWeather')[0].snow, {
    amount: 3,
    frost: 0.4,
    freezing: 0,
    flakes: 200,
    ...calm,
  });
  // A blizzard: big flakes the storm drives.
  assert.equal(World.setSnow({ amount: 3, flakes: 10000, flakeSize: 2.5, storm: 1 }), true);
  assert.equal(ops('worldWeather').at(-1).snow.flakeSize, 2.5);
  assert.equal(ops('worldWeather').at(-1).snow.storm, 1);
  assert.equal(World.setSnow({ amount: 3, frost: 0.4, flakes: 200 }), true);
  const before = ops('worldWeather').length;
  // The same snow again is no change: nothing goes out.
  assert.equal(World.setSnow({ amount: 3, frost: 0.4, flakes: 200 }), true);
  assert.equal(ops('worldWeather').length, before);
  // A player who arrives later hears it with the weather.
  report(2);
  assert.deepEqual(ops('worldWeather').at(-1).snow, World.snow);
  // {} melts it.
  assert.equal(World.setSnow({}), true);
  assert.deepEqual(ops('worldWeather').at(-1).snow, {
    amount: 0,
    frost: 0,
    freezing: 0,
    flakes: 0,
    flakeSize: 1,
    storm: 0,
  });
});

test('setFog takes 0 to 1 and tells every ready player once per change', async (t) => {
  const { World, report, ops, sent } = await started(t);
  report(1);
  sent.length = 0;
  assert.equal(World.fog, 0);
  assert.equal(World.setFog(1.5), false);
  assert.equal(World.setFog(-0.1), false);
  assert.equal(World.setFog(NaN), false);
  assert.deepEqual(sent, []);
  assert.equal(World.setFog(0.6), true);
  assert.equal(World.fog, 0.6);
  assert.equal(ops('worldWeather').at(-1).fog, 0.6);
  assert.equal(World.setFog(0.6), true);
  assert.equal(ops('worldWeather').length, 1);
  report(2);
  assert.equal(ops('worldWeather').at(-1).fog, 0.6);
  assert.equal(World.setFog(0), true);
  assert.equal(ops('worldWeather').at(-1).fog, 0);
});

test('setRain takes 0 to 1, soaks the ground while it rains and dries it slowly after', async (t) => {
  const { World, advance, report, ops, sent } = await started(t);
  report(1);
  sent.length = 0;
  assert.equal(World.setRain(1.5), false);
  assert.equal(World.setRain(0.5, -1), false);
  assert.equal(World.setRain(NaN), false);
  assert.deepEqual(sent, []);
  assert.equal(World.setRain(0.5, 0.25), true);
  assert.equal(World.rainStrength, 0.5);
  assert.equal(World.rainLevel, 0.25);
  assert.equal(ops('worldWeather').length, 1);
  assert.equal(ops('worldWeather')[0].rainStrength, 0.5);
  assert.equal(ops('worldWeather')[0].rainLevel, 0.25);
  // 240 real seconds is 3600 game seconds: half a downpour wets by 3600 * 0.5 / 1800
  advance(240);
  assert.equal(World.wetness, 1);
  assert.equal(World.puddles, 0);
  advance(480);
  assert.equal(World.wetness, 3);
  assert.equal(World.puddles, 1);
  World.setRain(0);
  const wet = World.wetness;
  advance(240);
  assert.equal(wet - World.wetness < 0.3, true);
  assert.equal(World.wetness > 2, true);
  assert.equal(World.setRain(0, 1), true);
});

test('setWind takes a vector of at most 50 m/s and reads back a Vector3', async (t) => {
  const { World, host, report, ops, sent } = await started(t);
  report(1);
  sent.length = 0;
  assert.equal(World.setWind({ x: 3, z: 4 }), true);
  assert.equal(World.windSpeed, 5);
  assert.deepEqual({ ...World.wind }, { x: 3, y: 0, z: 4 });
  assert.equal(World.wind instanceof host.api.Vector3, true);
  assert.deepEqual(ops('worldWeather')[0].wind, { x: 3, y: 0, z: 4 });
  const count = sent.length;
  assert.equal(World.setWind({ x: 40, y: 40 }), false);
  assert.equal(World.setWind({ x: NaN }), false);
  assert.equal(World.setWind({ x: '1' }), false);
  assert.equal(World.setWind(null), false);
  assert.equal(World.setWind(7), false);
  assert.equal(sent.length, count);
  assert.equal(World.windSpeed, 5);
});

test('the clock stops by itself at its last day', async (t) => {
  const { World, state, events, report, ops, sent } = await started(t);
  World.setTime(36499, 23);
  report(1);
  sent.length = 0;
  state.clock.setScale(200);
  sent.length = 0;
  // 3600 game seconds at 200 is 18 real seconds
  const advanceTo = state.now();
  state.now = state.clock.now = () => advanceTo + 18000;
  assert.equal(state.clock.atCeiling(), true);
  state.wake();
  assert.equal(World.timeScale, 0);
  assert.equal(World.day, 36500);
  assert.equal(World.hour, 0);
  assert.equal(state.timer, null);
  assert.equal(ops('worldTime').at(-1).timeScale, 0);
  assert.deepEqual(events.at(-1), ['day', 36500]);
  assert.equal(World.setTime(36500, 1), false);
});

test('a frozen clock sets no timer and sends nothing', async (t) => {
  const { World, state, advance, sent, report } = await started(t);
  report(1);
  World.setTimeScale(0);
  sent.length = 0;
  advance(1000);
  assert.equal(state.timer, null);
  assert.deepEqual(sent, []);
  assert.equal(World.hour, 9);
});

test('World.SHARED_DIMENSION is the shared dimension and cannot be changed', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const { World } = f.host.api;
  assert.equal(World.SHARED_DIMENSION, 4294967295);
  assert.throws(() => {
    World.SHARED_DIMENSION = 0;
  }, TypeError);
  assert.equal(World.SHARED_DIMENSION, 4294967295);
});

test('World.level is the level the server runs', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  assert.equal(f.host.api.World.level, '');
  f.host.statusRoutes.configure({ map: 'kutnohorsko' });
  assert.equal(f.host.api.World.level, 'kutnohorsko');
});
