'use strict';

// The Prší example resource (Dev/examples/resources/prsi, not shipped with the server: a resource
// of its own on the TabletopDeck SDK): two seated players get four cards each and the first card
// lies face up; only a card that follows it goes on the table, a draw takes one, and the turn
// passes.

const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const RESOURCE = path.join(__dirname, '../../../../Dev/examples/resources/prsi');

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function until(check, what, milliseconds = 6000) {
  const end = Date.now() + milliseconds;
  while (!check()) {
    if (Date.now() > end) {
      assert.fail(`timed out waiting for ${what}`);
    }
    await delay(25);
  }
}

test('prsi resource: four cards each, only a following card goes on the table, then the turn passes', async (t) => {
  const f = serverFixture(t);
  fs.cpSync(RESOURCE, path.join(f.root, 'prsi'), { recursive: true });
  await f.host.startAll();
  const alice = f.report(7, { position: { x: 100, y: 200, z: 30 } });
  const bob = f.report(8);
  f.host.emit('consoleCommand', 'prsi', ['near', '7', 'table']);
  await until(() => f.host.tabletops.tables.size === 1, 'the table');
  const [table] = [...f.host.tabletops.tables.values()];
  // A table of its own: the game's two benches at its sides.
  assert.equal(f.host.smartObjects.placed.size, 2);
  const seatAt = (index) => {
    const { x, y, z } = table.seats[index].at;
    return { x, y, z };
  };
  f.report(7, { position: seatAt(0), seated: true });
  f.report(8, { position: seatAt(1), seated: true });
  await until(() => f.host.tabletops.decks.decksOf(table).length === 1, 'the deck');
  const deck = f.host.tabletops.decks.decksOf(table)[0].handle;
  assert.equal(deck.pieces.length, 32);
  await until(
    () =>
      deck.hand(alice).length === 4 &&
      deck.hand(bob).length === 4 &&
      deck.pile('table').length === 1,
    'four cards each and one on the table',
  );
  const board = table.handle;
  const mover = board.playerAt(board.turn);
  const other = mover === alice ? bob : alice;
  const top = deck.pile('table')[0];
  const follows = (card) => card.rank === 'Q' || card.suit === top.suit || card.rank === top.rank;
  const key = (player, action) =>
    f.host.tabletops.receive(player, 'kcds:tabletop', { op: 'deck', table: table.id, action });
  const point = (player, index) => {
    for (let step = 0; step < index; step += 1) {
      key(player, 'next');
    }
  };
  // The one whose turn it is has the draw key; the other has none.
  assert.equal(deck.actions(mover).draw, 'Draw a card');
  assert.equal(deck.actions(other).draw, false);
  // Not their turn: nothing goes.
  key(other, 'play');
  await delay(100);
  assert.equal(deck.hand(other).length, 4);
  const hand = deck.hand(mover);
  const wrong = hand.findIndex((card) => !follows(card));
  const right = hand.findIndex(follows);
  if (wrong >= 0) {
    point(mover, wrong);
    key(mover, 'play');
    await delay(100);
    assert.equal(deck.hand(mover).length, 4, 'a card that does not follow stays in the hand');
    point(mover, (4 - wrong) % 4);
  }
  if (right >= 0) {
    point(mover, right);
    key(mover, 'play');
    await until(() => deck.pile('table').length === 2, 'the card on the table');
    assert.equal(deck.hand(mover).length, 3);
  } else {
    key(mover, 'draw');
    await until(() => deck.hand(mover).length === 5, 'the drawn card');
  }
  assert.equal(board.playerAt(board.turn), other, 'the turn passed');
  // Now theirs: a draw key, or Stand after an ace.
  const keys = deck.actions(other);
  assert.ok(keys.draw !== false || keys.pass === 'Stand');
  assert.deepEqual(deck.actions(mover), { draw: false, pass: false, look: 'Look closer' });
});
