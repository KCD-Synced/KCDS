'use strict';

// Player.find: a command argument names a player by id, nickname or part of one.

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

async function playersFixture(t) {
  const f = serverFixture(t);
  await f.host.startAll();
  f.report(4097, { nickname: 'Kharold' });
  f.report(4098, { nickname: 'Kharon' });
  f.report(4099, { nickname: 'Marek' });
  f.report(4100, { nickname: 'Old Marek' });
  return f.host.api.Player;
}

function ids(players) {
  return players.map((player) => player.id);
}

test('Player.find takes an id that names a player', async (t) => {
  const Player = await playersFixture(t);
  assert.deepEqual(ids(Player.find('4099')), [4099]);
  assert.deepEqual(ids(Player.find(' 4097 ')), [4097]);
});

test('Player.find prefers the whole nickname, ignoring case', async (t) => {
  const Player = await playersFixture(t);
  assert.deepEqual(ids(Player.find('kharold')), [4097]);
  assert.deepEqual(ids(Player.find('MAREK')), [4099]);
});

test('Player.find falls back to a prefix, then to a part of the nickname', async (t) => {
  const Player = await playersFixture(t);
  assert.deepEqual(ids(Player.find('kharo')).sort(), [4097, 4098]);
  assert.deepEqual(ids(Player.find('khart')), []);
  assert.deepEqual(ids(Player.find('harol')), [4097]);
  assert.deepEqual(ids(Player.find('old')), [4100]);
});

test('Player.find finds nobody for an empty text or an unknown id', async (t) => {
  const Player = await playersFixture(t);
  assert.deepEqual(Player.find(''), []);
  assert.deepEqual(Player.find(undefined), []);
  assert.deepEqual(Player.find('1234'), []);
});
