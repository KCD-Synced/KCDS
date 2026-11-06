'use strict';

// World.addInteraction: a hint on a level entity or a position, shown by the
// players' games, whose use runs the resource's handler.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

function lists(f) {
  return f.sent
    .filter((message) => message.op === 'interactions')
    .map((message) => [message.id, message.list.map((item) => [item.id, item.text])]);
}

test('an interaction goes to every ready player and its use runs the handler', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const first = f.report(7);
  f.report(8, { ready: false });
  const used = [];
  const pillory = f.host.api.World.addInteraction(
    { entity: 'so_pillory[Pillory/pillory1]', text: 'Dať do klady', radius: 3, mode: 'hold' },
    (player, interaction) => used.push([player.id, interaction.id]),
  );
  assert.deepEqual(lists(f), [[7, [[pillory.id, 'Dať do klady']]]]);
  const [item] = f.sent.find((message) => message.op === 'interactions').list;
  assert.equal(item.entity, 'so_pillory[Pillory/pillory1]');
  assert.equal(item.radius, 3);
  assert.equal(item.key, 0);
  assert.equal(item.hold, true);
  f.host.worldInteractions.fired(first, pillory.id);
  assert.deepEqual(used, [[7, pillory.id]]);
  // The player whose world gets ready gets the list then.
  f.report(8);
  assert.deepEqual(lists(f).at(-1), [8, [[pillory.id, 'Dať do klady']]]);
});

test('an interaction on a body follows it and skips its own player', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7);
  f.report(8);
  const prisoner = f.host.api.Player.all().find((player) => player.id === 8);
  const search = f.host.api.World.addInteraction(
    { entity: prisoner, text: 'Search', key: 'primary_action' },
    () => {},
  );
  const untie = f.host.api.World.addInteraction(
    { entity: prisoner, text: 'Untie', key: 'script_action' },
    () => {},
  );
  const last = (id) => f.sent.filter((m) => m.op === 'interactions' && m.id === id).at(-1);
  const seen = last(7).list;
  assert.deepEqual(
    seen.map((item) => [item.id, item.body, item.entity, item.key]),
    [
      [search.id, 8, '', 0],
      [untie.id, 8, '', 6],
    ],
  );
  // The prisoner does not get the hints on their own body.
  assert.equal(last(8)?.list.length ?? 0, 0);
  assert.throws(
    () => f.host.api.World.addInteraction({ entity: { id: -1 }, text: 'x' }, () => {}),
    TypeError,
  );
});

test('changes go out once, hidden or removed interactions do nothing when used', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const first = f.report(7);
  const second = f.report(8);
  let used = 0;
  const bell = f.host.api.World.addInteraction(
    { position: { x: 1, y: 2, z: 3 }, text: 'Zazvoniť' },
    () => {
      used += 1;
    },
  );
  assert.equal(bell.setText('Zazvoniť na zvon'), true);
  assert.equal(bell.setText('Zazvoniť na zvon'), true);
  bell.setPlayers([second]);
  f.host.worldInteractions.fired(first, bell.id);
  assert.equal(used, 0);
  f.host.worldInteractions.fired(second, bell.id);
  assert.equal(used, 1);
  bell.setEnabled(false);
  f.host.worldInteractions.fired(second, bell.id);
  assert.equal(used, 1);
  bell.remove();
  assert.equal(bell.exists, false);
  assert.equal(bell.setText('x'), false);
  // Each player's list went out only when it changed.
  assert.deepEqual(lists(f), [
    [7, [[bell.id, 'Zazvoniť']]],
    [8, [[bell.id, 'Zazvoniť']]],
    [7, [[bell.id, 'Zazvoniť na zvon']]],
    [8, [[bell.id, 'Zazvoniť na zvon']]],
    [7, []],
    [8, []],
  ]);
});

test('addInteraction refuses what it cannot show', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  const World = f.host.api.World;
  const noop = () => {};
  assert.throws(() => World.addInteraction({ text: 'a' }, noop), TypeError);
  assert.throws(
    () => World.addInteraction({ entity: 'x', position: { x: 0, y: 0, z: 0 }, text: 'a' }, noop),
    TypeError,
  );
  assert.throws(
    () => World.addInteraction({ entity: 'x', text: 'a', radius: 20 }, noop),
    RangeError,
  );
  assert.throws(
    () => World.addInteraction({ entity: 'x', text: 'a', key: 'fly' }, noop),
    RangeError,
  );
  assert.throws(() => World.addInteraction({ entity: 'x', text: 'two\nlines' }, noop), RangeError);
  assert.throws(() => World.addInteraction({ entity: 'x', text: 'a', color: 1 }, noop), TypeError);
  assert.throws(() => World.addInteraction({ entity: 'x', text: 'a' }, 'handler'), TypeError);
});

// a hint on a horse moves with it; setPlayers names who sees it.
test('an interaction on a horse goes by its id, for the players setPlayers names', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7);
  f.report(8);
  const owner = f.host.api.Player.all().find((player) => player.id === 7);
  const horse = f.host.api.Horse.spawn({ x: 1, y: 2, z: 3 });
  const feed = f.host.api.World.addInteraction(
    { entity: horse, text: 'Feed', radius: 3 },
    () => {},
  );
  feed.setPlayers([owner]);
  const last = (id) => f.sent.filter((m) => m.op === 'interactions' && m.id === id).at(-1);
  assert.deepEqual(
    last(7).list.map((item) => [item.id, item.body, item.entity]),
    [[feed.id, horse.id, '']],
  );
  assert.equal(last(8)?.list.length ?? 0, 0);
});

// a hint on a resource's prop goes by the prop's id, and the server knows where it is.
test('an interaction on a prop goes by its id and counts as near where the prop is', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7, { position: { x: 0, y: 0, z: 0 } });
  const { World, Prop } = f.host.api;
  const far = [];
  for (let index = 0; index < 255; index += 1) {
    far.push(
      World.addInteraction(
        { position: { x: 10 + index, y: 0, z: 0 }, text: `at ${index}` },
        () => {},
      ),
    );
  }
  const shrine = Prop.spawn('barrel_a', { x: 1, y: 0, z: 0 });
  const pray = World.addInteraction({ entity: shrine, text: 'Pray' }, () => {});
  const last = f.sent.filter((m) => m.op === 'interactions' && m.id === 7).at(-1).list;
  assert.equal(last.length, 255);
  const sent = last.find((item) => item.id === pray.id);
  assert.ok(sent, 'the prop, 1 m away, is among the 255 nearest');
  assert.deepEqual([sent.body, sent.entity, sent.text], [shrine.id, '', 'Pray']);
  assert.ok(!last.some((item) => item.id === far.at(-1).id), 'the farthest position is left out');
});

test('a player gets the 255 interactions nearest to them, picked again as they move', async (t) => {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(7, { position: { x: 0, y: 0, z: 0 } });
  const { World } = f.host.api;
  const made = [];
  for (let index = 0; index < 300; index += 1) {
    made.push(
      World.addInteraction({ position: { x: index, y: 0, z: 0 }, text: `at ${index}` }, () => {}),
    );
  }
  const last = () => f.sent.filter((m) => m.op === 'interactions' && m.id === 7).at(-1).list;
  assert.equal(last().length, 255);
  assert.deepEqual(
    last().map((item) => item.id),
    made.slice(0, 255).map((one) => one.id),
    'the 255 nearest of x = 0, in the order they were made',
  );
  assert.notEqual(f.host.worldInteractions.nearestTimer, null, 'picked again while more than 255');
  // At the other end of the row the nearest are the last 255 made.
  f.report(7, { position: { x: 299, y: 0, z: 0 } });
  f.host.worldInteractions.sync();
  assert.deepEqual(
    last().map((item) => item.id),
    made.slice(45).map((one) => one.id),
  );
  for (const one of made.slice(255)) {
    one.remove();
  }
  assert.equal(f.host.worldInteractions.nearestTimer, null, 'no timer with 255 or fewer');
  assert.equal(last().length, 255);
});
