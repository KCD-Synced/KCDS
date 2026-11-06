'use strict';

// Tabletop minigames on the server: the board's cells, pieces and their moves, seats,
// a player's requests from their game, turns, forfeits, bets, save and load.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  GLOBAL_WORLD,
  clientFixture,
  serverFixture,
  serverResource,
  delay,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const SETUP = `
const { Tabletop } = kcds.world;
globalThis.table = Tabletop.create({
  position: { x: 100, y: 200, z: 30 },
  rotation: 90,
  board: { rows: 3, columns: 3, cellSize: 0.5 },
  seats: [
    { position: { x: 101, y: 200, z: 30 }, side: 'x' },
    { position: { x: 99, y: 200, z: 30 }, side: 'o' },
  ],
  turnSeconds: 0.2,
});
const log = (...parts) => console.log(parts.join(' '));
kcds.core.Events.on('tabletopSit', (t, player, seat) => log('sit', player.id, seat));
kcds.core.Events.on('tabletopLeave', (t, player, seat, reason) => log('leave', player.id, seat, reason));
kcds.core.Events.on('tabletopPieceSelect', (t, player, piece) => {
  log('select', player.id, piece.side);
  return piece.side === t.seats[t.seatOf(player)].side;
});
kcds.core.Events.on('tabletopMoveRequest', (t, player, piece, cell) => {
  log('move', player.id, cell.row, cell.column);
  return t.pieceAt(cell) === null;
});
kcds.core.Events.on('tabletopTurn', (t, seat, player) => log('turn', seat, player.id));
kcds.core.Events.on('tabletopTurnTimeout', (t, seat) => log('timeout', seat));
kcds.core.Events.on('tabletopForfeit', (t, player, seat, reason) => log('forfeit', player.id, reason));
kcds.core.Events.on('tabletopEnd', (t, winner, reason) => log('end', winner ? winner.id : 'none', reason));
`;

async function setup(t) {
  const f = serverFixture(t);
  serverResource(f.root, 'board', SETUP);
  await f.host.startAll();
  const vm = f.host.resources.get('board').vm;
  return {
    ...f,
    table: vm.table,
    lines: () => f.linesOf('board').filter((line) => line !== 'started'),
  };
}

function near(actual, expected) {
  for (const axis of ['x', 'y', 'z']) {
    assert.ok(
      Math.abs(actual[axis] - expected[axis]) < 1e-6,
      `${axis}: ${actual[axis]} vs ${expected[axis]}`,
    );
  }
}

// The game's own dice badges are round, flat pieces.
const GOLD = 'dicebadge_balatro_badge_gold';
const SILVER = 'dicebadge_balatro_badge_silver';

const click = (f, player, row, column) =>
  f.host.tabletops.receive(player, 'kcds:tabletop', {
    op: 'click',
    table: f.table.id,
    cell: [row, column],
  });

test('tabletop: cells are turned with the table, pieces stand and stack on them', async (t) => {
  const f = await setup(t);
  // A quarter turn: the board's columns run along the world's y axis.
  near(f.table.cellPosition({ row: 1, column: 1 }), { x: 100, y: 200, z: 30 });
  near(f.table.cellPosition([1, 2]), { x: 100, y: 200.5, z: 30 });
  near(f.table.cellPosition([2, 1]), { x: 99.5, y: 200, z: 30 });
  assert.deepEqual(f.table.cellAt({ x: 99.6, y: 200.4, z: 30 }), { row: 2, column: 2 });
  assert.equal(f.table.cellAt({ x: 103, y: 200, z: 30 }), null);
  const first = f.table.addPiece({ model: GOLD, cell: [0, 0], side: 'x' });
  const second = f.table.addPiece({ model: GOLD, cell: [0, 0], side: 'x' });
  assert.deepEqual(
    f.table.piecesAt([0, 0]).map((piece) => piece.id),
    [first.id, second.id],
  );
  assert.ok(second.prop.position.z > first.prop.position.z);
  assert.throws(() => f.table.addPiece({ model: GOLD, cell: [3, 0] }), /row/);
  await second.moveTo([2, 2], { path: 'slide', ms: 120 });
  near(second.prop.position, f.table.cellPosition([2, 2]));
  assert.deepEqual(second.cell, { row: 2, column: 2 });
  second.remove();
  assert.equal(second.removed, true);
  assert.deepEqual(
    f.table.pieces.map((piece) => piece.id),
    [first.id],
  );
});

test('tabletop: players sit from their game, pick their own piece and the server moves it', async (t) => {
  const f = await setup(t);
  const henry = f.report(7);
  const hans = f.report(8);
  // Their games run the client resources: they learn the table at once.
  f.host.clientResourcesStarted(7);
  f.host.clientResourcesStarted(8);
  const tables = f.sent.filter(
    (message) => message.name === 'kcds:tabletop' && message.player === 7,
  );
  assert.equal(JSON.parse(tables.at(-1).payload).tables[0].rows, 3);
  f.table.addPiece({ model: GOLD, cell: [0, 0], side: 'x' });
  f.table.addPiece({ model: SILVER, cell: [2, 2], side: 'o' });
  f.host.tabletops.receive(henry, 'kcds:tabletop', { op: 'sit', table: f.table.id, seat: 0 });
  f.host.tabletops.receive(hans, 'kcds:tabletop', { op: 'sit', table: f.table.id, seat: 0 });
  f.host.tabletops.receive(hans, 'kcds:tabletop', { op: 'sit', table: f.table.id, seat: 1 });
  assert.equal(f.table.seatOf(henry), 0);
  assert.equal(f.table.seatOf(hans), 1);
  near(henry.position, { x: 101, y: 200, z: 30 });
  const told = f.sent.filter(
    (message) => message.op === 'event' && message.name === 'kcds:tabletop',
  );
  assert.ok(
    told.some((message) => message.player === 7 && JSON.parse(message.payload).op === 'seated'),
  );
  f.table.start();
  // Not Hans's turn: his click does nothing.
  click(f, hans, 2, 2);
  // Henry picks the other side's piece: refused. Then his own, then an empty cell.
  click(f, henry, 2, 2);
  click(f, henry, 0, 0);
  click(f, henry, 1, 1);
  await delay(500);
  assert.equal(f.table.pieceAt([1, 1])?.side, 'x');
  assert.equal(f.table.pieceAt([0, 0]), null);
  assert.deepEqual(f.lines().slice(0, 6), [
    'sit 7 0',
    'sit 8 1',
    'turn 0 7',
    'select 7 o',
    'select 7 x',
    'move 7 1 1',
  ]);
});

test('tabletop: the turn passes on its timer and a player who leaves forfeits', async (t) => {
  const f = await setup(t);
  const henry = f.report(7);
  const hans = f.report(8);
  f.table.sit(henry, 0);
  f.table.sit(hans, 1);
  f.table.start();
  await delay(320);
  assert.equal(f.table.turn, 1);
  f.host.disconnect(8);
  assert.equal(f.table.state, 'idle');
  const lines = f.lines();
  assert.ok(lines.includes('timeout 0'), lines.join('\n'));
  assert.ok(lines.includes('forfeit 8 disconnect'));
  assert.ok(lines.includes('end 7 forfeit'));
  assert.equal(f.table.seatOf(henry), 0);
});

test('tabletop: bets go to the winner, and a game without one gives them back', async (t) => {
  const f = await setup(t);
  const api = f.host.makeAPI();
  const henry = f.report(7);
  const hans = f.report(8);
  api.Inventory.add(henry, { item: 'money', amount: 100 });
  api.Inventory.add(hans, { item: 'money', amount: 30 });
  f.host.playerInventories.flush();
  f.table.sit(henry, 0);
  f.table.sit(hans, 1);
  assert.equal(await f.table.bet(50), false);
  f.host.playerInventories.flush();
  assert.equal(f.table.pot, 0);
  assert.equal(await f.table.bet(20), true);
  assert.equal(f.table.pot, 40);
  f.table.start();
  f.table.finish(henry);
  f.host.playerInventories.flush();
  const purse = (player) =>
    api.Inventory.get(player)
      .items.filter((row) => row.name === 'money')
      .reduce((total, row) => total + row.amount, 0);
  assert.deepEqual([purse(henry), purse(hans)], [120, 10]);
});

test('tabletop: save and load bring the board back; a stopped resource takes its tables', async (t) => {
  const f = await setup(t);
  const hans = f.report(8);
  f.table.addPiece({ model: GOLD, cell: [0, 1], side: 'x', data: { king: true } });
  f.table.addPiece({ model: SILVER, cell: [2, 0], side: 'o', visibleTo: hans });
  const saved = JSON.parse(JSON.stringify(f.table.save()));
  f.table.load({ pieces: [] });
  assert.equal(f.table.pieces.length, 0);
  f.table.load(saved);
  assert.deepEqual(
    f.table.pieces.map((piece) => [
      piece.cell.row,
      piece.cell.column,
      piece.side,
      piece.data.king ?? false,
    ]),
    [
      [0, 1, 'x', true],
      [2, 0, 'o', false],
    ],
  );
  assert.deepEqual(
    f.table.pieceAt([2, 0]).prop.visibleTo.map((player) => player.id),
    [8],
  );
  const props = f.host.props.props.size;
  await f.host.stop('board');
  assert.equal(f.host.tabletops.tables.size, 0);
  assert.equal(f.host.props.props.size, props - 2);
});

// The table as the server describes it to a player's game.
function tableMessage(fixedView = false) {
  return {
    op: 'tables',
    tables: [
      {
        id: 1,
        position: { x: 100, y: 200, z: 30 },
        rotation: { w: 1, x: 0, y: 0, z: 0 },
        rows: 3,
        columns: 3,
        cellSize: 0.5,
        height: 0,
        fixedView,
        seats: [{ position: { x: 101, y: 200, z: 30 }, taken: false }],
      },
    ],
  };
}

// The player's game: this player stands at a seat, the camera looks straight down at the board.
function playerGame(t) {
  const f = clientFixture(t);
  f.runtime.entities.updateDirectory({
    localPlayerId: 7,
    entities: [
      {
        id: 7,
        nativeId: 7,
        kind: 'player',
        dimension: GLOBAL_WORLD,
        position: { x: 101.2, y: 200, z: 30 },
        rotation: { w: 1, x: 0, y: 0, z: 0 },
      },
    ],
  });
  // Right +x, forward -z (down), up +y, at (100.4, 200.6, 31): above cell row 2, column 2.
  f.runtime.sync.call = (request) =>
    request.op === 'localWorldCamera'
      ? { m: [1, 0, 0, 100.4, 0, 0, 1, 200.6, 0, -1, 0, 31], fov: 1 }
      : null;
  return f;
}

const requests = (f) =>
  f.sent
    .filter((message) => message.op === 'event' && message.name === 'kcds:tabletop')
    .map((message) => JSON.parse(message.payload));

test('tabletop (client): a hint near a free seat sits down; the crosshair points, mouse 1 clicks', async (t) => {
  const f = playerGame(t);
  f.runtime.tabletop.receive(tableMessage());
  await delay(300);
  const hints = f.sent.filter((message) => message.op === 'actionHints').at(-1).hints;
  const sit = hints.find((hint) => hint.text === 'Sit down');
  assert.equal(sit.key, 'script_action');
  f.runtime.actionHints.receiveFired({ id: sit.id });
  assert.deepEqual(requests(f), [{ op: 'sit', table: 1, seat: 0 }]);
  f.runtime.tabletop.receive({ op: 'seated', table: 1, seat: 0 });
  await delay(250);
  const hover = f.sent.filter(
    (message) => message.op === 'gizmoSet' && message.owner === 'kcds:tabletop',
  );
  assert.ok(hover.length > 0);
  f.runtime.input.receiveEdges({ edges: [['mouse1', 'down']], down: ['mouse1'] });
  assert.deepEqual(requests(f).at(-1), { op: 'click', table: 1, cell: [2, 2] });
  const stand = f.sent.filter((message) => message.op === 'actionHints').at(-1).hints;
  assert.ok(stand.some((hint) => hint.text === 'Stand up'));
  f.runtime.tabletop.receive({ op: 'stood', table: 1 });
  assert.ok(f.sent.some((message) => message.op === 'gizmoRemove' || message.op === 'gizmoClear'));
});

test("tabletop (client): with the table's own camera, the arrow keys move the pointed cell", async (t) => {
  const f = playerGame(t);
  f.runtime.tabletop.receive(tableMessage(true));
  f.runtime.tabletop.receive({ op: 'seated', table: 1, seat: 0 });
  f.runtime.input.receiveEdges({ edges: [['up', 'down']], down: ['up'] });
  f.runtime.input.receiveEdges({ edges: [['left', 'down']], down: ['left'] });
  f.runtime.input.receiveEdges({ edges: [['enter', 'down']], down: ['enter'] });
  // The seat is at the board's +x side, so the player looks along -x: from the middle (1, 1) up
  // is one column down and left one row down.
  assert.deepEqual(requests(f).at(-1), { op: 'click', table: 1, cell: [0, 0] });
});

test('tabletop (client): with a camera the mouse pointer picks the cell under it; combat is blocked', async (t) => {
  const f = playerGame(t);
  f.runtime.tabletop.receive(tableMessage(true));
  f.runtime.tabletop.receive({ op: 'seated', table: 1, seat: 0 });
  // The pointer shows, the fighting and the mouse look are held off while seated.
  const controls = f.sent.filter((message) => message.op === 'controls').at(-1);
  assert.equal(controls.cursor, true);
  for (const action of ['attack_primary_mouse', 'block', 'rotateyaw']) {
    assert.ok(controls.actions.includes(action), action);
  }
  // The pointer in the middle of the screen: the cell straight below the camera, as the
  // crosshair's (2, 2) in the test above.
  f.runtime.input.receiveCursor({ x: 0, y: 0 });
  f.runtime.tabletop.point();
  f.runtime.input.receiveEdges({ edges: [['mouse1', 'down']], down: ['mouse1'] });
  assert.deepEqual(requests(f).at(-1), { op: 'click', table: 1, cell: [2, 2] });
  // Off the board, the pointer points at nothing.
  f.runtime.input.receiveCursor({ x: 1, y: 1 });
  f.runtime.tabletop.point();
  assert.equal(f.runtime.tabletop.hover, null);
  // Standing up gives it all back.
  f.runtime.tabletop.receive({ op: 'stood', table: 1 });
  const after = f.sent.filter((message) => message.op === 'controls').at(-1);
  assert.equal(after.cursor, false);
  assert.equal(after.actions.length, 0);
});

test("tabletop (client): a table's controls choose the pointer and the blocked actions", async (t) => {
  const f = playerGame(t);
  const message = tableMessage(true);
  message.tables[0].controls = { pointer: 'keys', block: ['use'] };
  f.runtime.tabletop.receive(message);
  f.runtime.tabletop.receive({ op: 'seated', table: 1, seat: 0 });
  const controls = f.sent.filter((sent) => sent.op === 'controls').at(-1);
  assert.equal(controls.cursor, false);
  assert.deepEqual(controls.actions, ['use']);
});

test('tabletop: marks go to the players they are for, as cells, a colour and a style', async (t) => {
  const f = await setup(t);
  f.report(7);
  f.report(8);
  f.host.clientResourcesStarted(7);
  f.host.clientResourcesStarted(8);
  const henry = f.host.players.get(7);
  const marksOf = (id) =>
    f.sent
      .filter((message) => message.name === 'kcds:tabletop' && message.player === id)
      .map((message) => JSON.parse(message.payload))
      .filter((payload) => payload.op === 'marks')
      .at(-1)?.marks;
  f.table.highlight(
    [
      [0, 0],
      [1, 1],
    ],
    { color: 0xffe8c45a, style: 'fill' },
  );
  f.table.highlight([[2, 2]], { players: [henry], style: 'dot' });
  assert.deepEqual(marksOf(8), [
    {
      cells: [
        [0, 0],
        [1, 1],
      ],
      color: 0xffe8c45a,
      style: 'fill',
    },
  ]);
  assert.equal(marksOf(7).length, 2);
  assert.deepEqual(marksOf(7)[1], { cells: [[2, 2]], color: 0xffffd27f, style: 'dot' });
  assert.throws(() => f.table.highlight([[0, 0]], { style: 'glow' }), RangeError);
  // Henry's own go with clearHighlight(henry); everyone's stay.
  f.table.clearHighlight(henry);
  assert.equal(marksOf(7).length, 1);
  f.table.clearHighlight();
  assert.deepEqual(marksOf(7), []);
  assert.deepEqual(marksOf(8), []);
});

test('tabletop (client): marks are drawn as flat faces on the board, the pointed cell as a frame', async (t) => {
  const f = playerGame(t);
  f.runtime.tabletop.receive(tableMessage());
  const drawings = () =>
    f.sent.filter((message) => message.op === 'gizmoSet' && message.owner === 'kcds:tabletop');
  f.runtime.tabletop.receive({
    op: 'marks',
    table: 1,
    marks: [
      { cells: [[0, 0]], color: 0xffe8c45a, style: 'fill' },
      {
        cells: [
          [1, 1],
          [2, 1],
        ],
        color: 0xff2f7d3a,
        style: 'dot',
      },
      { cells: [[2, 2]], color: 0xffe0362e, style: 'frame' },
    ],
  });
  const marks = drawings();
  // A fill, the dots and a frame with its faint inside: faces only, no wire edges.
  assert.equal(marks.length, 4);
  for (const drawing of marks) {
    assert.equal(drawing.lines.length, 0);
    assert.ok(drawing.triangles.length > 0);
    assert.ok(drawing.fill >>> 24 < 0xff, 'blended');
  }
  // The fill's corners lie on the board's top, just above it, inside cell (0, 0).
  const fill = marks[0];
  for (let i = 0; i < fill.points.length; i += 3) {
    assert.ok(fill.points[i] > 99.25 && fill.points[i] < 99.75, `x ${fill.points[i]}`);
    assert.ok(Math.abs(fill.points[i + 2] - 30.007) < 1e-6);
  }
  // Seated, the pointed cell is a frame of its own.
  f.runtime.tabletop.receive({ op: 'seated', table: 1, seat: 0 });
  await delay(250);
  assert.ok(drawings().some((drawing) => drawing.id === 'hover'));
  // New marks replace the old ones; none removes them all.
  f.runtime.tabletop.receive({ op: 'marks', table: 1, marks: [] });
  const removed = f.sent.filter(
    (message) => message.op === 'gizmoRemove' && message.id.startsWith('mark:'),
  );
  assert.equal(removed.length, 4);
});

test("tabletop: a chair seat is taken by sitting on the game's seat there, and left by getting up", async (t) => {
  const f = serverFixture(t);
  serverResource(
    f.root,
    'chairs',
    `globalThis.table = kcds.world.Tabletop.create({
      position: { x: 100, y: 200, z: 30 },
      board: { rows: 3, columns: 3, cellSize: 0.5 },
      seats: [{ position: { x: 101, y: 200, z: 30 }, chair: true }],
    });`,
  );
  await f.host.startAll();
  const table = f.host.resources.get('chairs').vm.table;
  const state = [...f.host.tabletops.tables.values()][0];
  const henry = f.report(7, { position: { x: 101.2, y: 200, z: 30 } });
  // Asking to sit does nothing at a chair: the game's own seat seats them.
  f.host.tabletops.receive(henry, 'kcds:tabletop', { op: 'sit', table: table.id, seat: 0 });
  assert.equal(table.seatOf(henry), -1);
  f.report(7, { position: { x: 101.2, y: 200, z: 30 }, seated: true });
  await delay(400);
  assert.equal(table.seatOf(henry), 0);
  // Not moved onto the seat: the game's sit-down put them there.
  near(henry.position, { x: 101.2, y: 200, z: 30 });
  assert.equal(state.seats[0].chair, true);
  f.report(7, { position: { x: 101.2, y: 200, z: 30 }, seated: false });
  await delay(400);
  assert.equal(table.seatOf(henry), -1);
  assert.throws(
    () =>
      f.host.tabletops.Tabletop.create({
        position: { x: 0, y: 0, z: 0 },
        board: { rows: 3, columns: 3, cellSize: 0.5 },
        seats: [{ position: { x: 1, y: 0, z: 0 }, chair: true, animation: { fragment: 'Sit' } }],
      }),
    TypeError,
  );
});
