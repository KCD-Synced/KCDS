'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { Beehives, FULL_STOCK, REGROW_MS } = require('./stash/beehives.cjs');

const HIVE = 'objects/manmade/beekeeping/beehive_a.cgf';
const ROWS = [
  ['trosecko', 'aaaaaaaaaaaaaaaa', 100, 100, 10, HIVE, false, true, null],
  ['trosecko', 'bbbbbbbbbbbbbbbb', 900, 900, 10, HIVE, false, true, null],
  ['trosecko', 'cccccccccccccccc', 100, 100, 10, 'objects/chest_rustic_a.cdf', true, true, 0.1],
  ['kutnohorsko', 'dddddddddddddddd', 100, 100, 10, HIVE, false, true, null],
];

function setup(players) {
  const stashes = new Map();
  const Stash = {
    adopt(guid) {
      if (!stashes.has(guid)) {
        const stash = {
          amount: 0,
          added: 0,
          getInventory: () => ({
            items: stash.amount ? [{ itemClass: 'honey', amount: stash.amount }] : [],
          }),
          addItem({ item, amount }) {
            assert.equal(item, 'honey');
            stash.amount += amount;
            stash.added += 1;
          },
        };
        stashes.set(guid, stash);
      }
      return stashes.get(guid);
    },
  };
  const clock = { ms: 1000 };
  const hives = new Beehives({
    Stash,
    players: () => players,
    level: () => 'trosecko',
    now: () => clock.ms,
    rows: ROWS,
  });
  return { hives, stashes, clock };
}

test('a hive is adopted with the full stock when a player comes near', () => {
  const players = [{ position: { x: 5000, y: 5000 } }];
  const { hives, stashes } = setup(players);
  hives.tick();
  assert.equal(stashes.size, 0, 'nobody near: nothing to pay for');
  players[0].position = { x: 120, y: 100 };
  hives.tick();
  assert.deepEqual(
    [...stashes.keys()],
    ['aaaaaaaaaaaaaaaa'],
    'not the far one, the chest or the other level',
  );
  assert.equal(stashes.get('aaaaaaaaaaaaaaaa').amount, FULL_STOCK);
});

test('taken honey stays gone until it grows back', () => {
  const { hives, stashes, clock } = setup([{ position: { x: 100, y: 100 } }]);
  hives.tick();
  const stash = stashes.get('aaaaaaaaaaaaaaaa');
  stash.amount = 3;
  clock.ms += REGROW_MS - 1;
  hives.tick();
  assert.equal(stash.amount, 3, 'gone for everyone');
  clock.ms += 1;
  hives.tick();
  assert.equal(stash.amount, FULL_STOCK);
});

test('a hive left full gets nothing more', () => {
  const { hives, stashes, clock } = setup([{ position: { x: 100, y: 100 } }]);
  hives.tick();
  const stash = stashes.get('aaaaaaaaaaaaaaaa');
  clock.ms += REGROW_MS;
  hives.tick();
  assert.equal(stash.added, 1);
});
