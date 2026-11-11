'use strict';

// The server's Audio: a game sound for the players near a point or a body, as the built-in
// client event 'kcds:audio', and the catalog of the game's trigger names.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

function sounds(sent) {
  return sent
    .filter((message) => message.op === 'event' && message.name === 'kcds:audio')
    .map((message) => [message.player, JSON.parse(message.payload)]);
}

async function crowd(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  const near = f.report(3, { position: { x: 0, y: 0, z: 0 } });
  f.report(4, { position: { x: 20, y: 0, z: 0 } });
  f.report(5, { position: { x: 80, y: 0, z: 0 } });
  f.report(6, { position: { x: 1, y: 0, z: 0 }, dimension: 7 });
  // 8 is near, but its client scripts do not run yet.
  f.report(8, { position: { x: 2, y: 0, z: 0 } });
  for (const id of [3, 4, 5, 6]) {
    f.host.clientResourcesStarted(id);
  }
  f.sent.length = 0;
  return { ...f, near, Audio: f.host.api.Audio };
}

test('Audio.playAt reaches the players near the point, in its dimension', async (t) => {
  const { Audio, sent } = await crowd(t);
  assert.equal(Audio.playAt('a_o_bell_kkut_kostelni', { x: 0, y: 0, z: 0 }), true);
  assert.deepEqual(sounds(sent), [
    [3, { trigger: 'a_o_bell_kkut_kostelni', at: { x: 0, y: 0, z: 0 } }],
    [4, { trigger: 'a_o_bell_kkut_kostelni', at: { x: 0, y: 0, z: 0 } }],
  ]);
  sent.length = 0;
  // A wider range, another dimension, and a point nobody is near.
  assert.equal(Audio.playAt('a_o_bell_kkut_kostelni', { x: 0, y: 0, z: 0 }, { range: 100 }), true);
  assert.equal(sounds(sent).length, 3);
  assert.equal(
    Audio.playAt('a_o_bell_kkut_kostelni', { x: 0, y: 0, z: 0 }, { dimension: 7 }),
    true,
  );
  assert.equal(sounds(sent).at(-1)[0], 6);
  assert.equal(Audio.playAt('a_o_bell_kkut_kostelni', { x: 900, y: 0, z: 0 }), false);
  assert.equal(sounds(sent).length, 4);
});

test('Audio.playFrom plays from a body for the players near it', async (t) => {
  const { Audio, sent, near } = await crowd(t);
  assert.equal(Audio.playFrom('c_w_sword_hit', near), true);
  assert.deepEqual(sounds(sent)[0], [
    3,
    { trigger: 'c_w_sword_hit', entity: 3, at: { x: 0, y: 0, z: 0 } },
  ]);
  assert.equal(sounds(sent).length, 2);
  Audio.playFrom('c_w_sword_hit', 3, { range: 5 });
  assert.equal(sounds(sent).length, 3);
});

test('npc.playVoice plays a voice line from its body for the players near it', async (t) => {
  const { sent, host } = await crowd(t);
  const { Npc } = host.npcs;
  const npc = Npc.create({ position: { x: 0, y: 0, z: 0 } });
  sent.length = 0;
  const line = 'Localization/dialog/q_test_line_01';
  assert.equal(npc.playVoice(line), true);
  // Purpose '50': heard up to 50 m, so 3 and 4 but not 5 (80 m) nor 6 (another dimension).
  assert.deepEqual(
    sounds(sent).map(([player]) => player),
    [3, 4],
  );
  assert.deepEqual(sounds(sent)[0][1], {
    file: line,
    purpose: '50',
    entity: npc.id,
    at: { x: 0, y: 0, z: 0 },
  });
  sent.length = 0;
  // A near voice reaches only the nearest; a range of its own wins.
  npc.playVoice(line, { purpose: '10' });
  assert.deepEqual(
    sounds(sent).map(([player]) => player),
    [3],
  );
  sent.length = 0;
  npc.playVoice(line, { purpose: 'sfp_dialog', range: 100 });
  assert.equal(sounds(sent).length, 3);
  assert.throws(() => npc.playVoice('../secret'), TypeError);
  assert.throws(() => npc.playVoice('C:/x'), TypeError);
  assert.throws(() => npc.playVoice(line, { purpose: 'loud' }), RangeError);
});

test('Audio refuses what it cannot play', async (t) => {
  const { Audio } = await crowd(t);
  assert.throws(() => Audio.playAt('', { x: 0, y: 0, z: 0 }), TypeError);
  assert.throws(() => Audio.playAt('bell ring', { x: 0, y: 0, z: 0 }), TypeError);
  assert.throws(() => Audio.playAt('bell', { x: NaN, y: 0, z: 0 }), TypeError);
  assert.throws(() => Audio.playAt('bell', { x: 1e9, y: 0, z: 0 }), RangeError);
  assert.throws(() => Audio.playAt('bell', { x: 0, y: 0, z: 0 }, { range: 0 }), RangeError);
  assert.throws(() => Audio.playAt('bell', { x: 0, y: 0, z: 0 }, { range: 500 }), RangeError);
  assert.throws(() => Audio.playAt('bell', { x: 0, y: 0, z: 0 }, 'loud'), TypeError);
  assert.throws(() => Audio.playFrom('bell', 'someone'), TypeError);
});

test('Audio.list names the game triggers, by prefix, without the cinematics', async (t) => {
  const { Audio } = await crowd(t);
  const all = Audio.list();
  assert.ok(all.length > 1000);
  assert.deepEqual(all, [...all].sort());
  assert.ok(all.every((name) => !name.startsWith('cin_')));
  const bells = Audio.list('a_o_bell');
  assert.ok(bells.length > 0);
  assert.ok(bells.every((name) => name.startsWith('a_o_bell')));
  assert.throws(() => Audio.list(5), TypeError);
});
