'use strict';

// The tabletop guide's two whole games (KCDS-DOCS/tabletop.md), run as the page
// writes them, in each language its tabs show: tic-tac-toe to a win, and checkers through its
// opening, a refused move and a capture. Players sit and click as their games ask the server to
// (the 'kcds:tabletop' requests of Client/resources/tabletop.cjs).

const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const assert = require('node:assert/strict');
const {
  LANGUAGES,
  serverResource,
  serverFixture,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const { docsDir, SKIP } = require('./docs_dir.cjs');

const DOCS = docsDir();
if (!DOCS) {
  test('the tabletop guide', { skip: SKIP }, () => {});
  return;
}
const GUIDE = path.join(DOCS, 'tabletop.md');
const FENCE = { javascript: 'js', lua: 'lua', csharp: 'cs' };
const COMMENT = { javascript: '//', lua: '--', csharp: '//' };

// The page's block of a game in one language: the one whose first line names the game.
function gameBlock(language, game) {
  const text = fs.readFileSync(GUIDE, 'utf8').replace(/\r\n/g, '\n');
  const pattern = new RegExp('```' + FENCE[language] + '\\n([\\s\\S]*?)```', 'g');
  for (const [, code] of text.matchAll(pattern)) {
    if (code.startsWith(`${COMMENT[language]} ${game}\n`)) {
      return code;
    }
  }
  throw new Error(`No ${language} ${game} on the page`);
}

async function playing(t, game, language) {
  const f = serverFixture(t);
  serverResource(f.root, game, { [language]: gameBlock(language, game) }, {}, language);
  await f.host.startAll();
  const first = f.report(7);
  const second = f.report(8);
  const [table] = f.host.tabletops.tables.values();
  const ask = (player, payload) =>
    f.host.tabletops.receive(player, 'kcds:tabletop', { table: table.id, ...payload });
  const click = (player, row, column) => ask(player, { op: 'click', cell: [row, column] });
  ask(first, { op: 'sit', seat: 0 });
  ask(second, { op: 'sit', seat: 1 });
  const chat = () =>
    f.sent.filter((message) => message.op === 'chat').map((message) => message.text);
  return { f, board: table.handle, first, second, click, chat };
}

for (const language of LANGUAGES) {
  test(`tabletop guide (${language}): tic-tac-toe is played to three in a row`, async (t) => {
    const { board, first, second, click, chat } = await playing(t, 'tic-tac-toe', language);
    assert.equal(board.state, 'playing');
    assert.equal(board.turn, 0);
    // Out of turn: nothing happens.
    click(second, 1, 1);
    assert.equal(board.pieces.length, 0);
    click(first, 0, 0);
    click(second, 1, 1);
    // A taken cell keeps the turn.
    click(first, 1, 1);
    assert.equal(board.turn, 0);
    click(first, 0, 1);
    click(second, 2, 2);
    click(first, 0, 2);
    assert.equal(board.state, 'idle');
    assert.deepEqual(
      board.pieces.map((piece) => `${piece.side}${piece.cell.row}${piece.cell.column}`),
      ['x00', 'o11', 'x01', 'o22', 'x02'],
    );
    assert.deepEqual(chat(), ['Player 7 wins at tic-tac-toe: three in a row']);
  });

  test(`tabletop guide (${language}): checkers sets up, refuses a wrong move and captures`, async (t) => {
    const { board, first, second, click } = await playing(t, 'checkers', language);
    const count = (side) => board.pieces.filter((piece) => piece.side === side).length;
    assert.equal(board.state, 'playing');
    assert.deepEqual([count('white'), count('black')], [12, 12]);
    // White: straight ahead is no move; a diagonal step is.
    click(first, 2, 1);
    click(first, 3, 1);
    assert.equal(board.pieceAt([2, 1])?.side, 'white');
    assert.equal(board.turn, 0);
    click(first, 2, 1);
    click(first, 3, 2);
    assert.equal(board.pieceAt([3, 2])?.side, 'white');
    assert.equal(board.turn, 1);
    // Black steps into reach, and white jumps over it.
    click(second, 5, 0);
    click(second, 4, 1);
    assert.equal(board.turn, 0);
    click(first, 3, 2);
    click(first, 5, 0);
    assert.equal(board.pieceAt([5, 0])?.side, 'white');
    assert.equal(board.pieceAt([4, 1]), null);
    assert.deepEqual([count('white'), count('black')], [12, 11]);
    assert.equal(board.turn, 1);
  });
}
