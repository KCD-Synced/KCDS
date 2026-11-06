'use strict';

// The dice resource shipped with the server (Backend/resources/dice): a player
// who uses a dice table sits down on a free chair; when both chairs hold seated players, the match
// starts, each playing from their own chair.

const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  serverResource,
  serverFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { TABLES } = require('./dice/dice.cjs');

const SERVER = fs.readFileSync(path.join(__dirname, '../../../resources/dice/server.js'), 'utf8');

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function atTable(t, apart = false) {
  const fixture = serverFixture(t);
  serverResource(fixture.root, 'dice', SERVER);
  await fixture.host.startAll();
  const table = TABLES[0];
  const at = (dx) => ({ x: table.position.x + dx, y: table.position.y, z: table.position.z });
  const place = (id, dx, seated = false) => fixture.report(id, { position: at(dx), seated });
  place(7, 0.5);
  place(8, apart ? 40 : -0.5);
  return { ...fixture, table, place };
}

function sits(sent, player) {
  return sent.filter(
    (message) => message.op === 'diceState' && message.kind === 'sit' && message.id === player,
  );
}

function chats(sent, pattern) {
  return sent.filter((message) => message.op === 'chat' && pattern.test(message.text));
}

test('a player who uses a table sits down on a free chair and waits', async (t) => {
  const { host, sent, table } = await atTable(t);
  host.dice.clientMove(7, 0, 'tableUse', 0);
  await delay(20);
  assert.deepEqual(
    sits(sent, 7).map((message) => [message.match, message.behavior, message.seat]),
    [[0, table.behavior, 0]],
  );
  assert.equal(chats(sent, /Waiting for another player/).length, 1);
  assert.deepEqual(host.dice.sittersAt(table.id), [7, null]);
  // Using the table again keeps the chair, without another sit-down.
  host.dice.clientMove(7, 0, 'tableUse', 0);
  await delay(20);
  assert.equal(sits(sent, 7).length, 1);
  await delay(600);
  assert.equal(host.dice.matches.size, 0, 'one player alone does not play');
});

test('the second player sits on the other chair; the match starts when both sit', async (t) => {
  const { host, sent, table, place } = await atTable(t);
  host.dice.clientMove(8, 0, 'tableUse', 0);
  host.dice.clientMove(7, 0, 'tableUse', 0);
  await delay(20);
  assert.equal(sits(sent, 8)[0].seat, 0);
  assert.equal(sits(sent, 7)[0].seat, 1);
  assert.equal(chats(sent, /sits down to play dice with you/).length, 1);
  await delay(600);
  assert.equal(host.dice.matches.size, 0, 'no match while they walk to their chairs');
  place(7, 0.5, true);
  place(8, -0.5, true);
  await delay(600);
  assert.equal(host.dice.matches.size, 1);
  const match = [...host.dice.matches.values()][0];
  assert.equal(match.table.id, table.id);
  assert.deepEqual(match.players, [8, 7], 'each plays from their own chair');
  const starts = sent.filter((message) => message.op === 'diceState' && message.kind === 'start');
  assert.deepEqual(starts.map((message) => [message.id, message.seat]).sort(), [
    [7, 1],
    [8, 0],
  ]);
});

test('after a match the same pair plays again only after one sat down anew', async (t) => {
  const { host, place, table } = await atTable(t);
  host.dice.clientMove(7, 0, 'tableUse', 0);
  host.dice.clientMove(8, 0, 'tableUse', 0);
  place(7, 0.5, true);
  place(8, -0.5, true);
  await delay(600);
  const [match] = [...host.dice.matches.keys()];
  host.dice.end(host.dice.matches.get(match), 0, 'won');
  await delay(600);
  assert.equal(host.dice.matches.size, 0, 'no automatic rematch');
  assert.deepEqual(host.dice.sittersAt(table.id), [7, 8], 'both stay on their chairs');
  // Player 7 stands up: their chair is free once the sit-down time passed.
  place(7, 1.5, false);
  const later = Date.now() + 11000;
  host.dice.now = () => later;
  await delay(600);
  assert.deepEqual(host.dice.sittersAt(table.id), [null, 8]);
  host.dice.clientMove(7, 0, 'tableUse', 0);
  place(7, 0.5, true);
  await delay(600);
  assert.equal(host.dice.matches.size, 1, 'they play again');
});

test('a third player finds both chairs taken', async (t) => {
  const { host, sent, place } = await atTable(t);
  place(9, 0.2);
  host.dice.clientMove(7, 0, 'tableUse', 0);
  host.dice.clientMove(8, 0, 'tableUse', 0);
  host.dice.clientMove(9, 0, 'tableUse', 0);
  await delay(20);
  assert.equal(sits(sent, 9).length, 0);
  assert.equal(chats(sent, /Both chairs at this dice table are taken/).length, 1);
});

test('players apart from a table do not sit', async (t) => {
  const { host, sent } = await atTable(t, true);
  host.dice.clientMove(8, 0, 'tableUse', 0);
  await delay(20);
  assert.equal(sits(sent, 8).length, 0);
});
