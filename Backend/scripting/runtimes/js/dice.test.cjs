'use strict';

// Dice matches between two players: the scoring, the catalog and a match's flow.
const test = require('node:test');
const assert = require('node:assert/strict');
const { score, canScore } = require('./dice/scoring.cjs');
const { DiceMatches, TABLES } = require('./dice/dice.cjs');

test('scoring: singles, kinds, straights, and selections that do not score', () => {
  assert.equal(score([1]), 100);
  assert.equal(score([5]), 50);
  assert.equal(score([1, 5]), 150);
  assert.equal(score([1, 1, 1]), 1000);
  assert.equal(score([4, 4, 4]), 400);
  assert.equal(score([4, 4, 4, 4]), 800);
  assert.equal(score([2, 2, 2, 2, 2]), 800);
  assert.equal(score([6, 6, 6, 6, 6, 6]), 4800);
  assert.equal(score([1, 2, 3, 4, 5]), 500);
  assert.equal(score([2, 3, 4, 5, 6]), 750);
  assert.equal(score([1, 2, 3, 4, 5, 6]), 1500);
  assert.equal(score([1, 2, 3, 4, 5, 5]), 550);
  assert.equal(score([1, 1, 2, 3, 4, 5]), 600);
  assert.equal(score([1, 2]), 0, 'a die that does not count spoils the selection');
  assert.equal(score([2, 2]), 0);
  assert.equal(score([]), 0);
  assert.equal(score([8]), 0);
  assert.equal(canScore([2, 3, 4, 6, 6, 2]), false);
  assert.equal(canScore([2, 3, 3, 3, 6, 4]), true);
  assert.equal(canScore([2, 3, 4, 5, 6, 6]), true);
});

test('the NPC opponent keeps the best dice and banks by its skill', () => {
  const ai = require('./dice/dice_ai.cjs');
  assert.equal(ai.skillOf('easy'), 20);
  assert.equal(ai.skillOf(70), 70);
  assert.throws(() => ai.skillOf('impossible'), TypeError);
  assert.throws(() => ai.skillOf(101), RangeError);
  const turn = {
    faces: [1, 5, 2, 3, 4, 6, 0],
    rolledMask: 0b111111,
    available: 0b111111,
    allDice: 0b111111,
    rules: undefined,
    turnScore: 0,
    own: 0,
    other: 0,
    target: 4000,
  };
  // A straight: all six kept; a skilled player throws all six again, an unskilled one banks.
  assert.deepEqual(
    ai.choose(turn, 85, () => 0.99),
    { held: 0b111111, kind: 'continue' },
  );
  assert.deepEqual(
    ai.choose(turn, 20, () => 0.99),
    { held: 0b111111, kind: 'pass' },
  );
  // Banking wins: it banks.
  assert.equal(ai.choose({ ...turn, own: 3000 }, 85, () => 0.99).kind, 'pass');
  // A one and a five with four dice to throw: both kept, and on it goes.
  const pair = { ...turn, faces: [1, 5, 2, 3, 3, 6, 0] };
  assert.deepEqual(
    ai.choose(pair, 50, () => 0.99),
    { held: 0b11, kind: 'continue' },
  );
  // Nothing scores: no move.
  assert.equal(
    ai.choose({ ...turn, faces: [2, 3, 4, 6, 6, 2, 0] }, 50, () => 0.99),
    null,
  );
});

test('Dice.startAgainstNpc: the server plays the NPC, its throws and moves', () => {
  let clock = 0;
  // The seat draw (1: the NPC throws first), its throw (three ones and three others), then its
  // chance of a slip (none).
  const { dice, events, players, sent } = matchWith([1, 1, 1, 1, 2, 3, 4, 1000], () => clock);
  const host = dice.host;
  host.npcs = { npcs: new Map([[50, { id: 50, name: 'Hanus' }]]), targetDown() {} };
  assert.throws(() => dice.startAgainstNpc(7, { npc: 99 }), TypeError);
  const match = dice.startAgainstNpc(7, { npc: 50, difficulty: 'hard', targetScore: 300 });
  const start = sent.find((op) => op.kind === 'start');
  assert.equal(start.id, 7);
  assert.equal(start.opponent, 50);
  assert.equal(start.opponentName, 'Hanus');
  assert.equal(start.first, false);
  // Nothing goes to the NPC's seat.
  assert.ok(sent.every((op) => op.id === 7));
  // Its first throw waits for the player's sit-down.
  clock = 1600;
  dice.tick();
  assert.equal(sent.filter((op) => op.kind === 'roll').length, 0);
  clock = 8000;
  dice.tick();
  const roll = sent.filter((op) => op.kind === 'roll').at(-1);
  assert.equal(roll.mine, false);
  clock = 11000;
  dice.tick();
  const move = sent.filter((op) => op.kind === 'move').at(-1);
  assert.deepEqual([move.move, move.held], ['pass', 0b111]);
  const end = events.find(([name]) => name === 'diceMatchEnd');
  assert.deepEqual(end.slice(1, 6), [match, players.get(7), null, 1, 'won']);
  assert.equal(sent.filter((op) => op.kind === 'end').at(-1).result, 'lost');
  assert.equal(dice.api().gameOf(7), 0);
});

test('the catalog: the levels dice tables with two seats each', () => {
  assert.ok(TABLES.length >= 75);
  for (const table of TABLES) {
    assert.match(table.id, /^[0-9a-f]{16}$/);
    assert.match(table.behavior, /^[0-9a-f]{16}$/);
    assert.equal(table.seats.length, 2);
  }
});

// A host with two players at the first table, the events and the clients' ops written down;
// `faces` are the seat draw, then the dice dealt in order.
function matchWith(faces, now = () => 0) {
  const table = TABLES[0];
  const at = (dx) => ({ x: table.position.x + dx, y: table.position.y, z: table.position.z });
  const players = new Map([
    [7, { id: 7, ready: true, nickname: 'Seven', position: at(0.5) }],
    [8, { id: 8, ready: true, nickname: 'Eight', position: at(-0.5) }],
  ]);
  const events = [];
  const sent = [];
  const host = {
    players,
    emit: (name, ...args) => events.push([name, ...args]),
    bridge: { send: (op) => sent.push(op) },
  };
  const queue = [...faces];
  const random = (n) => (n === 2 ? queue.shift() ?? 0 : (queue.shift() ?? 1) - 1);
  const dice = new DiceMatches(host, random, now);
  return { dice, events, players, sent };
}

test('a match: throws, keeps, banks, busts and a win at the target', () => {
  // Seat draw 0; turn 1 throws 1 1 1 2 3 4 at once: keep the three 1s (1000) and pass.
  // Turn 2 (seat 1) throws 2 3 4 6 6 2, a bust. Turn 3 (seat 0) throws 5 2 3 4 6 6: keep the 5
  // (50) and throw the other five: 1 1 2 2 3, keep the two 1s (200) and pass: 1250 wins.
  const { dice, events, sent } = matchWith([
    0, 1, 1, 1, 2, 3, 4, 2, 3, 4, 6, 6, 2, 5, 2, 3, 4, 6, 6, 1, 1, 2, 2, 3,
  ]);
  const Dice = dice.api();
  const match = Dice.start(7, 8, { targetScore: 1200, secondName: 'Hans' });
  assert.equal(Dice.gameOf(7), match);
  assert.equal(Dice.gameOf(8), match);
  // The throw waits for the thrower's game: no move before it, only the thrower asks.
  assert.deepEqual(
    events.map((e) => e[0]),
    ['diceMatchStarting', 'diceMatchStart', 'diceTurnStart'],
  );
  assert.equal(dice.move(7, 'pass', 0b000111), false, 'nothing thrown yet');
  assert.equal(dice.clientMove(8, match, 'throw', 0), false, 'not their throw');
  assert.equal(dice.clientMove(7, match, 'throw', 0), true);
  assert.equal(dice.clientMove(7, match, 'throw', 0), false, 'thrown already');
  assert.deepEqual(
    events.map((e) => e[0]),
    ['diceMatchStarting', 'diceMatchStart', 'diceTurnStart', 'diceRoll'],
  );
  // Each client learns the match from its own side, then the first throw.
  const [startA, startB] = sent.filter((op) => op.kind === 'start');
  assert.deepEqual(
    [startA.id, startA.opponent, startA.seat, startA.first, startA.ownName, startA.opponentName],
    [7, 8, 0, true, '', 'Hans'],
  );
  assert.deepEqual([startB.id, startB.seat, startB.first, startB.ownName], [8, 1, false, 'Hans']);
  assert.equal(startA.behavior, TABLES[0].behavior);
  const rolls = sent.filter((op) => op.kind === 'roll');
  assert.deepEqual(
    rolls.map((op) => [op.id, op.mine, op.rolled]),
    [
      [7, true, 0b111111],
      [8, false, 0b111111],
    ],
  );
  assert.deepEqual(rolls[0].faces, [1, 1, 1, 2, 3, 4, 0]);
  assert.equal(dice.move(8, 'pass', 0b000111), false, 'not their turn');
  assert.equal(dice.move(7, 'pass', 0b001000), false, 'a 2 does not score');
  // The dice selected so far go to the other player's game as they are selected.
  assert.equal(dice.clientMove(7, match, 'select', 0b000001), true);
  assert.equal(dice.clientMove(7, match, 'select', 0), true);
  assert.equal(dice.clientMove(8, match, 'select', 0b000001), false, 'not their turn');
  assert.equal(dice.clientMove(7, match, 'select', 0b1000000), false, 'no such die');
  assert.deepEqual(
    sent.filter((op) => op.move === 'select').map((op) => [op.id, op.held]),
    [
      [8, 0b000001],
      [8, 0],
    ],
  );
  assert.equal(dice.clientMove(7, match + 1, 'pass', 0b000111), false, 'another match');
  assert.equal(dice.clientMove(7, match, 'pass', 0b000111), true);
  assert.deepEqual(
    sent.filter((op) => op.kind === 'move' && op.move !== 'select'),
    [{ op: 'diceState', id: 8, match, kind: 'move', move: 'pass', held: 0b000111 }],
  );
  const passed = events.filter((e) => e[0] === 'diceTurnEnd')[0][1];
  assert.deepEqual([passed.reason, passed.points, passed.totalScore], ['pass', 1000, 1000]);
  // Seat 1's game throws; seat 0's game throws after the bust.
  assert.equal(dice.clientMove(8, match, 'throw', 0), true);
  assert.equal(dice.clientMove(7, match, 'throw', 0), true);
  // Seat 1 busts on its turn's first throw, and seat 0 throws again at once.
  const bust = events.filter((e) => e[0] === 'diceTurnEnd').at(-1)[1];
  assert.deepEqual([bust.seat, bust.reason], [1, 'bust']);
  assert.deepEqual(events.at(-1)[1].faces, [5, 2, 3, 4, 6, 6]);
  // Keep the 5 and throw the other five, then keep the 1s and bank to win.
  assert.equal(dice.move(7, 'continue', 0b000001), true);
  assert.equal(dice.clientMove(7, match, 'throw', 0), true);
  const second = events.filter((e) => e[0] === 'diceRoll').at(-1)[1];
  assert.equal(second.rolledMask, 0b111110);
  assert.deepEqual(second.faces, [5, 1, 1, 2, 2, 3]);
  assert.equal(dice.move(7, 'pass', 0b000110), true);
  const end = events.at(-1);
  assert.deepEqual(end.slice(0, 1), ['diceMatchEnd']);
  assert.deepEqual([end[1], end[4], end[5], end[6], end[7]], [match, 0, 'won', 1250, 0]);
  assert.deepEqual(
    sent.filter((op) => op.kind === 'end').map((op) => [op.id, op.reason, op.result]),
    [
      [7, 'won', 'won'],
      [8, 'won', 'lost'],
    ],
  );
  assert.equal(Dice.gameOf(7), 0);
  assert.equal(dice.move(7, 'pass', 0b000110), false, 'the match is over');
});

test('a pass keeping no dice ends the turn with the points it has so far', () => {
  // Seat draw 0; seat 0 passes its first throw keeping nothing: the turn ends with none.
  {
    const { dice, events } = matchWith([0, 5, 2, 3, 4, 6, 6]);
    const match = dice.api().start(7, 8, { targetScore: 2000 });
    assert.equal(dice.clientMove(7, match, 'throw', 0), true);
    assert.equal(dice.clientMove(7, match, 'pass', 0), true);
    const passed = events.filter((e) => e[0] === 'diceTurnEnd').at(-1)[1];
    assert.deepEqual([passed.seat, passed.points, passed.totalScore], [0, 0, 0]);
    assert.equal(dice.clientMove(8, match, 'throw', 0), true, "seat 1's turn");
  }
  // Seat 0 throws 5 2 3 4 6 6, keeps the 5 (50), throws 1 1 2 2 3 and passes keeping none of them.
  const { dice, events, sent } = matchWith([0, 5, 2, 3, 4, 6, 6, 1, 1, 2, 2, 3]);
  const Dice = dice.api();
  const match = Dice.start(7, 8, { targetScore: 2000 });
  assert.equal(dice.clientMove(7, match, 'throw', 0), true);
  assert.equal(dice.move(7, 'continue', 0), false, 'a continue keeps dice');
  assert.equal(dice.move(7, 'continue', 0b000001), true);
  assert.equal(dice.clientMove(7, match, 'throw', 0), true);
  assert.equal(dice.clientMove(7, match, 'pass', 0), true);
  const passed = events.filter((e) => e[0] === 'diceTurnEnd').at(-1)[1];
  assert.deepEqual(
    [passed.seat, passed.reason, passed.points, passed.totalScore],
    [0, 'pass', 50, 50],
  );
  assert.deepEqual(
    sent.filter((op) => op.kind === 'move' && op.move === 'pass'),
    [{ op: 'diceState', id: 8, match, kind: 'move', move: 'pass', held: 0 }],
  );
});

test("a throw the thrower's game never asks for is dealt after a while", () => {
  let clock = 0;
  const { dice, events } = matchWith([0, 1, 1, 1, 2, 3, 4], () => clock);
  dice.api().start(7, 8);
  clock = 29 * 1000;
  dice.tick();
  assert.ok(!events.some((e) => e[0] === 'diceRoll'));
  clock = 30 * 1000;
  dice.tick();
  assert.deepEqual(events.at(-1)[1].faces, [1, 1, 1, 2, 3, 4]);
});

test('a match ends when a player leaves, waits too long or a script stops it', () => {
  let clock = 0;
  const { dice, events, sent } = matchWith([1, 1, 1, 1, 1, 1, 1], () => clock);
  const Dice = dice.api();
  // Seat 1 throws first.
  let match = Dice.start(7, 8);
  clock = 3 * 60 * 1000;
  dice.tick();
  let end = events.at(-1);
  assert.deepEqual([end[0], end[4], end[5]], ['diceMatchEnd', 0, 'timedOut']);
  match = Dice.start(7, 8);
  assert.equal(dice.leave(7, 'left'), true);
  end = events.at(-1);
  assert.deepEqual([end[4], end[5]], [1, 'left']);
  match = Dice.start(7, 8);
  assert.equal(dice.clientMove(8, match, 'gaveUp', 0), true);
  end = events.at(-1);
  assert.deepEqual([end[4], end[5]], [0, 'gaveUp']);
  match = Dice.start(7, 8);
  assert.equal(Dice.setNames(match, 'Henry', 'Hans'), true);
  // Each client gets its own name first.
  assert.deepEqual(
    sent.filter((op) => op.kind === 'names').map((op) => [op.id, op.ownName, op.opponentName]),
    [
      [7, 'Henry', 'Hans'],
      [8, 'Hans', 'Henry'],
    ],
  );
  assert.throws(() => Dice.setNames(match, 'x'.repeat(65), ''), TypeError);
  assert.equal(Dice.stop(match), true);
  assert.equal(Dice.stop(match), false);
  end = events.at(-1);
  assert.deepEqual([end[4], end[5]], [255, 'stopped']);
});

test('Dice.start refuses players apart, playing already, or with a bad target', () => {
  const { dice, players } = matchWith([0, 0, 0]);
  const Dice = dice.api();
  assert.throws(() => Dice.start(7, 7), /themselves/);
  assert.throws(() => Dice.start(7, 9), /connected/);
  assert.throws(() => Dice.start(7, 8, { targetScore: 0 }), /targetScore/);
  players.get(8).position = { x: 0, y: 0, z: 0 };
  assert.throws(() => Dice.start(7, 8), /stand together/);
});

test("players nearby watch the table's things in the thrower's game, and the end", () => {
  const { dice, sent } = matchWith([0, 1, 1, 1, 2, 3, 4]);
  const table = TABLES[0];
  // A watcher at the table, one far away, one in another world.
  const near = { id: 9, ready: true, position: { ...table.position, x: table.position.x + 3 } };
  const far = { id: 10, ready: true, position: { ...table.position, x: table.position.x + 40 } };
  const elsewhere = { ...near, id: 11, dimension: 5 };
  for (const player of [near, far, elsewhere]) {
    dice.host.players.set(player.id, player);
  }
  const match = dice.api().start(7, 8);
  const die = ['8000000000000000', 1, 2, 3, 0, 0, 0, 1, true, 0.462, 'objects/dice_basic.cgf', ''];
  // Only the thrower's game is passed on: to the other player of the match and the watcher near.
  assert.equal(dice.clientView(8, match, [die]), false, 'not their turn');
  assert.equal(dice.clientView(7, match, [die]), true);
  const watches = () => sent.filter((op) => op.op === 'diceWatch');
  assert.deepEqual(
    watches().map((op) => [op.id, op.ended, op.things]),
    [
      [8, false, [die]],
      [9, false, [die]],
    ],
  );
  assert.deepEqual(dice.api().watchers(match), [8, 9]);
  assert.deepEqual(dice.api().watchers(match + 1), []);
  // The other player's turn: their game throws its own dice, their watching ends.
  assert.equal(dice.clientMove(7, match, 'throw', 0), true);
  assert.equal(dice.clientMove(7, match, 'pass', 0b000111), true);
  assert.deepEqual(watches().at(-1), { op: 'diceWatch', id: 8, match, ended: true, things: [] });
  assert.deepEqual(dice.api().watchers(match), [9]);
  // The match ends: the watcher's game puts the table back.
  dice.leave(8, 'gaveUp');
  assert.deepEqual(watches().at(-1), { op: 'diceWatch', id: 9, match, ended: true, things: [] });
});

test("dice of the game's kinds: dealt by their weights, their meshes sent", () => {
  const { dice, sent } = matchWith([0]);
  const lucky = Array(6).fill('dieLucky');
  assert.throws(() => dice.api().start(7, 8, { firstDice: lucky.slice(1) }), /six dice kinds/);
  assert.throws(
    () => dice.api().start(7, 8, { firstDice: [...lucky.slice(1), 'noSuchDie'] }),
    /noSuchDie is not a dice kind/,
  );
  const kinds = dice.api().kinds();
  const luckyKind = kinds.find((kind) => kind.name === 'dieLucky');
  assert.deepEqual(luckyKind.weights, [6, 1, 2, 3, 4, 6]);
  assert.ok(kinds.length >= 41);
  // A die may be named by its id too, in any case.
  const firstDice = [...lucky.slice(1), luckyKind.id.toUpperCase()];
  const match = dice.api().start(7, 8, { firstDice });
  assert.deepEqual(dice.api().diceOf(match), [Array(6).fill('dieLucky'), null]);
  const starts = sent.filter((op) => op.op === 'diceState' && op.kind === 'start');
  assert.deepEqual(starts[0].ownDice, Array(6).fill(luckyKind.model));
  assert.deepEqual(starts[0].ownSides, Array(6).fill([0, 1, 2, 3, 4, 5]));
  assert.deepEqual(starts[0].opponentSides, []);
  assert.deepEqual(starts[0].opponentDice, []);
  assert.deepEqual(starts[1].ownDice, []);
  assert.deepEqual(starts[1].opponentDice, Array(6).fill(luckyKind.model));
  // The first seat throws first; its lucky dice are dealt out of their weights' total: the last
  // whole number lands on the last side (a 6), the first on the first (a 1).
  const asked = [];
  const answers = [21, 0, 6, 7, 21, 0];
  dice.random = (n) => {
    asked.push(n);
    return answers.shift();
  };
  assert.equal(dice.clientMove(7, match, 'throw', 0), true);
  assert.deepEqual(asked, Array(6).fill(22));
  const roll = sent.filter((op) => op.op === 'diceState' && op.kind === 'roll').at(-1);
  assert.deepEqual(roll.faces, [6, 1, 2, 3, 6, 1, 0]);
});

test('a player using a dice table: diceTableUse with the nearest table and their match', () => {
  const { dice, events, players } = matchWith([0]);
  assert.equal(dice.clientMove(7, 0, 'tableUse', 0), true);
  const used = events.filter(([name]) => name === 'diceTableUse');
  assert.equal(used.length, 1);
  assert.equal(used[0][1], players.get(7));
  assert.equal(used[0][2], TABLES[0]);
  assert.equal(used[0][3], 0);
  const match = dice.api().start(7, 8);
  dice.clientMove(8, 0, 'tableUse', 0);
  assert.equal(events.filter(([name]) => name === 'diceTableUse').at(-1)[3], match);
  // Far from every table: nothing.
  players.get(7).position = { x: 0, y: 0, z: 0 };
  assert.equal(dice.clientMove(7, 0, 'tableUse', 0), false);
});

test('jokers: in three of a kind and straights with a real die, never alone, joker dice play', () => {
  const { JOKER } = require('./dice/scoring.cjs');
  assert.equal(score([JOKER]), 0, 'never a single 1 or 5');
  assert.equal(score([JOKER, 5]), 0);
  assert.equal(score([JOKER, 4, 4]), 400, 'it completes three of a kind');
  assert.equal(score([JOKER, JOKER, 1]), 1000);
  assert.equal(score([JOKER, JOKER, JOKER]), 0, 'three of a kind needs a real die');
  assert.equal(score([1, 2, 3, 4, JOKER]), 500, 'it completes a straight');
  assert.equal(score([JOKER, JOKER, JOKER, 2, 6]), 750, 'the straight from 2 to 6');
  assert.equal(score([JOKER, JOKER, JOKER, JOKER, 2, 6]), 1500, 'all six');
  assert.equal(score([JOKER, 2, 3]), 0, 'a die that does not count still spoils it');
  assert.equal(score([JOKER, 3, 3, 1]), 400);
  assert.equal(canScore([2, 3, 4, 6, 6, JOKER]), true, 'two sixes and a joker');
  assert.equal(canScore([2, 3, 4, 6, JOKER, JOKER]), true);
  assert.equal(canScore([JOKER, JOKER, JOKER]), false, 'jokers alone are a bust');
  assert.equal(canScore([2, 2, 4, 4, 6, JOKER]), true);
  assert.equal(canScore([2, 4, 6, JOKER]), false);
  // A devil's die lands on its first side (the joker) when the weights' first whole number comes,
  // and both games are told its sides.
  const { dice, sent } = matchWith([0]);
  const match = dice.api().start(7, 8, { firstDice: Array(6).fill('dieDevil') });
  const start = sent.find((op) => op.op === 'diceState' && op.kind === 'start');
  assert.deepEqual(start.ownSides, Array(6).fill([6, 1, 2, 3, 4, 5]));
  dice.random = () => 0;
  dice.clientMove(7, match, 'throw', 0);
  const roll = sent.filter((op) => op.op === 'diceState' && op.kind === 'roll').at(-1);
  assert.deepEqual(roll.faces, [...Array(6).fill(JOKER), 0]);
});

test("badges: the game's table, its passive effects and the Defence badge", () => {
  const { rulesOf, headStart, inEffect, checkBadge } = require('./dice/badges.cjs');
  const { dice, sent } = matchWith([0]);
  const table = dice.api().badges();
  assert.equal(table.length, 35);
  for (const badge of table) {
    assert.match(badge.id, /^[0-9a-f-]{36}$/);
    assert.ok(['tin', 'silver', 'gold'].includes(badge.tier));
  }
  assert.throws(() => dice.api().start(7, 8, { firstBadge: 'noSuchBadge' }), /not a dice badge/);
  // Headstart: a share of the target, as the game reckons it.
  assert.equal(headStart(checkBadge('HeadStartDiceBadgePlumb'), 2000), 100);
  assert.equal(headStart(checkBadge('HeadStartDiceBadgeSilver'), 3000), 300);
  assert.equal(headStart(checkBadge('HeadStartDiceBadgeGold'), 2000), 400);
  // Formations: the Cut, the Gallows and the Eye, alone and with what the rest scores.
  const cut = rulesOf(checkBadge('FormationsDiceBadgePlumb'));
  assert.equal(score([3, 5], cut), 200);
  assert.equal(score([3, 5, 1], cut), 300);
  assert.equal(score([3, 5, 2], cut), 0, 'a die that does not count still spoils it');
  assert.equal(score([3, 5]), 0, 'without the badge it does not score');
  assert.equal(canScore([3, 5, 2, 4, 6, 6], cut), true);
  assert.equal(canScore([3, 5, 2, 4, 6, 6]), true, 'a five still scores without it');
  assert.equal(canScore([3, 3, 2, 4, 6, 6], cut), false);
  const gallows = rulesOf(checkBadge('FormationsDiceBadgeSilver'));
  assert.equal(score([4, 5, 6], gallows), 400);
  assert.equal(canScore([2, 2, 3, 4, 6, 6], gallows), false);
  const eye = rulesOf(checkBadge('FormationsDiceBadgeGold'));
  assert.equal(score([1, 3, 5], eye), 500);
  assert.equal(score([1, 2, 3, 4, 5], eye), 500, 'the straight reads as well as the Eye');
  assert.equal(score([1, 1, 3, 5], eye), 600);
  // The Emperor's 1+1+1 and Tyche's sixes.
  const emperor = rulesOf(checkBadge('ExtraValuebadgeGold'));
  assert.equal(score([1, 1, 1], emperor), 3000);
  assert.equal(score([1, 1, 1, 1], emperor), 6000);
  assert.equal(score([1, 1], emperor), 200);
  const tyche = rulesOf(checkBadge('ziskejOdznacek_tycheGoldBadge'));
  assert.equal(score([6, 6, 6], tyche), 1200);
  assert.equal(score([5, 5, 5], tyche), 500);
  // Defence: the other player's badge of its tier does nothing; another tier plays.
  const tinDefence = checkBadge('NullDiceBadgePlumb');
  assert.equal(inEffect(checkBadge('HeadStartDiceBadgePlumb'), tinDefence), null);
  assert.equal(inEffect(checkBadge('HeadStartDiceBadgeGold'), tinDefence).tier, 'gold');
  assert.equal(inEffect(tinDefence, checkBadge('NullDiceBadgePlumb')).effect, 'null');
  // A match: both games are told both badges; the cancelled headstart gives nothing.
  const match = dice.api().start(7, 8, {
    firstBadge: 'HeadStartDiceBadgeGold',
    secondBadge: 'nulldicebadgegold',
  });
  const starts = sent.filter((op) => op.op === 'diceState' && op.kind === 'start');
  const gold = table.find((badge) => badge.name === 'HeadStartDiceBadgeGold');
  assert.deepEqual(starts[0].ownBadge, { item: gold.id, effect: 0, tier: 2 });
  assert.deepEqual(starts[1].opponentBadge, starts[0].ownBadge);
  assert.equal(starts[0].opponentBadge.effect, 2);
  assert.deepEqual(dice.api().badgesOf(match), [
    { name: 'HeadStartDiceBadgeGold', inEffect: false },
    { name: 'NullDiceBadgeGold', inEffect: true },
  ]);
  dice.api().stop(match);
  const second = dice.api().start(7, 8, { firstBadge: 'HeadStartDiceBadgeGold' });
  assert.deepEqual(dice.matches.get(second).scores, [400, 0]);
  // The second seat's game: its own badge none, the other's the headstart.
  assert.equal(sent.filter((op) => op.kind === 'start').at(-1).ownBadge, null);
});

test('active badges: banking, the seventh die, rerolls, changed dice and Resurrection', () => {
  const lastRoll = (sent) => sent.filter((op) => op.kind === 'roll').at(-1);
  const lastMove = (sent) => sent.filter((op) => op.kind === 'move').at(-1);
  // Tyche: 600 for each six past two, doubled; the Defence badge of a higher tier cancels.
  const { rulesOf, checkBadge, inEffect } = require('./dice/badges.cjs');
  const tyche = rulesOf(checkBadge('ziskejOdznacek_tycheGoldBadge'));
  assert.equal(score([6, 6, 6, 6, 6], tyche), 4200);
  assert.equal(score([6, 6, 6, 1], tyche), 1300);
  assert.equal(inEffect(checkBadge('BalatroBadgePlumb'), checkBadge('NullDiceBadgeSilver')), null);
  assert.equal(score([5, 5, 5], rulesOf(checkBadge('ExtraValuebadgeGold'))), 500);

  // Warlord (gold): the whole turn doubled when banked with it; once a match.
  {
    const { dice, events, sent } = matchWith([0, 1, 1, 2, 3, 4, 6, 1, 2, 2, 3, 4, 6]);
    const match = dice.api().start(7, 8, { firstBadge: 'MultiplierDiceBadgeGold' });
    assert.deepEqual(dice.api().badgeUses(match), [1, 0]);
    dice.clientMove(7, match, 'throw', 0);
    assert.equal(dice.clientMove(7, match, 'continue', 0b000001), true);
    dice.clientMove(7, match, 'throw', 0);
    assert.equal(dice.clientMove(8, match, 'badgePass', 0b000010), false, 'not their turn');
    assert.equal(dice.clientMove(7, match, 'badgePass', 0b000010), true);
    assert.deepEqual(dice.matches.get(match).scores, [400, 0]);
    assert.deepEqual(lastMove(sent), {
      op: 'diceState',
      id: 8,
      match,
      kind: 'move',
      move: 'badgePass',
      held: 0b000010,
    });
    const used = events.filter(([name]) => name === 'diceBadge');
    assert.equal(used.length, 1);
    assert.equal(used[0][1].reason, 'multiplier');
    assert.equal(used[0][1].points, 200);
    assert.deepEqual(dice.api().badgeUses(match), [0, 0]);
  }

  // Doppelganger (silver): the banked selection twice; twice a match.
  {
    const { dice } = matchWith([0, 1, 1, 1, 2, 3, 4]);
    const match = dice.api().start(7, 8, {
      firstBadge: 'DoubleTakeDiceBadge',
      targetScore: 5000,
    });
    assert.deepEqual(dice.api().badgeUses(match), [2, 0]);
    dice.clientMove(7, match, 'throw', 0);
    assert.equal(dice.clientMove(7, match, 'pass', 0b001111), false, 'a 2 does not count');
    assert.equal(dice.clientMove(7, match, 'badgePass', 0b000111), true);
    assert.deepEqual(dice.matches.get(match).scores, [2000, 0]);
  }

  // Might: a seventh die thrown after a throw, kept like the others; once while six.
  {
    const { dice, sent } = matchWith([0, 2, 2, 3, 3, 4, 6, 1]);
    const match = dice.api().start(7, 8, { firstBadge: 'ExtraDiceBadgePlumb' });
    dice.clientMove(7, match, 'throw', 0);
    assert.equal(lastRoll(sent).bust, true, 'a bust without a Resurrection badge');
    assert.equal(dice.matches.get(match).seat, 1);
  }
  {
    const { dice, sent } = matchWith([0, 2, 2, 3, 3, 4, 1, 5]);
    const match = dice.api().start(7, 8, { firstBadge: 'ExtraDicebadgeSilver' });
    dice.clientMove(7, match, 'throw', 0);
    assert.equal(dice.clientMove(7, match, 'badge', 0b000001), false, 'it takes no dice');
    assert.equal(dice.clientMove(7, match, 'badge', 0), true);
    assert.deepEqual(lastRoll(sent).faces, [2, 2, 3, 3, 4, 1, 5]);
    assert.equal(lastRoll(sent).rolled, 0b1000000);
    assert.equal(dice.clientMove(7, match, 'badge', 0), false, 'seven dice already');
    assert.equal(dice.clientMove(7, match, 'continue', 0b1100000), true);
    assert.equal(dice.matches.get(match).available, 0b0011111);
    assert.deepEqual(dice.api().badgeUses(match), [1, 0]);
  }

  // Fortune (silver): up to two selected dice thrown again.
  {
    const { dice, sent } = matchWith([0, 1, 2, 3, 3, 4, 6, 5, 5]);
    const match = dice.api().start(7, 8, { firstBadge: 'RerollDiceBadgeSilver' });
    dice.clientMove(7, match, 'throw', 0);
    assert.equal(dice.clientMove(7, match, 'badge', 0b000111), false, 'three dice');
    assert.equal(dice.clientMove(7, match, 'badge', 0b000110), true);
    assert.deepEqual(lastRoll(sent).faces, [1, 5, 5, 3, 4, 6, 0]);
    assert.equal(lastRoll(sent).rolled, 0b000110);
    assert.equal(lastMove(sent).move, 'badge');
    assert.equal(dice.clientMove(7, match, 'badge', 0b000001), false, 'once a match');
    assert.equal(dice.clientMove(7, match, 'continue', 0b000111), true);
  }

  // The gold reroll of one face: two dice of one face.
  {
    const { dice } = matchWith([0, 1, 2, 3, 3, 4, 6, 2, 2]);
    const match = dice.api().start(7, 8, { firstBadge: 'RerollPipsbadgeGold' });
    dice.clientMove(7, match, 'throw', 0);
    assert.equal(dice.clientMove(7, match, 'badge', 0b000110), false, 'a 2 and a 3');
    assert.equal(dice.clientMove(7, match, 'badge', 0b001100), true);
  }

  // Transmutation (tin): one selected die shows a 3; Balatro: a joker die's joker.
  {
    const { dice, sent } = matchWith([0, 1, 2, 4, 4, 6, 6]);
    const match = dice.api().start(7, 8, { firstBadge: 'SetDiceDiceBadgePlumb' });
    dice.clientMove(7, match, 'throw', 0);
    assert.equal(dice.clientMove(7, match, 'badge', 0b000110), false, 'one die');
    const rolls = sent.filter((op) => op.kind === 'roll').length;
    assert.equal(dice.clientMove(7, match, 'badge', 0b000010), true);
    assert.equal(sent.filter((op) => op.kind === 'roll').length, rolls, 'nothing thrown');
    assert.deepEqual(dice.matches.get(match).faces, [1, 3, 4, 4, 6, 6, 0]);
  }
  {
    const { dice } = matchWith([0]);
    const devil = Array(6).fill('dieDevil');
    const match = dice.api().start(7, 8, { firstDice: devil, firstBadge: 'BalatroBadgeSilver' });
    dice.random = () => 1;
    dice.clientMove(7, match, 'throw', 0);
    assert.deepEqual(dice.matches.get(match).faces, [2, 2, 2, 2, 2, 2, 0]);
    assert.equal(dice.clientMove(7, match, 'badge', 0b000001), true);
    assert.equal(dice.matches.get(match).faces[0], 7);
    assert.equal(dice.clientMove(7, match, 'badge', 0b000001), false, 'it shows its joker now');
    assert.equal(dice.clientMove(7, match, 'badge', 0b000010), true, 'twice a match');
  }

  // Resurrection (silver): after a bust, the dice thrown again or the turn banked; in time.
  {
    let now = 0;
    const { dice, sent } = matchWith([0, 1, 5, 2, 3, 4, 6, 2, 2, 3, 4, 6, 6], () => now);
    const match = dice.api().start(7, 8, { firstBadge: 'AntiBustDiceBadgeSilver' });
    dice.clientMove(7, match, 'throw', 0);
    dice.clientMove(7, match, 'continue', 0b000011);
    dice.clientMove(7, match, 'throw', 0);
    assert.equal(lastRoll(sent).bust, true);
    assert.equal(dice.matches.get(match).seat, 0, 'the turn waits for the badge');
    assert.equal(dice.clientMove(7, match, 'pass', 0b000100), false);
    assert.equal(dice.clientMove(7, match, 'bustChoice', 3), false);
    assert.equal(dice.clientMove(7, match, 'bustChoice', 0), true);
    assert.deepEqual(dice.matches.get(match).scores, [150, 0]);
    assert.equal(lastMove(sent).move, 'bustChoice');
    assert.deepEqual(dice.api().badgeUses(match), [1, 0]);
    // The other player busts; then the first busts again and lets the badge go: the bust stands.
    // Every throw: 2, 3, 4, 6, 6, 2 (no die scores).
    const busting = [1, 2, 3, 5, 5, 1];
    let next = 0;
    dice.random = () => busting[next++ % busting.length];
    dice.clientMove(8, match, 'throw', 0);
    assert.equal(dice.matches.get(match).seat, 0);
    now = 1000;
    dice.clientMove(7, match, 'throw', 0);
    assert.equal(dice.matches.get(match).bustSince, 1000);
    now += 31 * 1000;
    dice.tick();
    assert.equal(dice.matches.get(match).seat, 1, 'the bust stood');
    const told = sent.filter((op) => op.move === 'bustChoice' && op.held === 2);
    assert.deepEqual(
      told.map((op) => op.id),
      [7, 8],
      'both games are told',
    );
    assert.deepEqual(dice.api().badgeUses(match), [1, 0]);
    // Thrown again: the busted dice wait for the thrower's game.
    dice.clientMove(8, match, 'throw', 0);
    dice.clientMove(7, match, 'throw', 0);
    assert.equal(dice.clientMove(7, match, 'bustChoice', 1), true);
    assert.equal(dice.matches.get(match).awaitingThrow, true);
    assert.deepEqual(dice.api().badgeUses(match), [0, 0]);
  }
});

test('table uses: each ready player is told whether they are answered, again on a change', () => {
  const { dice, players, sent } = matchWith([]);
  const told = () => sent.filter((op) => op.op === 'diceTables');
  dice.pumpTables(false);
  assert.deepEqual(told(), [
    { op: 'diceTables', id: 7, offered: false },
    { op: 'diceTables', id: 8, offered: false },
  ]);
  dice.pumpTables(false);
  assert.equal(told().length, 2);
  dice.pumpTables(true);
  assert.deepEqual(told().slice(2), [
    { op: 'diceTables', id: 7, offered: true },
    { op: 'diceTables', id: 8, offered: true },
  ]);
  // A player entering the world later is told at once; one leaving it is told again on return.
  players.set(9, { id: 9, ready: true, nickname: 'Nine', position: { x: 0, y: 0, z: 0 } });
  players.get(8).ready = false;
  dice.pumpTables(true);
  assert.deepEqual(told().slice(4), [{ op: 'diceTables', id: 9, offered: true }]);
  players.get(8).ready = true;
  dice.pumpTables(true);
  assert.deepEqual(told().slice(5), [{ op: 'diceTables', id: 8, offered: true }]);
});

test('players in a match cannot be fought: NPCs stop, blows are held and refused', () => {
  const { ResourceHost } = require('./host.cjs');
  const { dice, players } = matchWith([]);
  const stopped = [];
  dice.host.npcs = { targetDown: (player) => stopped.push(player.id) };
  assert.ok(dice.start(7, 8));
  assert.deepEqual(stopped, [7, 8]);

  const sent = [];
  const host = {
    players: new Map([...players, [9, { id: 9, ready: true }]]),
    dice,
    handlers: new Map(),
    services: { handles: () => false },
    hitRulesSent: false,
    bridge: { send: (op) => sent.push(op) },
    emit: () => true,
  };
  // Blows wait for the server's ruling while a match runs, even without a playerHit handler.
  ResourceHost.prototype.pumpHitRules.call(host);
  assert.deepEqual(sent.pop(), { op: 'hitRules', enabled: true, harm: false });
  // A blow on a player in the match, or by one, is refused.
  ResourceHost.prototype.playerHit.call(host, { id: 7, request: 1, attacker: 9, damage: 10 });
  assert.equal(sent.pop().ruling, 0);
  ResourceHost.prototype.playerHit.call(host, { id: 9, request: 2, attacker: 8, damage: 10 });
  assert.equal(sent.pop().ruling, 0);
});

test('chairs: a player sits down to wait; the match plays each from their own chair', () => {
  let clock = 0;
  const { dice, players, sent, events } = matchWith([0], () => clock);
  const Dice = dice.api();
  const table = TABLES[0];
  assert.equal(Dice.sit(8), 0);
  assert.deepEqual(sent.at(-1), {
    op: 'diceState',
    id: 8,
    match: 0,
    kind: 'sit',
    behavior: table.behavior,
    seat: 0,
  });
  assert.equal(Dice.sit(8), 0, 'the chair they hold already');
  assert.equal(sent.length, 1, 'no second sit-down');
  assert.equal(Dice.sit(7), 1);
  assert.deepEqual(Dice.sittersAt(table.id), [8, 7]);
  // A third player at the table finds no chair.
  players.set(9, {
    id: 9,
    ready: true,
    nickname: 'Nine',
    position: { ...players.get(7).position },
  });
  assert.equal(Dice.sit(9), -1);
  // Started in the other order, each still plays from their own chair: player 8 on chair 0.
  const match = Dice.start(7, 8, { firstName: 'Seven', secondName: 'Eight' });
  const found = dice.matches.get(match);
  assert.deepEqual(found.players, [8, 7]);
  assert.deepEqual(found.names, ['Eight', 'Seven']);
  assert.deepEqual(
    events
      .find((e) => e[0] === 'diceMatchStart')
      .slice(2, 4)
      .map((p) => p.id),
    [8, 7],
  );
  const starts = sent.filter((op) => op.kind === 'start');
  assert.deepEqual(
    starts.map((op) => [op.id, op.seat, op.ownName]),
    [
      [8, 0, 'Eight'],
      [7, 1, 'Seven'],
    ],
  );
  assert.equal(Dice.sit(7), -1, 'a player who plays does not sit');
  // Chairs stay theirs through the match, however long.
  clock = 60 * 1000;
  dice.tick();
  assert.deepEqual(Dice.sittersAt(table.id), [8, 7]);
  Dice.stop(match);
  // After it, a player who stood up frees the chair once the sit-down time passed; one who still
  // sits keeps it.
  players.get(8).seated = true;
  clock += 11 * 1000;
  dice.tick();
  assert.deepEqual(Dice.sittersAt(table.id), [8, null]);
  // A player who leaves frees theirs at once.
  players.delete(8);
  assert.deepEqual(Dice.sittersAt(table.id), [null, null]);
});

test('chairs: a player away from every table or not in the world does not sit', () => {
  const { dice, players } = matchWith([]);
  const Dice = dice.api();
  players.get(7).position = { x: 0, y: 0, z: 0 };
  assert.equal(Dice.sit(7), -1);
  players.get(8).ready = false;
  assert.equal(Dice.sit(8), -1);
  assert.equal(Dice.sit(99), -1);
});

test("chairs: a table the player's game does not have is left out", () => {
  const { dice } = matchWith([]);
  const Dice = dice.api();
  const table = TABLES[0];
  assert.equal(Dice.sit(7), 0);
  assert.equal(dice.clientMove(8, 0, 'tableMissing', 0), false, 'they hold no chair');
  assert.equal(dice.clientMove(7, 0, 'tableMissing', 0), true);
  assert.equal(
    Dice.tables().some((one) => one.id === table.id),
    false,
    'listed nowhere',
  );
  assert.equal(Dice.sit(7), -1, 'no other table near');
  assert.deepEqual(Dice.sittersAt(table.id), [null, null]);
});

test("a spectator's camera follows the dice and comes back at the end", () => {
  const { dice, sent } = matchWith([0, 1, 1, 1, 2, 3, 4]);
  const table = TABLES[0];
  const shots = [];
  dice.host.cameraShots = {
    startShot: (player, shot) => shots.push([player.id, shot]),
    endShot: (player) => shots.push([player.id, 'ended']),
  };
  const far = { id: 9, ready: true, position: { x: 0, y: 0, z: 0 } };
  dice.host.players.set(far.id, far);
  const match = dice.api().start(7, 8);
  const { x, y, z } = table.position;
  const die = (dx) => [
    '8000000000000000',
    x + dx,
    y,
    z + 0.8,
    0,
    0,
    0,
    1,
    true,
    0.462,
    'd.cgf',
    '',
  ];

  assert.equal(dice.api().spectate(7, match), false, 'a player of the match');
  assert.equal(dice.api().spectate(9, match + 1), false, 'no such match');
  assert.equal(dice.api().endSpectate(9), false, 'not spectating');
  assert.equal(dice.api().spectate(9, match), true);
  assert.equal(shots.length, 1);

  // Far from the table they are still shown the dice, and the view moves over them.
  assert.equal(dice.clientView(7, match, [die(0.5)]), true);
  assert.equal(shots.length, 2);
  assert.deepEqual(shots[1][1].lookAt, { x: x + 0.5, y, z: z + 0.8 });
  assert.equal(sent.filter((op) => op.op === 'diceWatch' && op.id === 9).length, 1);
  // A die that moved a little leaves the view where it is; dice waiting at the origin do not count.
  dice.clientView(7, match, [die(0.52)]);
  assert.equal(shots.length, 2);
  dice.clientView(7, match, [['8000000000000001', 0, 0, 0, 0, 0, 0, 1, true, 1, 'd.cgf', '']]);
  assert.equal(shots.length, 2);

  // The end of the match gives the view back.
  dice.leave(8, 'gaveUp');
  assert.deepEqual(shots.at(-1), [9, 'ended']);
  assert.equal(dice.api().endSpectate(9), false);
});

test('diceMatchStarting lets a script set the options of a match', () => {
  const { dice, events } = matchWith([0, 1, 1, 1, 2, 3, 4]);
  const handler = (name, first, second, table, options) => {
    if (name === 'diceMatchStarting') {
      events.push(['seen', first.id, second?.id ?? null, table.id, options.targetScore]);
      options.targetScore = 1500;
    }
  };
  const emit = dice.host.emit;
  dice.host.emit = (name, ...args) => {
    handler(name, ...args);
    emit(name, ...args);
  };
  const given = { targetScore: 3000 };
  dice.api().start(7, 8, given);
  const seen = events.find((event) => event[0] === 'seen');
  assert.deepEqual(seen, ['seen', 7, 8, TABLES[0].id, 3000]);
  const started = events.find((event) => event[0] === 'diceMatchStart');
  assert.equal(started.at(-1), 1500);
  assert.equal(given.targetScore, 3000, "the caller's own options stay as they were");
});

// the throw again once its dice lie still, for a panel that waits for them.
test('diceSettled follows a throw once the dice stop moving, or 4 s after it at the latest', () => {
  let now = 0;
  const { dice, events } = matchWith([0, 1, 1, 1, 2, 3, 4, 2, 3, 4, 6, 6, 2], () => now);
  const match = dice.api().start(7, 8);
  const die = ['8000000000000000', 1, 2, 3, 0, 0, 0, 1, true, 0.462, 'objects/dice_basic.cgf', ''];
  assert.equal(dice.clientMove(7, match, 'throw', 0), true);
  const settled = () => events.filter((e) => e[0] === 'diceSettled');
  // The dice roll: views keep coming.
  for (now = 100; now <= 1200; now += 100) {
    dice.clientView(7, match, [die]);
    dice.tick();
  }
  assert.equal(settled().length, 0, 'still rolling');
  now = 1700;
  dice.tick();
  assert.equal(settled().length, 1, 'quiet for 0.5 s after the last view');
  const roll = events.filter((e) => e[0] === 'diceRoll').at(-1)[1];
  assert.deepEqual(settled()[0][1], roll);
  now = 9000;
  dice.tick();
  assert.equal(settled().length, 1, 'told once');
});
