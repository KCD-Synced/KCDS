'use strict';
// The rules of chess (FIDE's Laws of Chess, the basic rules of play): which moves are legal, what a
// move does to the position, and when the game is over. No engine of the board's: the server's
// resource (server.js) asks these rules about every click.
//
// A position is { board, turn, castling, enPassant, halfmoves, fullmoves }:
//   board      8 rows of 8 cells, row 0 white's back rank (rank 1), column 0 the a-file; a cell is
//              null or { kind: 'k' | 'q' | 'r' | 'b' | 'n' | 'p', color: 'white' | 'black' }
//   turn       the side to move
//   castling   { white: { king, queen }, black: { king, queen } }: which castlings are still
//              allowed (neither the king nor that rook has moved)
//   enPassant  the cell a pawn just passed over by its double step, or null
//   halfmoves  moves since the last capture or pawn move (the fifty-move rule)
//   fullmoves  the move number
//
// A move is { from, to, kind, color, capture, captureAt, promotion, castle, rookFrom, rookTo,
// double }; cells are { row, column }.

const SIZE = 8;
const BACK_RANK = ['r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'];
const KNIGHT_STEPS = [
  [1, 2],
  [2, 1],
  [2, -1],
  [1, -2],
  [-1, -2],
  [-2, -1],
  [-2, 1],
  [-1, 2],
];
const KING_STEPS = [
  [1, 0],
  [1, 1],
  [0, 1],
  [-1, 1],
  [-1, 0],
  [-1, -1],
  [0, -1],
  [1, -1],
];
const ROOK_LINES = [
  [1, 0],
  [-1, 0],
  [0, 1],
  [0, -1],
];
const BISHOP_LINES = [
  [1, 1],
  [1, -1],
  [-1, 1],
  [-1, -1],
];
const LETTERS = { k: 'K', q: 'Q', r: 'R', b: 'B', n: 'N', p: '' };

function other(color) {
  return color === 'white' ? 'black' : 'white';
}

function inside(row, column) {
  return row >= 0 && row < SIZE && column >= 0 && column < SIZE;
}

function cell(row, column) {
  return { row, column };
}

function same(a, b) {
  return a !== null && b !== null && a.row === b.row && a.column === b.column;
}

// The starting position.
function initialPosition() {
  const board = [];
  for (let row = 0; row < SIZE; row++) {
    board.push(new Array(SIZE).fill(null));
  }
  for (let column = 0; column < SIZE; column++) {
    board[0][column] = { kind: BACK_RANK[column], color: 'white' };
    board[1][column] = { kind: 'p', color: 'white' };
    board[6][column] = { kind: 'p', color: 'black' };
    board[7][column] = { kind: BACK_RANK[column], color: 'black' };
  }
  return {
    board,
    turn: 'white',
    castling: { white: { king: true, queen: true }, black: { king: true, queen: true } },
    enPassant: null,
    halfmoves: 0,
    fullmoves: 1,
  };
}

function copyPosition(position) {
  return {
    board: position.board.map((row) => row.map((square) => (square ? { ...square } : null))),
    turn: position.turn,
    castling: {
      white: { ...position.castling.white },
      black: { ...position.castling.black },
    },
    enPassant: position.enPassant ? { ...position.enPassant } : null,
    halfmoves: position.halfmoves,
    fullmoves: position.fullmoves,
  };
}

function pieceAt(position, at) {
  return inside(at.row, at.column) ? position.board[at.row][at.column] : null;
}

function kingOf(position, color) {
  for (let row = 0; row < SIZE; row++) {
    for (let column = 0; column < SIZE; column++) {
      const square = position.board[row][column];
      if (square && square.kind === 'k' && square.color === color) {
        return cell(row, column);
      }
    }
  }
  return null;
}

// Whether a piece of `by` attacks the cell.
function attacked(position, at, by) {
  const board = position.board;
  // Pawns attack diagonally forward: a white pawn from the row below.
  const pawnRow = by === 'white' ? at.row - 1 : at.row + 1;
  for (const side of [-1, 1]) {
    const column = at.column + side;
    if (inside(pawnRow, column)) {
      const square = board[pawnRow][column];
      if (square && square.color === by && square.kind === 'p') {
        return true;
      }
    }
  }
  for (const [rows, columns] of KNIGHT_STEPS) {
    const row = at.row + rows;
    const column = at.column + columns;
    if (inside(row, column)) {
      const square = board[row][column];
      if (square && square.color === by && square.kind === 'n') {
        return true;
      }
    }
  }
  for (const [rows, columns] of KING_STEPS) {
    const row = at.row + rows;
    const column = at.column + columns;
    if (inside(row, column)) {
      const square = board[row][column];
      if (square && square.color === by && square.kind === 'k') {
        return true;
      }
    }
  }
  const lines = [
    [ROOK_LINES, ['r', 'q']],
    [BISHOP_LINES, ['b', 'q']],
  ];
  for (const [directions, kinds] of lines) {
    for (const [rows, columns] of directions) {
      let row = at.row + rows;
      let column = at.column + columns;
      while (inside(row, column)) {
        const square = board[row][column];
        if (square) {
          if (square.color === by && kinds.includes(square.kind)) {
            return true;
          }
          break;
        }
        row += rows;
        column += columns;
      }
    }
  }
  return false;
}

function inCheck(position, color) {
  const king = kingOf(position, color);
  return king !== null && attacked(position, king, other(color));
}

function makeMove(from, to, square, extra = {}) {
  return {
    from,
    to,
    kind: square.kind,
    color: square.color,
    capture: null,
    captureAt: null,
    promotion: null,
    castle: null,
    rookFrom: null,
    rookTo: null,
    double: false,
    ...extra,
  };
}

// The moves of the piece on `from` as it moves, before asking whether its king is left in check.
function pseudoMoves(position, from) {
  const square = pieceAt(position, from);
  if (!square) {
    return [];
  }
  const moves = [];
  const color = square.color;
  const target = (row, column) => {
    if (!inside(row, column)) {
      return false;
    }
    const there = position.board[row][column];
    if (there && there.color === color) {
      return false;
    }
    moves.push(
      makeMove(from, cell(row, column), square, {
        capture: there ? there.kind : null,
        captureAt: there ? cell(row, column) : null,
      }),
    );
    return there === null;
  };
  if (square.kind === 'p') {
    pawnMoves(position, from, square, moves);
  } else if (square.kind === 'n') {
    for (const [rows, columns] of KNIGHT_STEPS) {
      target(from.row + rows, from.column + columns);
    }
  } else if (square.kind === 'k') {
    for (const [rows, columns] of KING_STEPS) {
      target(from.row + rows, from.column + columns);
    }
    castlingMoves(position, from, square, moves);
  } else {
    const directions = [];
    if (square.kind === 'r' || square.kind === 'q') {
      directions.push(...ROOK_LINES);
    }
    if (square.kind === 'b' || square.kind === 'q') {
      directions.push(...BISHOP_LINES);
    }
    for (const [rows, columns] of directions) {
      let row = from.row + rows;
      let column = from.column + columns;
      while (target(row, column)) {
        row += rows;
        column += columns;
      }
    }
  }
  return moves;
}

function pawnMoves(position, from, square, moves) {
  const forward = square.color === 'white' ? 1 : -1;
  const start = square.color === 'white' ? 1 : 6;
  const last = square.color === 'white' ? 7 : 0;
  const add = (to, extra) => {
    if (to.row === last) {
      for (const promotion of ['q', 'r', 'b', 'n']) {
        moves.push(makeMove(from, to, square, { ...extra, promotion }));
      }
    } else {
      moves.push(makeMove(from, to, square, extra));
    }
  };
  const one = cell(from.row + forward, from.column);
  if (inside(one.row, one.column) && pieceAt(position, one) === null) {
    add(one, {});
    const two = cell(from.row + 2 * forward, from.column);
    if (from.row === start && pieceAt(position, two) === null) {
      moves.push(makeMove(from, two, square, { double: true }));
    }
  }
  for (const side of [-1, 1]) {
    const to = cell(from.row + forward, from.column + side);
    if (!inside(to.row, to.column)) {
      continue;
    }
    const there = pieceAt(position, to);
    if (there && there.color !== square.color) {
      add(to, { capture: there.kind, captureAt: to });
    } else if (there === null && same(position.enPassant, to)) {
      // En passant: the pawn that passed over `to` is beside the capturing one.
      add(to, { capture: 'p', captureAt: cell(from.row, to.column) });
    }
  }
}

function castlingMoves(position, from, square, moves) {
  const color = square.color;
  const home = color === 'white' ? 0 : 7;
  if (from.row !== home || from.column !== 4 || inCheck(position, color)) {
    return;
  }
  const rights = position.castling[color];
  const enemy = other(color);
  const sides = [
    {
      right: rights.king,
      rook: 7,
      between: [5, 6],
      crossed: [5, 6],
      to: 6,
      rookTo: 5,
      castle: 'king',
    },
    {
      right: rights.queen,
      rook: 0,
      between: [1, 2, 3],
      crossed: [3, 2],
      to: 2,
      rookTo: 3,
      castle: 'queen',
    },
  ];
  for (const side of sides) {
    const rook = position.board[home][side.rook];
    if (!side.right || !rook || rook.kind !== 'r' || rook.color !== color) {
      continue;
    }
    if (side.between.some((column) => position.board[home][column] !== null)) {
      continue;
    }
    // The king may not pass over or land on an attacked cell.
    if (side.crossed.some((column) => attacked(position, cell(home, column), enemy))) {
      continue;
    }
    moves.push(
      makeMove(from, cell(home, side.to), square, {
        castle: side.castle,
        rookFrom: cell(home, side.rook),
        rookTo: cell(home, side.rookTo),
      }),
    );
  }
}

// The position after `move`.
function play(position, move) {
  const next = copyPosition(position);
  const board = next.board;
  const square = board[move.from.row][move.from.column];
  if (move.captureAt) {
    board[move.captureAt.row][move.captureAt.column] = null;
  }
  board[move.from.row][move.from.column] = null;
  board[move.to.row][move.to.column] = move.promotion
    ? { kind: move.promotion, color: square.color }
    : square;
  if (move.castle) {
    board[move.rookTo.row][move.rookTo.column] = board[move.rookFrom.row][move.rookFrom.column];
    board[move.rookFrom.row][move.rookFrom.column] = null;
  }
  // A king's move ends both castlings; a rook's move, or a rook taken on its corner, ends one.
  if (square.kind === 'k') {
    next.castling[square.color] = { king: false, queen: false };
  }
  for (const at of [move.from, move.to]) {
    for (const color of ['white', 'black']) {
      const home = color === 'white' ? 0 : 7;
      if (at.row === home && at.column === 0) {
        next.castling[color].queen = false;
      }
      if (at.row === home && at.column === 7) {
        next.castling[color].king = false;
      }
    }
  }
  next.enPassant = move.double ? cell((move.from.row + move.to.row) / 2, move.from.column) : null;
  next.halfmoves = square.kind === 'p' || move.capture ? 0 : position.halfmoves + 1;
  if (square.color === 'black') {
    next.fullmoves += 1;
  }
  next.turn = other(position.turn);
  return next;
}

// Every legal move of the side to move (or only of the piece on `from`).
function legalMoves(position, from = null) {
  const moves = [];
  for (let row = 0; row < SIZE; row++) {
    for (let column = 0; column < SIZE; column++) {
      const square = position.board[row][column];
      if (!square || square.color !== position.turn) {
        continue;
      }
      if (from !== null && (from.row !== row || from.column !== column)) {
        continue;
      }
      for (const move of pseudoMoves(position, cell(row, column))) {
        if (!inCheck(play(position, move), square.color)) {
          moves.push(move);
        }
      }
    }
  }
  return moves;
}

// The legal move from `from` to `to`; a pawn reaching the last row promotes to `promotion` ('q' by
// default). Null when there is none.
function findMove(position, from, to, promotion = 'q') {
  const moves = legalMoves(position, from).filter((move) => same(move.to, to));
  if (moves.length === 0) {
    return null;
  }
  return moves.find((move) => move.promotion === null || move.promotion === promotion) ?? null;
}

// The pieces each side has, for insufficient material: a bishop also by the colour of its cell.
function material(position) {
  const pieces = [];
  for (let row = 0; row < SIZE; row++) {
    for (let column = 0; column < SIZE; column++) {
      const square = position.board[row][column];
      if (square && square.kind !== 'k') {
        pieces.push({ ...square, light: (row + column) % 2 === 1 });
      }
    }
  }
  return pieces;
}

// Neither side can mate: king against king, king and a bishop or a knight against king, or kings
// and bishops all on cells of one colour.
function insufficientMaterial(position) {
  const pieces = material(position);
  if (pieces.length === 0) {
    return true;
  }
  if (pieces.length === 1 && (pieces[0].kind === 'b' || pieces[0].kind === 'n')) {
    return true;
  }
  if (pieces.every((piece) => piece.kind === 'b')) {
    return pieces.every((piece) => piece.light === pieces[0].light);
  }
  return false;
}

// The position's key for the threefold repetition: the board, the side to move, the castlings and
// an en passant capture that is really possible.
function positionKey(position) {
  const rows = position.board.map((row) => {
    return row
      .map((square) => {
        if (!square) {
          return '.';
        }
        return square.color === 'white' ? square.kind.toUpperCase() : square.kind;
      })
      .join('');
  });
  const rights = position.castling;
  const castling = [
    rights.white.king ? 'K' : '',
    rights.white.queen ? 'Q' : '',
    rights.black.king ? 'k' : '',
    rights.black.queen ? 'q' : '',
  ].join('');
  const passant = legalMoves(position).some((move) => {
    return move.kind === 'p' && same(move.to, position.enPassant);
  });
  const ep = passant ? `${position.enPassant.row}${position.enPassant.column}` : '-';
  return `${rows.join('/')} ${position.turn} ${castling || '-'} ${ep}`;
}

// What the position is: { over, result: 'white' | 'black' | 'draw' | null, reason, check }.
// `history` is the keys of the positions before (positionKey), for the threefold repetition.
function status(position, history = []) {
  const check = inCheck(position, position.turn);
  const moves = legalMoves(position);
  if (moves.length === 0) {
    if (check) {
      return { over: true, result: other(position.turn), reason: 'checkmate', check };
    }
    return { over: true, result: 'draw', reason: 'stalemate', check };
  }
  if (insufficientMaterial(position)) {
    return { over: true, result: 'draw', reason: 'insufficient material', check };
  }
  if (position.halfmoves >= 100) {
    return { over: true, result: 'draw', reason: 'the fifty-move rule', check };
  }
  const key = positionKey(position);
  if (history.filter((seen) => seen === key).length >= 2) {
    return { over: true, result: 'draw', reason: 'threefold repetition', check };
  }
  return { over: false, result: null, reason: '', check };
}

// A move in standard algebraic notation: "e4", "Nxf7+", "O-O", "exd8=Q#".
function notation(position, move) {
  const file = (column) => 'abcdefgh'[column];
  let text;
  if (move.castle) {
    text = move.castle === 'king' ? 'O-O' : 'O-O-O';
  } else {
    const target = `${file(move.to.column)}${move.to.row + 1}`;
    let from = '';
    if (move.kind === 'p') {
      from = move.capture ? file(move.from.column) : '';
    } else {
      // The file or the rank when another piece of the kind could go there too.
      const rivals = legalMoves(position).filter((other) => {
        return other.kind === move.kind && same(other.to, move.to) && !same(other.from, move.from);
      });
      if (rivals.length > 0) {
        const sameFile = rivals.some((other) => other.from.column === move.from.column);
        const sameRank = rivals.some((other) => other.from.row === move.from.row);
        if (!sameFile) {
          from = file(move.from.column);
        } else if (!sameRank) {
          from = String(move.from.row + 1);
        } else {
          from = `${file(move.from.column)}${move.from.row + 1}`;
        }
      }
    }
    const takes = move.capture ? 'x' : '';
    const promotion = move.promotion ? `=${LETTERS[move.promotion]}` : '';
    text = `${LETTERS[move.kind]}${from}${takes}${target}${promotion}`;
  }
  const after = play(position, move);
  const mark = inCheck(after, after.turn) ? (legalMoves(after).length === 0 ? '#' : '+') : '';
  return text + mark;
}

// Positions from a FEN record (for tests and for a game kept across restarts).
function fromFen(fen) {
  const [placement, turn, castling, passant, halfmoves = '0', fullmoves = '1'] = fen.split(' ');
  const board = [];
  for (let row = 0; row < SIZE; row++) {
    board.push(new Array(SIZE).fill(null));
  }
  placement.split('/').forEach((rank, index) => {
    const row = SIZE - 1 - index;
    let column = 0;
    for (const letter of rank) {
      if (/[1-8]/.test(letter)) {
        column += Number(letter);
        continue;
      }
      const color = letter === letter.toUpperCase() ? 'white' : 'black';
      board[row][column] = { kind: letter.toLowerCase(), color };
      column += 1;
    }
  });
  const enPassant =
    passant && passant !== '-'
      ? cell(Number(passant[1]) - 1, 'abcdefgh'.indexOf(passant[0]))
      : null;
  return {
    board,
    turn: turn === 'b' ? 'black' : 'white',
    castling: {
      white: { king: castling.includes('K'), queen: castling.includes('Q') },
      black: { king: castling.includes('k'), queen: castling.includes('q') },
    },
    enPassant,
    halfmoves: Number(halfmoves),
    fullmoves: Number(fullmoves),
  };
}

function toFen(position) {
  const ranks = [];
  for (let row = SIZE - 1; row >= 0; row--) {
    let text = '';
    let empty = 0;
    for (let column = 0; column < SIZE; column++) {
      const square = position.board[row][column];
      if (!square) {
        empty += 1;
        continue;
      }
      if (empty > 0) {
        text += String(empty);
        empty = 0;
      }
      text += square.color === 'white' ? square.kind.toUpperCase() : square.kind;
    }
    if (empty > 0) {
      text += String(empty);
    }
    ranks.push(text);
  }
  const rights = position.castling;
  const castling =
    [
      rights.white.king ? 'K' : '',
      rights.white.queen ? 'Q' : '',
      rights.black.king ? 'k' : '',
      rights.black.queen ? 'q' : '',
    ].join('') || '-';
  const passant = position.enPassant
    ? `${'abcdefgh'[position.enPassant.column]}${position.enPassant.row + 1}`
    : '-';
  const turn = position.turn === 'white' ? 'w' : 'b';
  return `${ranks.join('/')} ${turn} ${castling} ${passant} ${position.halfmoves} ${
    position.fullmoves
  }`;
}

module.exports = {
  initialPosition,
  legalMoves,
  findMove,
  play,
  status,
  inCheck,
  kingOf,
  positionKey,
  notation,
  fromFen,
  toFen,
  pieceAt,
};
