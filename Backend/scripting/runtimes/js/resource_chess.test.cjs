'use strict';

// The chess example resource (Dev/examples/resources/chess, not shipped with the server: a
// resource of its own on the Tabletop SDK): the rules (rules.js) and
// a whole game played by two seated players through the table's clicks.

const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const RESOURCE = path.join(__dirname, '../../../../Dev/examples/resources/chess');
const rules = require(path.join(RESOURCE, 'rules.js'));

function perft(position, depth) {
  if (depth === 0) {
    return 1;
  }
  let count = 0;
  for (const move of rules.legalMoves(position)) {
    count += perft(rules.play(position, move), depth - 1);
  }
  return count;
}

// A square from its name: 'e4' -> { row: 3, column: 4 }.
function square(name) {
  return { row: Number(name[1]) - 1, column: 'abcdefgh'.indexOf(name[0]) };
}

function playAll(position, names) {
  let current = position;
  for (const name of names) {
    const [from, to] = [name.slice(0, 2), name.slice(2, 4)];
    const move = rules.findMove(current, square(from), square(to), name[4] ?? 'q');
    assert.ok(move, `${name} is legal in ${rules.toFen(current)}`);
    current = rules.play(current, move);
  }
  return current;
}

test('chess rules: the move counts of the standard test positions (perft)', () => {
  const cases = [
    [rules.toFen(rules.initialPosition()), [20, 400, 8902]],
    // "Kiwipete": castling, en passant, promotions and pins.
    ['r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1', [48, 2039]],
    ['8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1', [14, 191, 2812]],
    ['r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1', [6, 264]],
    ['rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8', [44, 1486]],
  ];
  for (const [fen, counts] of cases) {
    const position = rules.fromFen(fen);
    counts.forEach((count, index) => {
      assert.equal(perft(position, index + 1), count, `${fen} at depth ${index + 1}`);
    });
  }
});

test('chess rules: castling, en passant, promotion and their notation', () => {
  const castle = rules.fromFen('r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1');
  const short = rules.findMove(castle, square('e1'), square('g1'));
  assert.equal(short.castle, 'king');
  assert.equal(rules.notation(castle, short), 'O-O');
  const after = rules.play(castle, short);
  assert.equal(rules.pieceAt(after, square('f1')).kind, 'r');
  assert.deepEqual(after.castling.white, { king: false, queen: false });
  // Not through an attacked cell: a rook on f8 guards f1.
  const guarded = rules.fromFen('5r1k/8/8/8/8/8/8/4K2R w K - 0 1');
  assert.equal(rules.findMove(guarded, square('e1'), square('g1')), null);
  // En passant only right after the double step.
  const passant = playAll(rules.initialPosition(), ['e2e4', 'a7a6', 'e4e5', 'd7d5']);
  const take = rules.findMove(passant, square('e5'), square('d6'));
  assert.equal(rules.notation(passant, take), 'exd6');
  assert.equal(rules.pieceAt(rules.play(passant, take), square('d5')), null);
  const late = playAll(passant, ['b1c3', 'a6a5']);
  assert.equal(rules.findMove(late, square('e5'), square('d6')), null);
  // Promotion, a queen by default or the piece asked for.
  const promote = rules.fromFen('8/4P1k1/8/8/8/8/8/4K3 w - - 0 1');
  const queen = rules.findMove(promote, square('e7'), square('e8'));
  assert.equal(queen.promotion, 'q');
  assert.equal(rules.notation(promote, queen), 'e8=Q');
  assert.equal(rules.findMove(promote, square('e7'), square('e8'), 'n').promotion, 'n');
});

test('chess rules: checkmate, stalemate and the draws', () => {
  const mate = playAll(rules.initialPosition(), ['f2f3', 'e7e5', 'g2g4', 'd8h4']);
  assert.deepEqual(rules.status(mate), {
    over: true,
    result: 'black',
    reason: 'checkmate',
    check: true,
  });
  const stalemate = rules.fromFen('7k/5Q2/6K1/8/8/8/8/8 b - - 0 1');
  assert.equal(rules.status(stalemate).reason, 'stalemate');
  assert.equal(
    rules.status(rules.fromFen('8/8/4k3/8/8/2B5/4K3/8 w - - 0 1')).reason,
    'insufficient material',
  );
  assert.equal(
    rules.status(rules.fromFen('8/8/4k3/8/8/2R5/4K3/8 w - - 100 80')).reason,
    'the fifty-move rule',
  );
  // The knights out and back twice: the start position a third time.
  let position = rules.initialPosition();
  const history = [rules.positionKey(position)];
  for (const name of ['g1f3', 'g8f6', 'f3g1', 'f6g8', 'g1f3', 'g8f6', 'f3g1', 'f6g8']) {
    position = playAll(position, [name]);
    const result = rules.status(position, history);
    history.push(rules.positionKey(position));
    if (history.length === 9) {
      assert.equal(result.reason, 'threefold repetition');
    } else {
      assert.equal(result.over, false);
    }
  }
});

// The resource's files, without its textures (the props need only the models).
function copyResource(root) {
  const target = path.join(root, 'chess');
  fs.cpSync(RESOURCE, target, {
    recursive: true,
    filter: (source) => !source.endsWith('.dds'),
  });
}

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

test('chess resource: two players sit and play a whole game to checkmate', async (t) => {
  const f = serverFixture(t);
  copyResource(f.root);
  await f.host.startAll();
  f.host.emit('consoleCommand', 'chess', ['100', '200', '30', '0']);
  const [table] = [...f.host.tabletops.tables.values()];
  assert.ok(table, 'the table stands');
  // The game's benches stand at both sides; asking the table to sit does not seat anyone there.
  assert.deepEqual(
    [...f.host.smartObjects.placed.values()].map((bench) => bench.blueprint),
    ['Bench/bench_1place_low', 'Bench/bench_1place_low'],
  );
  const white = f.report(7);
  const black = f.report(8);
  f.host.tabletops.receive(white, 'kcds:tabletop', { op: 'sit', table: table.id, seat: 0 });
  assert.equal(table.seats[0].playerId, null);
  // Each sits on a bench with the game's sit-down: their game reports them seated there.
  const seatAt = (index) => {
    const { x, y, z } = table.seats[index].at;
    return { x, y, z };
  };
  f.report(7, { position: seatAt(0), seated: true });
  f.report(8, { position: seatAt(1), seated: true });
  await until(() => table.pieces.size === 32, '32 pieces');
  const chats = () =>
    f.sent.filter((message) => message.op === 'chat').map((message) => message.text);
  // White sits at the table's +y: rank 1 is its last row, the a-file its last column.
  const click = (player, name) => {
    const at = square(name);
    f.host.tabletops.receive(player, 'kcds:tabletop', {
      op: 'click',
      table: table.id,
      cell: [7 - at.row, 7 - at.column],
    });
  };
  const moves = [];
  const move = async (player, from, to) => {
    click(player, from);
    click(player, to);
    const count = moves.length + 1;
    await until(
      () => chats().filter((text) => /^Chess \d+\.{1,3} /.test(text)).length >= count * 2,
      `${from}${to}`,
    );
    moves.push(`${from}${to}`);
  };
  // An illegal move does nothing: the pawn stays and it is still white's turn.
  click(white, 'e2');
  click(white, 'e5');
  await delay(50);
  assert.equal(
    chats().some((text) => /^Chess 1\./.test(text)),
    false,
  );
  // Black cannot move in white's turn.
  click(black, 'e7');
  click(black, 'e5');
  await delay(50);
  assert.equal(
    chats().some((text) => /^Chess /.test(text) && !/new game/.test(text)),
    false,
  );
  // Scholar's mate.
  await move(white, 'e2', 'e4');
  await move(black, 'e7', 'e5');
  await move(white, 'f1', 'c4');
  await move(black, 'b8', 'c6');
  await move(white, 'd1', 'h5');
  await move(black, 'g8', 'f6');
  await move(white, 'h5', 'f7');
  await until(() => chats().some((text) => /wins: checkmate/.test(text)), 'checkmate');
  const told = chats();
  assert.ok(told.includes('Chess 4. Qxf7#'), told.join('\n'));
  assert.ok(told.some((text) => text.includes('Moves: e4 e5 Bc4 Nc6 Qh5 Nf6 Qxf7#')));
  // The taken pawn left the board; 31 pieces stay.
  assert.equal(table.pieces.size, 31);
  assert.equal(table.playing, false);
});
