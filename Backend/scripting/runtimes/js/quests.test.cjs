'use strict';

// Quest: the handle, who has a quest, the ops sent to the clients, tracking
// and its reports, late join and reload, a player leaving and a resource stopping.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  GLOBAL_WORLD,
  serverResource,
  serverFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

async function started(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const events = [];
  f.host.on('questTrackingChanged', (quest, player, tracked) => {
    events.push([quest.key, player.id, tracked]);
  });
  const ops = (...names) => f.sent.filter((message) => names.includes(message.op));
  // A client's report, as main.cjs forwards it.
  const reportTracking = (id, quest, tracked) => {
    f.host.quests.trackingReported(f.host.players.get(id), quest, tracked);
  };
  return { ...f, events, ops, reportTracking, Quest: f.host.api.Quest };
}

test('Quest.give makes a handle with its defaults and the members read it back', async (t) => {
  const { Quest, host } = await started(t);
  const quest = Quest.give('smith', 'Find the smith', {
    description: 'He went north.\nAsk at the mill.',
    type: 'main',
    objectives: [
      'Talk to the miller',
      { text: 'Bring bread', optional: true, progress: 'done' },
      { text: 'Reach the forge', position: { x: 1, y: 2, z: 3 } },
    ],
  });
  assert.equal(quest instanceof host.api.Entity, true);
  assert.equal(quest.key, 'smith');
  assert.equal(quest.title, 'Find the smith');
  assert.equal(quest.description, 'He went north.\nAsk at the mill.');
  assert.equal(quest.type, 'main');
  assert.equal(quest.progress, 'active');
  assert.equal(quest.player, 0);
  assert.equal(quest.objectiveCount, 3);
  assert.equal(quest.dimension, GLOBAL_WORLD);
  assert.equal(quest.toString(), `Quest(${quest.id}, smith, active, 3 objectives)`);
  const objectives = quest.objectives();
  assert.deepEqual(objectives.slice(0, 2), [
    { text: 'Talk to the miller', progress: 'active', optional: false },
    { text: 'Bring bread', progress: 'done', optional: true },
  ]);
  assert.deepEqual({ ...objectives[2].position }, { x: 1, y: 2, z: 3 });
  assert.equal('position' in objectives[0], false);
  // The copy is not the quest's.
  objectives[2].position.x = 50;
  assert.equal(quest.objectives()[2].position.x, 1);
  assert.equal(Quest.give('plain', 'Plain').type, 'side');
  assert.equal(new Quest(quest.id), quest);
  assert.equal(Quest.getById(quest.id), quest);
  assert.equal(Quest.getById(999999), null);
  assert.throws(() => new Quest(999999), /Unknown quest/);
});

test('Quest.give refuses what it cannot write', async (t) => {
  const { Quest } = await started(t);
  assert.throws(() => Quest.give('has space', 'x'), TypeError);
  assert.throws(() => Quest.give('k'.repeat(65), 'x'), TypeError);
  assert.throws(() => Quest.give('', 'x'), TypeError);
  assert.throws(() => Quest.give('a', 5), TypeError);
  assert.throws(() => Quest.give('a', 'x', { type: 'daily' }), RangeError);
  assert.throws(() => Quest.give('a', 'x', { player: 77 }), TypeError);
  assert.throws(() => Quest.give('a', 'x', { announce: 1 }), TypeError);
  assert.throws(() => Quest.give('a', 'x', { objectives: 'x' }), TypeError);
  assert.throws(
    () => Quest.give('a', 'x', { objectives: [{ text: 'a', progress: 'won' }] }),
    RangeError,
  );
  assert.throws(() => Quest.give('a', 'x', {}, -1), TypeError);
  Quest.give('a', 'x');
  assert.throws(() => Quest.give('a', 'y'), /already exists/);
  // The same key in another world is another quest.
  assert.equal(Quest.give('a', 'y', {}, 3).dimension, 3);
});

test('Texts are cut to what travels and objectives to eight', async (t) => {
  const { Quest } = await started(t);
  const quest = Quest.give('long', 'é'.repeat(200), {
    description: 'line\none\u0001two',
    objectives: Array.from({ length: 10 }, (_, i) => `step ${i}`),
  });
  assert.equal(Buffer.byteLength(quest.title), 256);
  assert.equal(quest.description, 'line\none two');
  assert.equal(quest.objectiveCount, 8);
  quest.title = 'tab\there';
  assert.equal(quest.title, 'tab here');
});

test('A quest goes to the ready players who have it, with the toast when it is given', async (t) => {
  const { Quest, host, report, ops } = await started(t);
  report(7);
  report(8);
  report(9, { ready: false });
  const quest = Quest.give('smith', 'Find the smith', {
    objectives: [{ text: 'Go', position: { x: 1, y: 2, z: 3 } }],
  });
  assert.deepEqual(
    ops('questShow'),
    [7, 8].map((id) => ({
      op: 'questShow',
      id,
      quest: quest.id,
      key: 'smith',
      title: 'Find the smith',
      description: '',
      type: 'side',
      progress: 'active',
      announce: true,
      objectives: [
        { text: 'Go', progress: 'active', optional: false, position: { x: 1, y: 2, z: 3 } },
      ],
    })),
  );
  // One for a single player goes to that player only, and is found by them.
  const own = Quest.give('errand', 'An errand', { player: 8, announce: false });
  assert.deepEqual(
    ops('questShow')
      .slice(2)
      .map((m) => [m.id, m.key, m.announce]),
    [[8, 'errand', false]],
  );
  assert.equal(own.player, 8);
  assert.equal(Quest.give('errand', 'Their errand', { player: host.players.get(7) }).player, 7);
  assert.equal(Quest.find('errand', undefined, 8), own);
  assert.equal(Quest.find('errand', GLOBAL_WORLD, 7).title, 'Their errand');
  assert.equal(Quest.find('nothing'), null);
  assert.equal(Quest.all().length, 3);
});

test('Changes are sent again; title and description quietly, progress with the toast', async (t) => {
  const { Quest, report, ops, sent } = await started(t);
  report(7);
  const quest = Quest.give('q', 'Quest', { objectives: ['one', 'two'] });
  const before = sent.length;
  quest.title = 'Quest';
  quest.setObjectives(['one', 'two']);
  assert.equal(sent.length, before);
  quest.title = 'Renamed';
  quest.description = 'Now with a page';
  quest.progress = 'done';
  quest.setProgress('failed', false);
  quest.setObjective(1, { text: 'two', progress: 'done' });
  quest.setObjective(5, 'past the end');
  quest.setObjectives(['only'], false);
  const shows = ops('questShow').slice(1);
  assert.deepEqual(
    shows.map((m) => [m.title, m.description, m.progress, m.announce, m.objectives.length]),
    [
      ['Renamed', '', 'active', false, 2],
      ['Renamed', 'Now with a page', 'active', false, 2],
      ['Renamed', 'Now with a page', 'done', true, 2],
      ['Renamed', 'Now with a page', 'failed', false, 2],
      ['Renamed', 'Now with a page', 'failed', true, 2],
      ['Renamed', 'Now with a page', 'failed', false, 1],
    ],
  );
  assert.deepEqual(shows[4].objectives[1], {
    text: 'two',
    progress: 'done',
    optional: false,
    position: null,
  });
  assert.throws(() => quest.setProgress('none'), RangeError);
  assert.throws(() => {
    quest.progress = 'unstarted';
  }, RangeError);
  assert.throws(() => quest.setObjective(-1, 'x'), TypeError);
});

test('track and untrack tell the clients and the reports fire questTrackingChanged once', async (t) => {
  const { Quest, report, ops, events, reportTracking } = await started(t);
  report(7);
  report(8);
  report(9, { ready: false });
  const quest = Quest.give('q', 'Quest');
  const own = Quest.give('own', 'Own', { player: 7 });
  // Everyone who has it; a player who is not ready yet is not told now.
  assert.equal(quest.track(), 2);
  assert.deepEqual(
    ops('questTrack').map((m) => [m.id, m.quest, m.tracked]),
    [
      [7, quest.id, true],
      [8, quest.id, true],
    ],
  );
  // Nobody who does not have the quest is told.
  assert.equal(own.track(8), 0);
  assert.equal(own.track(123), 0);
  assert.equal(own.track(7), 1);
  assert.equal(quest.untrack(8), 1);
  assert.deepEqual(ops('questTrack').slice(-1), [
    { op: 'questTrack', id: 8, quest: quest.id, tracked: false },
  ]);
  // The clients report what they follow: only changes, only for a quest they have.
  reportTracking(7, quest.id, true);
  reportTracking(7, quest.id, true);
  reportTracking(8, own.id, true);
  reportTracking(7, 424242, true);
  reportTracking(7, quest.id, false);
  assert.deepEqual(events, [
    ['q', 7, true],
    ['q', 7, false],
  ]);
  assert.throws(() => quest.track('seven'), TypeError);
});

test('A player who arrives or reloads gets their quests and their tracking again', async (t) => {
  const { Quest, report, ops, reportTracking } = await started(t);
  const quest = Quest.give('q', 'Quest', { track: false });
  report(7, { ready: false });
  assert.equal(quest.track(7), 0);
  assert.deepEqual(ops('questShow'), []);
  // Late join: the quest without the toast, then the tracking.
  report(7);
  assert.deepEqual(
    ops('questShow', 'questTrack').map((m) => [m.op, m.id, m.announce ?? m.tracked]),
    [
      ['questShow', 7, false],
      ['questTrack', 7, true],
    ],
  );
  // The player stops following it in their game; after a reload it is not followed.
  reportTracking(7, quest.id, false);
  report(7, { ready: false });
  report(7);
  assert.deepEqual(
    ops('questShow', 'questTrack')
      .slice(2)
      .map((m) => m.op),
    ['questShow'],
  );
  // A quest given with track: true is followed by whoever has it at once.
  report(8);
  const tracked = Quest.give('t', 'Tracked', { track: true });
  assert.deepEqual(
    ops('questTrack')
      .slice(1)
      .map((m) => [m.id, m.quest]),
    [
      [7, tracked.id],
      [8, tracked.id],
    ],
  );
});

test('A quest follows its dimension', async (t) => {
  const { Quest, report, ops } = await started(t);
  report(7);
  report(8, { dimension: 5 });
  const quest = Quest.give('w', 'World five', {}, 5);
  assert.deepEqual(
    ops('questShow').map((m) => m.id),
    [8],
  );
  report(7, { dimension: 5 });
  report(8, { dimension: 6 });
  assert.deepEqual(
    ops('questShow').map((m) => m.id),
    [8, 7],
  );
  assert.deepEqual(ops('questHide'), [{ op: 'questHide', id: 8, quest: quest.id }]);
  assert.deepEqual(Quest.all(5), [quest]);
  assert.deepEqual(Quest.all(6), []);
});

test('remove and removeAll take quests out of every journal', async (t) => {
  const { Quest, report, ops } = await started(t);
  report(7);
  const a = Quest.give('a', 'A');
  Quest.give('b', 'B', {}, 4);
  Quest.give('c', 'C');
  a.remove();
  a.remove();
  assert.equal(a.exists, false);
  assert.throws(() => {
    a.title = 'x';
  }, /removed/);
  assert.throws(() => a.track(), /removed/);
  assert.deepEqual(ops('questHide'), [{ op: 'questHide', id: 7, quest: a.id }]);
  assert.equal(Quest.removeAll(4), 1);
  assert.equal(Quest.removeAll(), 1);
  assert.equal(Quest.all().length, 0);
  assert.equal(ops('questHide').length, 2);
});

test('A player who leaves takes their own quests along', async (t) => {
  const { Quest, host, report, ops } = await started(t);
  report(7);
  report(8);
  const own = Quest.give('own', 'Own', { player: 7 });
  const shared = Quest.give('shared', 'Shared');
  shared.track();
  host.disconnect(7);
  assert.equal(own.exists, false);
  assert.equal(shared.exists, true);
  assert.deepEqual(ops('questHide'), []);
  assert.equal(shared.track(), 1);
});

test('A stopped resource takes its quests out of the journals', async (t) => {
  const f = serverFixture(t);
  serverResource(
    f.root,
    'questgiver',
    `
    kcds.core.Events.on('questgiver:give', (id) => {
      const quest = kcds.characters.Quest.give('fetch', 'Fetch water', { player: id, objectives: ['Fill the bucket'] });
      console.log('given ' + quest.key + ' to ' + quest.player);
    });
    kcds.core.Events.on('questTrackingChanged', (quest, player, tracked) => {
      console.log('tracking ' + quest.key + ' ' + player.id + ' ' + tracked);
    });
    `,
  );
  await f.host.startAll();
  f.report(7);
  await f.host.services.emit('shared', 'questgiver:give', [7]);
  const quest = f.host.api.Quest.find('fetch');
  assert.equal(quest.player, 7);
  f.host.quests.trackingReported(f.host.players.get(7), quest.id, true);
  assert.deepEqual(
    f.linesOf('questgiver').filter((line) => line !== 'started'),
    ['given fetch to 7', 'tracking fetch 7 true'],
  );
  await f.host.stop('questgiver');
  assert.equal(quest.exists, false);
  assert.deepEqual(
    f.sent.filter((m) => m.op === 'questHide').map((m) => [m.id, m.quest]),
    [[7, quest.id]],
  );
});
