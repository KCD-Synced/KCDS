'use strict';

// Decks of pieces players hold in their hands at a Tabletop (the server API's TabletopDeck and
// DeckPiece): playing cards by default, or any set of flat pieces a resource ships
// (tarot, dominoes, tiles). A game's rules say what happens; the deck does the rest:
//
//   - the stack lies face down on the table; shuffle, deal (each piece flies to the player's
//     hand), dealTo (to a pile on the table: a poker flop);
//   - a hand is a fan of pieces in the player's left hand, in the game's hand slot, the fingers
//     closed on it: its faces only the holder's game ever gets, everyone else's game gets the
//     back model on the same place;
//   - the holder points a piece (left and right), selects pieces (they rise out of the fan) and
//     plays them: the right arm throws them, they fly to a pile and land there;
//   - reveal shows a hand's faces to everyone in the player's hand, cover lays the hand face
//     down on the table before the player with their hand on it, uncover takes it up again;
//   - piles on the table hold pieces face up or down, stacked or in a row; collect brings every
//     piece back into the stack; save and load keep a deck.
//
// The models: a folder with <id>.cgf per piece and back.cgf. KCDS's own playing cards are
// objects/kcds_cards (in the client's asset pak); a resource's own are objects/kcds/<name>/...
// (Dev/tools/assets/cards/make_cards.py draws a deck of cards). A face-down piece is always the
// back model, so no game learns what it is. Flights glide in the players' games (Prop.glideTo):
// the server sends where to, not every step.
//
// The players' games (Client/resources/tabletop.cjs) get { op: 'hand', table, deck, count,
// covered } on the table's event; a seated holder's keys send { op: 'deck', table, action }:
// 'prev', 'next', 'select', 'play', 'cover', 'look'.
//
// Events (all on the server):
//   deckSelect(deck, player, piece, selected)   refusable: the holder (un)selects a piece
//   deckPlayRequest(deck, player, pieces)       refusable: the holder plays the selected
//                                               pieces; otherwise they go to the playPile
//   deckCover(deck, player, covered)            refusable: the holder covers or takes up the hand

const { Vector3, Quaternion } = require('../math.cjs');

const RANKS = 'A23456789TJQK';
const SUITS = 'SHDC';
const JOKERS = ['RJ', 'BJ'];
const DEFAULT_MODELS = 'objects/kcds_cards';
const MODEL_FOLDER = /^[A-Za-z0-9_][A-Za-z0-9_/-]{0,127}$/;
const PIECE_ID = /^[A-Za-z0-9_]{1,16}$/;
const PILE_NAME = /^[A-Za-z0-9_-]{1,32}$/;
const MAX_PIECES = 216;
const MAX_DECKS = 4;
const MAX_PILES = 24;
const LAYOUTS = ['stack', 'row'];

// A card is 64 x 89 mm and 0.3 mm thick; stacked pieces lie this far apart.
const PIECE_THICK = 0.0004;
const STACK_PROPS = 12;
const FLY_MS = 450;
const DEAL_GAP_MS = 160;
const FLY_ARC = 0.12;

// The hold: the body sits as the game's dice players sit at their table, the
// forearms on it (DEFAULT_HOLD_ANIMATION), and HOLD_POSE raises the left hand before the chest,
// the palm towards the eyes. The turns were solved from the joints a game draws in that sitting
// (where each is and how it is turned) for the wrist 27 cm ahead of the shoulder and 10 cm under
// it; on top of another hold animation they put the hand elsewhere.
// Held still (speed 0) 3 s in, where both forearms rest: playing, the dice game's idle turns the
// right hand over and swings it before the eyes about 1 s and 5 s in. At
// 0 s, held still, the game shows nothing of it and the body stands up.
const DEFAULT_HOLD_ANIMATION = { fragment: 'DiceGameIdle', tags: 'sitting', speed: 0, time: 3 };
// The fan hangs on the left hand's own joint (LeftHand), which the turns move (the hand's item
// slot, LeftHandSlot, keeps where the animation puts it). On the joint's axes (x along the
// fingers, y out of the back of the hand, the thumb towards z): the pieces lie on the inside of
// the fingers, their faces towards the palm and so towards the eyes, their tops along the
// fingers. FAN_TURN turns a piece's own axes (x across, y up, z out of its face) onto the joint's.
const FAN_TURN = Object.freeze({ w: 0.5, x: 0.5, y: 0.5, z: -0.5 });
// Where the thumb holds the fan, on the joint's axes: in the palm, a little out of it.
const FAN_GRIP = Object.freeze({ x: 0.05, y: -0.02, z: 0 });
// Each piece turns about the grip by its share of the spread, its middle FAN_REACH from the grip
// along its own up; a pointed piece rises a little, a selected one more. Later pieces lie nearer
// the eyes.
// Wide enough that each piece's corner shows beside the next (12 degrees hid them, live).
const FAN_DEGREES = 18;
const FAN_MAX = 100;
const FAN_REACH = 0.04;
// High enough to show above the thumb and the other pieces (2 cm and less did not, live).
const FOCUS_RISE = 0.03;
const SELECT_RISE = 0.055;
const FAN_DEPTH = 0.0006;
// Where the fan is with the hold pose, from the player's position on their own axes (x right,
// y ahead, z up): the pieces fly from and to here.
const HAND_POINT = { x: -0.05, y: 0.41, z: 0.93 };
// The hold pose: degrees about each joint's parent's axes, on top of the hold animation. The
// fingers stay nearly straight behind the pieces and the thumb lies across their faces.
const FINGERS_STRAIGHT = [0, 0, -8];
const THUMB_ON = [0, 15, 0];
const HOLD_POSE = Object.freeze({
  LeftArm: [12.2, 5.1, -25.2],
  LeftForeArm: [-7.2, -21.3, -59.5],
  LeftHand: [-64, -14, -0.4],
  LeftHandIndex1: FINGERS_STRAIGHT,
  LeftHandMiddle1: FINGERS_STRAIGHT,
  LeftHandRing1: FINGERS_STRAIGHT,
  LeftHandPinky1: FINGERS_STRAIGHT,
  LeftHandThumb1: THUMB_ON,
  LeftHandThumb2: THUMB_ON,
});
// The throw, solved as the hold: the right hand reaches the fan's corner (its wrist 6 cm right
// of the body's middle, 40 cm ahead), takes the piece between thumb and index, and throws it
// over the table (the wrist 61 cm ahead, the fingers down) with the fingers open.
const REACH = Object.freeze({
  RightArm: [2.4, -2.6, -20.3],
  RightForeArm: [-10.1, -35.3, -13.6],
  RightHand: [-28.1, 3.9, -22.7],
});
const PINCH = Object.freeze({
  RightHandThumb1: [0, 0, -25],
  RightHandIndex1: [0, 0, -35],
  RightHandIndex2: [0, 0, -30],
});
const THROWN = Object.freeze({
  RightArm: [-35, -14.5, -9.4],
  RightForeArm: [-26.8, 40.6, -32.5],
  RightHand: [-12.5, 41.7, -6.7],
});
const THROW_KEYS = Object.freeze([
  { time: 0, pose: { ...HOLD_POSE } },
  { time: 0.3, pose: { ...HOLD_POSE, ...REACH } },
  { time: 0.42, pose: { ...HOLD_POSE, ...REACH, ...PINCH } },
  { time: 0.72, pose: { ...HOLD_POSE, ...THROWN } },
  { time: 1.1, pose: { ...HOLD_POSE } },
]);
// When the right hand has the piece, when it lets go, and when the throw is over.
const THROW_TAKE_MS = 400;
const THROW_RELEASE_MS = 680;
const THROW_MS = 1100;
// The piece between the right hand's thumb and index (RightHand's axes point the other way
// round: its fingers along -x): its face towards the palm, its top along the fingers.
const PINCH_TURN = Object.freeze({ w: 0.5, x: 0.5, y: -0.5, z: 0.5 });
const PINCH_AT = Object.freeze({ x: -0.1, y: -0.01, z: 0 });
// Where the thrown piece leaves the hand, on the player's axes.
const RELEASE_POINT = { x: 0.07, y: 0.69, z: 0.81 };
// Covering: the hold pose is let go, so the left forearm lies on the table again as the hold
// animation has it, and the pieces lie face down under that hand (on the player's axes).
const COVER_POSE = Object.freeze({ LeftHandIndex1: [0, 0, -4] });
const COVER_POINT = { x: 0.08, y: 0.3 };
// The holder's keys besides pointing and playing, shown as the game's own key hints: draw (E,
// deckDrawRequest: a piece from the stack), pass (Space, deckPass: a check, a stand, a pass, as
// the game names it) and look (holding E: the close looks at the hand and the table, as C).
// Off with false.
const DEFAULT_ACTIONS = Object.freeze({ draw: false, pass: 'Pass', look: 'Look closer' });
const ACTION_LABEL_MAX = 48;
// The close look at the table: this high over the table, this far from its middle towards the
// player's seat, looking at the middle.
const LOOK_HEIGHT = 0.55;
const LOOK_BACK = 0.22;
const SEAT_REACH = 0.3;
// The close look at the hand: from this point on the player's axes (between the eyes and the fan,
// on the line the pieces' faces turn to) at HAND_POINT.
const HAND_LOOK_FROM = { x: -0.04, y: 0.24, z: 1.13 };

function standardCards(jokers, ranks = RANKS) {
  const ids = [];
  for (const suit of SUITS) {
    for (const rank of ranks) {
      ids.push(rank + suit);
    }
  }
  return jokers ? [...ids, ...JOKERS] : ids;
}

// The ranks of a shorter standard deck (Prší's and Mariáš's 32 cards: 7 to the ace), all of them
// when none are given.
function ranksOf(value) {
  if (value === undefined) {
    return RANKS;
  }
  const known = [...RANKS];
  if (!Array.isArray(value) || value.length === 0 || !value.every((rank) => known.includes(rank))) {
    throw new TypeError(`TabletopDeck: ranks is a list of ${known.join(' ')}`);
  }
  return [...new Set(value)];
}

// The holder's draw and pass keys: a label each, or false for no such key.
function actionsOf(value, base, name) {
  if (value === undefined) {
    return { ...base };
  }
  if (value === null || typeof value !== 'object') {
    throw new TypeError(`TabletopDeck: ${name} is { draw, pass, look }`);
  }
  const actions = { ...base };
  for (const [key, label] of Object.entries(value)) {
    if (!Object.hasOwn(DEFAULT_ACTIONS, key)) {
      throw new TypeError(`TabletopDeck: ${name} has draw, pass and look, not ${key}`);
    }
    const good =
      label === false ||
      (typeof label === 'string' && label.length > 0 && label.length <= ACTION_LABEL_MAX);
    if (!good) {
      throw new TypeError(
        `TabletopDeck: ${name}.${key} is a text of up to ${ACTION_LABEL_MAX} characters, or false`,
      );
    }
    actions[key] = label;
  }
  return actions;
}

function finite(value, name) {
  const number = Number(value);
  if (!Number.isFinite(number)) {
    throw new TypeError(`TabletopDeck: ${name} must be a number`);
  }
  return number;
}

// A point on the table's own axes: { x, y } in metres from its middle.
function spotOf(value, fallback, name) {
  if (value === undefined) {
    return { ...fallback };
  }
  if (!value || typeof value !== 'object') {
    throw new TypeError(`TabletopDeck: ${name} is { x, y } on the table`);
  }
  const x = Array.isArray(value) ? value[0] : value.x;
  const y = Array.isArray(value) ? value[1] : value.y;
  const spot = { x: finite(x, `${name}.x`), y: finite(y, `${name}.y`) };
  if (Math.hypot(spot.x, spot.y) > 4) {
    throw new RangeError(`TabletopDeck: ${name} is at most 4 m from the table's middle`);
  }
  return spot;
}

function yawTurn(degrees) {
  return Quaternion.fromAxisAngle(new Vector3(0, 0, 1), (degrees * Math.PI) / 180);
}

function plain(turn) {
  return { w: turn.w, x: turn.x, y: turn.y, z: turn.z };
}

class TabletopDecks {
  constructor(tabletops) {
    this.tabletops = tabletops;
    this.host = tabletops.host;
    this.decks = new Map();
    this.nextId = 1;
    this.byHandle = new WeakMap();
    this.pieceByHandle = new WeakMap();
    // `${table}:${player}` of the players looking close up -> 'hand' or 'table'.
    this.lookers = new Map();
  }

  get api() {
    return this.host.api;
  }

  // --- the classes -------------------------------------------------------------------------------

  defineDeck() {
    this.Deck ??= this.makeDeckClass();
    return this.Deck;
  }

  definePiece() {
    this.Piece ??= this.makePieceClass();
    return this.Piece;
  }

  makeDeckClass() {
    const registry = this;
    class TabletopDeck {
      get id() {
        return registry.stateOf(this).id;
      }
      get table() {
        return registry.stateOf(this).table.handle;
      }
      // Every piece of the deck, wherever it is.
      get pieces() {
        return registry.stateOf(this).pieces.map((piece) => piece.handle);
      }
      // How many pieces are left in the stack.
      get remaining() {
        return registry.stateOf(this).stack.length;
      }
      get piles() {
        return [...registry.stateOf(this).piles.keys()];
      }
      get exists() {
        const deck = registry.byHandle.get(this);
        return Boolean(deck) && registry.decks.get(deck.id) === deck;
      }
      shuffle() {
        registry.shuffle(registry.stateOf(this));
      }
      deal(players, count = 1) {
        return registry.deal(registry.stateOf(this), players, count);
      }
      dealTo(pile, count = 1, options = {}) {
        return registry.dealTo(registry.stateOf(this), pile, count, options);
      }
      hand(player) {
        return registry.handOf(registry.stateOf(this), player).map((piece) => piece.handle);
      }
      selected(player) {
        return registry
          .handOf(registry.stateOf(this), player)
          .filter((piece) => piece.selected)
          .map((piece) => piece.handle);
      }
      select(piece, selected = true) {
        registry.select(registry.stateOf(this), registry.pieceState(piece), selected);
      }
      play(player, pieces, pile, options = {}) {
        return registry.play(registry.stateOf(this), player, pieces, pile, options);
      }
      addPile(name, options = {}) {
        registry.addPile(registry.stateOf(this), name, options);
      }
      pile(name) {
        return registry.pileOf(registry.stateOf(this), name).pieces.map((piece) => piece.handle);
      }
      move(piece, pile, options = {}) {
        return registry.move(registry.stateOf(this), registry.pieceState(piece), pile, options);
      }
      flip(piece, faceUp) {
        registry.flip(registry.stateOf(this), registry.pieceState(piece), faceUp);
      }
      reveal(player) {
        registry.reveal(registry.stateOf(this), player, true);
      }
      conceal(player) {
        registry.reveal(registry.stateOf(this), player, false);
      }
      cover(player) {
        return registry.cover(registry.stateOf(this), player, true);
      }
      uncover(player) {
        return registry.cover(registry.stateOf(this), player, false);
      }
      covered(player) {
        return registry.stateOf(this).covered.has(player?.id);
      }
      collect() {
        return registry.collect(registry.stateOf(this));
      }
      restock(pile, options = {}) {
        return registry.restock(registry.stateOf(this), pile, options);
      }
      // The labels of the holder's draw and pass keys ({ draw, pass }: a text, or false for no
      // such key); for one player only, or the deck's own for everyone.
      setActions(actions, player) {
        registry.setActions(registry.stateOf(this), actions, player);
      }
      actions(player) {
        return { ...registry.actionsFor(registry.stateOf(this), player) };
      }
      save() {
        return registry.save(registry.stateOf(this));
      }
      load(saved) {
        registry.load(registry.stateOf(this), saved);
      }
      destroy() {
        registry.destroy(registry.stateOf(this));
      }
      toString() {
        return `TabletopDeck(${this.id})`;
      }
    }
    return TabletopDeck;
  }

  makePieceClass() {
    const registry = this;
    class DeckPiece {
      get id() {
        return registry.pieceState(this).id;
      }
      // A playing card's rank ('A', '2' ... '9', 'T', 'J', 'Q', 'K'), else null.
      get rank() {
        return registry.pieceState(this).rank;
      }
      // A playing card's suit ('S', 'H', 'D' or 'C'), else null.
      get suit() {
        return registry.pieceState(this).suit;
      }
      get deck() {
        return registry.pieceState(this).deck.handle;
      }
      // 'stack', 'hand', 'pile', or 'moving' while it flies.
      get place() {
        return registry.pieceState(this).place;
      }
      get holder() {
        const piece = registry.pieceState(this);
        return piece.holder === null ? null : registry.player(piece.holder);
      }
      get pile() {
        return registry.pieceState(this).pile?.name ?? null;
      }
      get faceUp() {
        return registry.pieceState(this).faceUp;
      }
      get selected() {
        return registry.pieceState(this).selected;
      }
      // The game's own data on the piece (a value, who played it); saved with the deck.
      get data() {
        return registry.pieceState(this).data;
      }
      set data(value) {
        registry.pieceState(this).data = value ?? {};
      }
      toString() {
        return `DeckPiece(${this.id})`;
      }
    }
    return DeckPiece;
  }

  stateOf(handle) {
    const deck = this.byHandle.get(handle);
    if (!deck || this.decks.get(deck.id) !== deck) {
      throw new Error('TabletopDeck: the deck is gone');
    }
    return deck;
  }

  pieceState(handle) {
    const piece = this.pieceByHandle.get(handle);
    if (!piece || this.decks.get(piece.deck.id) !== piece.deck) {
      throw new TypeError('TabletopDeck: not a piece of a deck that is still there');
    }
    return piece;
  }

  player(id) {
    return this.host.players.get(id) ?? null;
  }

  isPlayer(value) {
    return value instanceof this.api.Player;
  }

  here(deck) {
    return this.decks.get(deck.id) === deck;
  }

  // --- making a deck ---------------------------------------------------------------------------

  create(table, options = {}) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('table.createDeck takes options');
    }
    if (this.decksOf(table).length >= MAX_DECKS) {
      throw new RangeError(`TabletopDeck: at most ${MAX_DECKS} decks on a table`);
    }
    if (options.pieces !== undefined && options.ranks !== undefined) {
      throw new TypeError('TabletopDeck: takes pieces or ranks, not both');
    }
    const ids = options.pieces ?? standardCards(options.jokers === true, ranksOf(options.ranks));
    if (!Array.isArray(ids) || ids.length === 0 || ids.length > MAX_PIECES) {
      throw new RangeError(`TabletopDeck: pieces is a list of 1 to ${MAX_PIECES} ids`);
    }
    for (const id of ids) {
      if (typeof id !== 'string' || !PIECE_ID.test(id)) {
        throw new TypeError("TabletopDeck: a piece's id is 1 to 16 letters, digits or _");
      }
    }
    const models = options.models ?? DEFAULT_MODELS;
    if (typeof models !== 'string' || !MODEL_FOLDER.test(models) || models.includes('..')) {
      throw new TypeError(
        "TabletopDeck: models is a folder of the game's files (objects/kcds/<name>)",
      );
    }
    const hold =
      options.holdAnimation === undefined ? DEFAULT_HOLD_ANIMATION : options.holdAnimation;
    if (hold !== null && (typeof hold !== 'object' || typeof hold.fragment !== 'string')) {
      throw new TypeError('TabletopDeck: holdAnimation is { fragment, tags } or null');
    }
    const deck = {
      id: this.nextId,
      table,
      models: models.replace(/\/+$/, '').toLowerCase(),
      pieces: [],
      stack: [],
      hands: new Map(),
      focus: new Map(),
      covered: new Set(),
      posed: new Set(),
      busy: new Set(),
      piles: new Map(),
      stackAt: spotOf(options.position, { x: -0.12, y: 0 }, 'position'),
      height: options.height === undefined ? table.height : finite(options.height, 'height'),
      hold,
      playPile: options.playPile ?? 'table',
      stackProps: [],
      timers: new Set(),
      actions: actionsOf(options.actions, DEFAULT_ACTIONS, 'actions'),
      playerActions: new Map(),
    };
    this.nextId += 1;
    ids.forEach((id, index) => {
      const card = id.length === 2 && RANKS.includes(id[0]) && SUITS.includes(id[1]);
      const piece = {
        key: index + 1,
        id,
        rank: card ? id[0] : null,
        suit: card ? id[1] : null,
        deck,
        place: 'stack',
        holder: null,
        pile: null,
        faceUp: false,
        revealed: false,
        selected: false,
        data: {},
        props: [],
        handle: Object.create(this.definePiece().prototype),
      };
      this.pieceByHandle.set(piece.handle, piece);
      deck.pieces.push(piece);
      deck.stack.push(piece);
    });
    deck.handle = Object.create(this.defineDeck().prototype);
    this.byHandle.set(deck.handle, deck);
    this.decks.set(deck.id, deck);
    this.addPile(deck, deck.playPile, { position: options.playPosition ?? { x: 0.06, y: 0 } });
    this.drawStack(deck);
    this.tabletops.announce(table);
    for (const seat of table.seats) {
      const seated = seat.playerId === null ? null : this.player(seat.playerId);
      if (seated) {
        this.sendHand(deck, seated);
      }
    }
    return deck.handle;
  }

  decksOf(table) {
    return [...this.decks.values()].filter((deck) => deck.table === table);
  }

  // --- places on the table -----------------------------------------------------------------------

  addPile(deck, name, options = {}) {
    if (typeof name !== 'string' || !PILE_NAME.test(name)) {
      throw new TypeError('TabletopDeck.addPile: a name is 1 to 32 letters, digits, _ or -');
    }
    if (options === null || typeof options !== 'object') {
      throw new TypeError(
        'TabletopDeck.addPile takes { position, faceUp, layout, spacing, rotation }',
      );
    }
    if (!deck.piles.has(name) && deck.piles.size >= MAX_PILES) {
      throw new RangeError(`TabletopDeck: at most ${MAX_PILES} piles`);
    }
    const layout = options.layout ?? 'stack';
    if (!LAYOUTS.includes(layout)) {
      throw new TypeError(`TabletopDeck.addPile: layout is ${LAYOUTS.join(' or ')}`);
    }
    const old = deck.piles.get(name);
    const pile = {
      name,
      at: spotOf(options.position, old?.at ?? { x: 0, y: 0 }, 'position'),
      faceUp: options.faceUp === undefined ? old?.faceUp ?? true : options.faceUp === true,
      layout,
      spacing: options.spacing === undefined ? 0.07 : finite(options.spacing, 'spacing'),
      turn: options.rotation === undefined ? 0 : finite(options.rotation, 'rotation'),
      pieces: old?.pieces ?? [],
    };
    deck.piles.set(name, pile);
    pile.pieces.forEach((piece) => this.drawPiece(piece));
  }

  pileOf(deck, name) {
    const pile = deck.piles.get(name);
    if (!pile) {
      throw new Error(`TabletopDeck: no pile '${name}'`);
    }
    return pile;
  }

  // A point of the table's top on its own axes, in the world.
  worldAt(deck, x, y, lift = 0) {
    const { table } = deck;
    const turned = table.yaw.rotateVector(new Vector3(x, y, deck.height + lift));
    return new Vector3(table.at.x + turned.x, table.at.y + turned.y, table.at.z + turned.z);
  }

  // Where the n-th piece of a pile lies, and its turn.
  pileSpot(deck, pile, index) {
    let { x, y } = pile.at;
    let lift = (index + 1) * PIECE_THICK;
    let degrees = pile.turn;
    if (pile.layout === 'row') {
      const step = yawTurn(pile.turn).rotateVector(new Vector3(pile.spacing, 0, 0));
      const middle = (Math.max(pile.pieces.length, index + 1) - 1) / 2;
      x += step.x * (index - middle);
      y += step.y * (index - middle);
      lift = PIECE_THICK;
    } else {
      // A thrown stack is not neat: each piece lies a little turned.
      degrees += (index % 5) * 3 - 6;
    }
    return {
      position: this.worldAt(deck, x, y, lift),
      rotation: deck.table.yaw.clone().mul(yawTurn(degrees)),
    };
  }

  // The table's spot before a seat: from the middle towards the seat, and the seat's direction.
  seatSpot(table, player, height) {
    const index = this.tabletops.seatIndex(table, player);
    const seat = index >= 0 ? table.seats[index].at : player.position;
    const back = table.yaw.clone().conjugate();
    const local = back.rotateVector(
      new Vector3(seat.x - table.at.x, seat.y - table.at.y, seat.z - table.at.z),
    );
    const length = Math.hypot(local.x, local.y) || 1;
    return {
      x: (local.x / length) * SEAT_REACH,
      y: (local.y / length) * SEAT_REACH,
      yaw: (Math.atan2(local.y, local.x) * 180) / Math.PI,
      height,
    };
  }

  modelOf(deck, piece, face) {
    return face ? `${deck.models}/${piece.id.toLowerCase()}.cgf` : `${deck.models}/back.cgf`;
  }

  // --- drawing -----------------------------------------------------------------------------------

  clearProps(piece) {
    for (const prop of piece.props) {
      if (prop.exists !== false) {
        prop.destroy();
      }
    }
    piece.props = [];
  }

  spawnProp(deck, model, position, turn, viewers = {}) {
    return this.host.props.spawn(this.api.Prop, {
      model,
      position,
      orientation: plain(turn),
      physics: 'none',
      dimension: deck.table.dimension,
      ...viewers,
    });
  }

  // The stack: a few backs, as high as the pieces it holds.
  drawStack(deck) {
    const want = Math.min(STACK_PROPS, deck.stack.length);
    while (deck.stackProps.length > want) {
      const prop = deck.stackProps.pop();
      if (prop.exists !== false) {
        prop.destroy();
      }
    }
    const step = want > 0 ? (deck.stack.length * PIECE_THICK) / want : 0;
    for (let index = 0; index < want; index += 1) {
      const position = this.worldAt(deck, deck.stackAt.x, deck.stackAt.y, (index + 1) * step);
      const turn = deck.table.yaw.clone().mul(yawTurn((index % 3) - 1));
      if (deck.stackProps[index]) {
        deck.stackProps[index].setPose(position, plain(turn));
      } else {
        deck.stackProps.push(this.spawnProp(deck, this.modelOf(deck, null, false), position, turn));
      }
    }
  }

  // A piece on a pile on the table.
  drawPiece(piece) {
    const { deck } = piece;
    this.clearProps(piece);
    if (piece.place === 'pile') {
      const spot = this.pileSpot(deck, piece.pile, piece.pile.pieces.indexOf(piece));
      const model = this.modelOf(deck, piece, piece.faceUp);
      piece.props.push(this.spawnProp(deck, model, spot.position, spot.rotation));
    }
  }

  // A player's hand: the fan in their left hand, or face down on the table when covered.
  drawHand(deck, player) {
    const hand = deck.hands.get(player.id) ?? [];
    for (const piece of hand) {
      this.clearProps(piece);
    }
    if (hand.length === 0) {
      return;
    }
    if (deck.covered.has(player.id)) {
      const under = this.underHand(deck, player);
      hand.forEach((piece, index) => {
        const position = new Vector3(
          under.position.x,
          under.position.y,
          under.position.z + (index + 1) * PIECE_THICK,
        );
        const turn = under.rotation.clone().mul(yawTurn(index * 4 - hand.length * 2));
        piece.props.push(this.spawnProp(deck, this.modelOf(deck, piece, false), position, turn));
      });
      return;
    }
    const focus = deck.focus.get(player.id) ?? -1;
    const spread = Math.min(FAN_MAX, FAN_DEGREES * (hand.length - 1));
    const base = new Quaternion(FAN_TURN.w, FAN_TURN.x, FAN_TURN.y, FAN_TURN.z);
    hand.forEach((piece, index) => {
      const share = hand.length > 1 ? index / (hand.length - 1) - 0.5 : 0;
      const fan = Quaternion.fromAxisAngle(new Vector3(0, 0, 1), (-share * spread * Math.PI) / 180);
      const turn = base.clone().mul(fan);
      let reach = FAN_REACH;
      if (piece.selected) {
        reach += SELECT_RISE;
      } else if (index === focus) {
        reach += FOCUS_RISE;
      }
      const out = turn.rotateVector(new Vector3(0, reach, index * FAN_DEPTH));
      const options = {
        joint: 'LeftHand',
        position: { x: FAN_GRIP.x + out.x, y: FAN_GRIP.y + out.y, z: FAN_GRIP.z + out.z },
        rotation: plain(turn),
      };
      const face = { ...options, model: this.modelOf(deck, piece, true) };
      if (piece.revealed) {
        piece.props.push(this.host.props.attach(player, face));
        return;
      }
      // The face reaches the holder's game only; everyone else's gets the back.
      piece.props.push(this.host.props.attach(player, face, { audience: new Set([player.id]) }));
      const back = { ...options, model: this.modelOf(deck, piece, false) };
      piece.props.push(this.host.props.attach(player, back, { hiddenIds: new Set([player.id]) }));
    });
  }

  // Tells the holder's game how many pieces they hold, and which keys they have.
  sendHand(deck, player) {
    this.host.emitToPlayerClient(player.id, 'kcds:tabletop', {
      op: 'hand',
      table: deck.table.id,
      deck: deck.id,
      count: (deck.hands.get(player.id) ?? []).length,
      covered: deck.covered.has(player.id),
      actions: this.actionsFor(deck, player),
    });
  }

  actionsFor(deck, player) {
    if (player !== undefined && player !== null && !this.isPlayer(player)) {
      throw new TypeError('TabletopDeck.actions takes a Player or nothing');
    }
    return (player && deck.playerActions.get(player.id)) ?? deck.actions;
  }

  // New labels for the deck (everyone), or for one player (their own over the deck's, until the
  // deck's are set again); each seated player's game shows them at once.
  setActions(deck, value, player) {
    if (player !== undefined && player !== null) {
      if (!this.isPlayer(player)) {
        throw new TypeError('TabletopDeck.setActions takes the labels and a Player or nothing');
      }
      deck.playerActions.set(
        player.id,
        actionsOf(value, this.actionsFor(deck, player), 'setActions'),
      );
    } else {
      deck.actions = actionsOf(value, deck.actions, 'setActions');
      deck.playerActions.clear();
    }
    for (const seat of deck.table.seats) {
      const seated = seat.playerId === null ? null : this.player(seat.playerId);
      if (seated && (!player || seated === player)) {
        this.sendHand(deck, seated);
      }
    }
  }

  // A player sat at the table: their game learns the deck's keys.
  playerSat(table, player) {
    for (const deck of this.decksOf(table)) {
      this.sendHand(deck, player);
    }
  }

  // The hold pose while a player has pieces, off when the hand is empty.
  pose(deck, player) {
    const holding = (deck.hands.get(player.id) ?? []).length > 0;
    if (holding && !deck.posed.has(player.id)) {
      deck.posed.add(player.id);
      if (deck.hold) {
        const { fragment, ...rest } = deck.hold;
        player.playAnimation(fragment, { loop: true, lockMovement: true, ...rest });
      }
      this.setStill(deck, player);
    } else if (!holding && deck.posed.has(player.id) && !deck.busy.has(player.id)) {
      // While they throw, the throw's end lets the hand down (play).
      this.unpose(deck, player);
    }
  }

  unpose(deck, player) {
    deck.posed.delete(player.id);
    deck.covered.delete(player.id);
    player.setPoseAnimation(null);
    if (deck.hold) {
      player.stopAnimation();
    }
  }

  setStill(deck, player) {
    const still = deck.covered.has(player.id) ? COVER_POSE : HOLD_POSE;
    player.setPoseAnimation({
      keys: [
        { time: 0, pose: still },
        { time: 1, pose: still },
      ],
      loop: true,
    });
  }

  handChanged(deck, player) {
    const hand = deck.hands.get(player.id) ?? [];
    const focus = deck.focus.get(player.id) ?? 0;
    deck.focus.set(player.id, Math.max(0, Math.min(hand.length - 1, focus)));
    this.drawHand(deck, player);
    this.pose(deck, player);
    this.sendHand(deck, player);
  }

  // --- moving pieces -----------------------------------------------------------------------------

  // A piece flies from one point to another: a prop of its own (the back unless it shows face up)
  // that glides in the players' games on an arc and is gone when it lands. Resolves true when it
  // landed with the deck still there.
  fly(deck, piece, from, to, face, turnTo) {
    const prop = this.spawnProp(deck, this.modelOf(deck, piece, face), from, deck.table.yaw);
    prop.glideTo(to, plain(turnTo), { ms: FLY_MS, arc: FLY_ARC });
    return this.later(deck, FLY_MS, () => true).then((landed) => {
      if (prop.exists !== false) {
        prop.destroy();
      }
      return landed;
    });
  }

  // Runs `work` after `ms`, unless the deck is gone by then (false).
  later(deck, ms, work) {
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        deck.timers.delete(timer);
        resolve(this.here(deck) ? work() : false);
      }, ms);
      deck.timers.add(timer);
    });
  }

  // A point on the player's own axes (x right, y ahead, z up) from where they are, in the world.
  bodyPoint(player, { x, y, z }) {
    const turned = player.rotation.rotateVector(new Vector3(x, y, z));
    const at = player.position;
    return new Vector3(at.x + turned.x, at.y + turned.y, at.z + turned.z);
  }

  handPoint(player) {
    return this.bodyPoint(player, HAND_POINT);
  }

  // Where a covered hand lies: on the table's top under the player's left hand (as the hold
  // animation lays it on the table), its long side along the forearm.
  underHand(deck, player) {
    const point = this.bodyPoint(player, { ...COVER_POINT, z: 0 });
    const top = this.worldAt(deck, 0, 0, 0).z;
    return {
      position: new Vector3(point.x, point.y, top),
      rotation: player.rotation.clone().mul(yawTurn(90)),
    };
  }

  stackTop(deck) {
    return this.worldAt(
      deck,
      deck.stackAt.x,
      deck.stackAt.y,
      (deck.stack.length + 1) * PIECE_THICK,
    );
  }

  takeFromStack(deck) {
    const piece = deck.stack.pop() ?? null;
    if (piece) {
      piece.place = 'moving';
      this.drawStack(deck);
    }
    return piece;
  }

  // Takes it from wherever it is (a hand, a pile, the stack), drawn nowhere, 'moving'.
  lift(piece) {
    const { deck } = piece;
    const holder = piece.place === 'hand' ? this.player(piece.holder) : null;
    if (piece.place === 'hand') {
      const hand = deck.hands.get(piece.holder) ?? [];
      hand.splice(hand.indexOf(piece), 1);
    } else if (piece.place === 'pile') {
      piece.pile.pieces.splice(piece.pile.pieces.indexOf(piece), 1);
    } else if (piece.place === 'stack') {
      deck.stack.splice(deck.stack.indexOf(piece), 1);
    }
    const pile = piece.pile;
    this.clearProps(piece);
    piece.place = 'moving';
    piece.holder = null;
    piece.pile = null;
    piece.selected = false;
    piece.revealed = false;
    if (holder) {
      this.handChanged(deck, holder);
    }
    if (pile?.layout === 'row') {
      pile.pieces.forEach((other) => this.drawPiece(other));
    }
    this.drawStack(deck);
  }

  intoHand(deck, piece, player) {
    piece.place = 'hand';
    piece.holder = player.id;
    piece.faceUp = false;
    deck.hands.set(player.id, [...(deck.hands.get(player.id) ?? []), piece]);
    this.handChanged(deck, player);
  }

  intoPile(deck, piece, pile, faceUp) {
    piece.place = 'pile';
    piece.pile = pile;
    piece.faceUp = faceUp;
    pile.pieces.push(piece);
    if (pile.layout === 'row') {
      pile.pieces.forEach((other) => this.drawPiece(other));
    } else {
      this.drawPiece(piece);
    }
  }

  intoStack(deck, piece, bottom = false) {
    piece.place = 'stack';
    piece.faceUp = false;
    if (bottom) {
      deck.stack.unshift(piece);
    } else {
      deck.stack.push(piece);
    }
    this.drawStack(deck);
  }

  shuffle(deck) {
    for (let index = deck.stack.length - 1; index > 0; index -= 1) {
      const other = Math.floor(Math.random() * (index + 1));
      [deck.stack[index], deck.stack[other]] = [deck.stack[other], deck.stack[index]];
    }
  }

  playersOf(value) {
    const players = [value].flat();
    if (players.length === 0 || !players.every((player) => this.isPlayer(player))) {
      throw new TypeError('TabletopDeck.deal takes a Player or a list of them');
    }
    return players;
  }

  countOf(count) {
    const number = finite(count, 'count');
    if (!Number.isInteger(number) || number < 1 || number > MAX_PIECES) {
      throw new RangeError(`TabletopDeck: count is a whole number from 1 to ${MAX_PIECES}`);
    }
    return number;
  }

  // Round by round, one piece to each player in turn: each flies from the stack into their hand.
  // Returns the pieces dealt at once; they are in the hands when they land.
  deal(deck, value, count) {
    const players = this.playersOf(value);
    const rounds = this.countOf(count);
    const dealt = [];
    let gap = 0;
    for (let round = 0; round < rounds; round += 1) {
      for (const player of players) {
        const piece = this.takeFromStack(deck);
        if (!piece) {
          return dealt;
        }
        dealt.push(piece.handle);
        const from = this.stackTop(deck);
        this.later(deck, gap, () =>
          this.fly(deck, piece, from, this.handPoint(player), false, player.rotation).then((ok) => {
            if (!ok) {
              return;
            }
            if (this.host.players.get(player.id) === player) {
              this.intoHand(deck, piece, player);
            } else {
              this.intoStack(deck, piece, true);
            }
          }),
        );
        gap += DEAL_GAP_MS;
      }
    }
    return dealt;
  }

  // From the stack onto a pile: { faceUp } (the pile's own by default).
  dealTo(deck, name, count, options = {}) {
    const pile = this.pileOf(deck, name);
    const many = this.countOf(count);
    const faceUp = options.faceUp === undefined ? pile.faceUp : options.faceUp === true;
    const dealt = [];
    const landing = pile.pieces.length;
    let gap = 0;
    for (let index = 0; index < many; index += 1) {
      const piece = this.takeFromStack(deck);
      if (!piece) {
        break;
      }
      dealt.push(piece.handle);
      const from = this.stackTop(deck);
      const spot = this.pileSpot(deck, pile, landing + index);
      this.later(deck, gap, () =>
        this.fly(deck, piece, from, spot.position, false, spot.rotation).then((ok) => {
          if (ok) {
            this.intoPile(deck, piece, pile, faceUp);
          }
        }),
      );
      gap += DEAL_GAP_MS;
    }
    return dealt;
  }

  handOf(deck, player) {
    if (!this.isPlayer(player)) {
      throw new TypeError('TabletopDeck: takes a Player');
    }
    return deck.hands.get(player.id) ?? [];
  }

  select(deck, piece, selected) {
    if (piece.deck !== deck || piece.place !== 'hand') {
      throw new Error("TabletopDeck.select: the piece is in nobody's hand");
    }
    piece.selected = selected === true;
    const holder = this.player(piece.holder);
    if (holder) {
      this.drawHand(deck, holder);
    }
  }

  piecesOf(deck, value) {
    const pieces = [value].flat().map((one) => this.pieceState(one));
    if (!pieces.every((piece) => piece.deck === deck)) {
      throw new TypeError('TabletopDeck: a piece of another deck');
    }
    return pieces;
  }

  // The player throws pieces from their hand onto a pile: their right hand takes them from the
  // fan, swings over the table and lets go, and each piece flies on and lands. { faceUp } (the
  // pile's own by default). Resolves true when they all lie there.
  async play(deck, player, value, name = deck.playPile, options = {}) {
    const pieces = this.piecesOf(deck, value);
    if (!pieces.every((piece) => piece.place === 'hand' && piece.holder === player.id)) {
      throw new Error("TabletopDeck.play: a piece is not in that player's hand");
    }
    const pile = this.pileOf(deck, name);
    const faceUp = options.faceUp === undefined ? pile.faceUp : options.faceUp === true;
    if (deck.covered.delete(player.id)) {
      this.drawHand(deck, player);
      this.sendHand(deck, player);
    }
    deck.busy.add(player.id);
    player.setPoseAnimation({ keys: THROW_KEYS, blend: 'smooth', loop: false });
    // The right hand has them: out of the fan, between its thumb and index.
    let held = [];
    this.later(deck, THROW_TAKE_MS, () => {
      held = pieces.filter((piece) => piece.place === 'hand' && piece.holder === player.id);
      held.forEach((piece, index) => {
        this.lift(piece);
        piece.props.push(this.pinched(deck, player, piece, faceUp, index));
      });
    });
    const flights = pieces.map((piece, index) =>
      this.later(deck, THROW_RELEASE_MS + index * 90, () => {
        if (!held.includes(piece)) {
          return false;
        }
        this.clearProps(piece);
        if (this.host.players.get(player.id) !== player) {
          this.intoStack(deck, piece, true);
          return false;
        }
        const from = this.bodyPoint(player, RELEASE_POINT);
        const spot = this.pileSpot(deck, pile, pile.pieces.length);
        // Face up it shows its face in the air too: everyone sees it land.
        return this.fly(deck, piece, from, spot.position, faceUp, spot.rotation).then((ok) => {
          if (ok) {
            this.intoPile(deck, piece, pile, faceUp);
          }
          return ok;
        });
      }),
    );
    this.later(deck, THROW_MS, () => {
      deck.busy.delete(player.id);
      if (deck.posed.has(player.id) && this.host.players.get(player.id) === player) {
        // The last piece thrown: the hand goes down only now, after the throw.
        if ((deck.hands.get(player.id) ?? []).length === 0) {
          this.unpose(deck, player);
        } else {
          this.setStill(deck, player);
        }
      }
    });
    const landed = await Promise.all(flights);
    return landed.every(Boolean);
  }

  // A thrown piece between the right hand's thumb and index, as everyone sees it: its back, or
  // its face when it is thrown face up.
  pinched(deck, player, piece, faceUp, index) {
    return this.host.props.attach(player, {
      model: this.modelOf(deck, piece, faceUp),
      joint: 'RightHand',
      position: { x: PINCH_AT.x, y: PINCH_AT.y, z: PINCH_AT.z + index * FAN_DEPTH },
      rotation: { ...PINCH_TURN },
    });
  }

  // Any piece to a pile: from the stack, another pile or a hand (without a throw).
  async move(deck, piece, name, options = {}) {
    if (piece.deck !== deck) {
      throw new TypeError('TabletopDeck.move: a piece of another deck');
    }
    if (piece.place === 'moving') {
      return false;
    }
    const pile = this.pileOf(deck, name);
    const faceUp = options.faceUp === undefined ? pile.faceUp : options.faceUp === true;
    const holder = piece.place === 'hand' ? this.player(piece.holder) : null;
    let from = this.stackTop(deck);
    if (holder) {
      from = this.handPoint(holder);
    } else if (piece.props[0]) {
      from = piece.props[0].position;
    }
    const shownBefore = piece.place === 'pile' && piece.faceUp;
    this.lift(piece);
    const spot = this.pileSpot(deck, pile, pile.pieces.length);
    const ok = await this.fly(
      deck,
      piece,
      from,
      spot.position,
      shownBefore && faceUp,
      spot.rotation,
    );
    if (ok) {
      this.intoPile(deck, piece, pile, faceUp);
    }
    return ok;
  }

  flip(deck, piece, faceUp) {
    if (piece.deck !== deck || piece.place !== 'pile') {
      throw new Error('TabletopDeck.flip: only a piece on a pile turns over');
    }
    piece.faceUp = faceUp === undefined ? !piece.faceUp : faceUp === true;
    this.drawPiece(piece);
  }

  // The player's hand shows its faces to everyone (a showdown), or only to them again.
  reveal(deck, player, shown) {
    for (const piece of this.handOf(deck, player)) {
      piece.revealed = shown;
    }
    this.drawHand(deck, player);
  }

  // The hand face down on the table before the player, their hand on it; or taken up again.
  cover(deck, player, covered) {
    const hand = this.handOf(deck, player);
    if (hand.length === 0 || deck.covered.has(player.id) === covered) {
      return false;
    }
    if (covered) {
      deck.covered.add(player.id);
    } else {
      deck.covered.delete(player.id);
    }
    this.drawHand(deck, player);
    this.setStill(deck, player);
    this.sendHand(deck, player);
    return true;
  }

  // Every piece back into the stack, face down; the hands empty. Resolves when they are there.
  async collect(deck) {
    const flights = [];
    for (const piece of deck.pieces) {
      if (piece.place === 'stack' || piece.place === 'moving') {
        continue;
      }
      const holder = piece.place === 'hand' ? this.player(piece.holder) : null;
      let from = piece.props[0]?.position ?? this.stackTop(deck);
      if (holder) {
        from = this.handPoint(holder);
      }
      this.lift(piece);
      const flight = this.fly(deck, piece, from, this.stackTop(deck), false, deck.table.yaw);
      flights.push(
        flight.then((ok) => {
          if (ok) {
            this.intoStack(deck, piece);
          }
        }),
      );
    }
    await Promise.all(flights);
    return this.here(deck);
  }

  // A pile's pieces back into the stack, face down, but its top `keep` (1 at first: Prší's card
  // to follow), the stack shuffled after (`shuffle`, true at first). Resolves how many went.
  async restock(deck, name, options = {}) {
    const pile = this.pileOf(deck, name);
    const keep = options.keep === undefined ? 1 : finite(options.keep, 'keep');
    if (!Number.isInteger(keep) || keep < 0) {
      throw new RangeError('TabletopDeck.restock: keep is a whole number from 0');
    }
    const taken = pile.pieces.slice(0, Math.max(0, pile.pieces.length - keep));
    const flights = taken.map((piece) => {
      const from = piece.props[0]?.position ?? this.stackTop(deck);
      this.lift(piece);
      return this.fly(deck, piece, from, this.stackTop(deck), false, deck.table.yaw).then((ok) => {
        if (ok) {
          this.intoStack(deck, piece, true);
        }
      });
    });
    // What stays lies on the table where the pile starts.
    pile.pieces.forEach((piece) => this.drawPiece(piece));
    await Promise.all(flights);
    if (!this.here(deck)) {
      return 0;
    }
    if (options.shuffle !== false) {
      this.shuffle(deck);
    }
    return taken.length;
  }

  // --- the holder's keys -------------------------------------------------------------------------

  receive(table, player, payload) {
    const action = payload.action;
    if (action === 'look') {
      this.look(table, player);
      return;
    }
    const decks = this.decksOf(table);
    // The deck asked for, else the one they hold pieces of, else the table's first (drawing into
    // an empty hand).
    const deck =
      decks.find((one) => one.id === Number(payload.deck)) ??
      decks.find((one) => one.hands.has(player.id)) ??
      decks[0];
    if (!deck || this.tabletops.seatIndex(table, player) < 0 || deck.busy.has(player.id)) {
      return;
    }
    const hand = deck.hands.get(player.id) ?? [];
    const actions = this.actionsFor(deck, player);
    if (action === 'draw') {
      // Refused, nothing happens; else a piece flies from the stack into their hand.
      if (
        actions.draw !== false &&
        deck.stack.length > 0 &&
        this.host.emit('deckDrawRequest', deck.handle, player)
      ) {
        this.deal(deck, player, 1);
      }
      return;
    }
    if (action === 'pass') {
      if (actions.pass !== false) {
        this.host.emit('deckPass', deck.handle, player);
      }
      return;
    }
    if (action === 'cover') {
      const covered = !deck.covered.has(player.id);
      if (hand.length > 0 && this.host.emit('deckCover', deck.handle, player, covered)) {
        this.cover(deck, player, covered);
      }
      return;
    }
    if (hand.length === 0 || deck.covered.has(player.id)) {
      return;
    }
    const focus = deck.focus.get(player.id) ?? 0;
    if (action === 'prev' || action === 'next') {
      const step = action === 'next' ? 1 : -1;
      deck.focus.set(player.id, (focus + step + hand.length) % hand.length);
      this.drawHand(deck, player);
    } else if (action === 'select') {
      const piece = hand[focus];
      const asked =
        piece && this.host.emit('deckSelect', deck.handle, player, piece.handle, !piece.selected);
      if (asked) {
        this.select(deck, piece, !piece.selected);
      }
    } else if (action === 'play') {
      const chosen = hand.filter((piece) => piece.selected);
      const pieces = chosen.length > 0 ? chosen : [hand[focus]].filter(Boolean);
      const handles = pieces.map((piece) => piece.handle);
      if (pieces.length > 0 && this.host.emit('deckPlayRequest', deck.handle, player, handles)) {
        this.play(deck, player, handles, deck.playPile).catch((error) => {
          this.host.bridge.log('host', `deck play: ${error.message}`);
        });
      }
    }
  }

  // The close looks, one after the other: at the pieces in the hand (when it holds any, face up
  // in front of the camera), then at the table as the game's dice players have, then the seat's
  // own view again.
  look(table, player) {
    const seat = this.tabletops.seatIndex(table, player);
    if (seat < 0) {
      return;
    }
    const key = `${table.id}:${player.id}`;
    const now = this.lookers.get(key);
    const holding = this.decksOf(table).some(
      (deck) => (deck.hands.get(player.id) ?? []).length > 0 && !deck.covered.has(player.id),
    );
    if (now === undefined && holding) {
      this.lookers.set(key, 'hand');
      this.api.Camera.startShot(player, {
        position: this.bodyPoint(player, HAND_LOOK_FROM),
        lookAt: this.handPoint(player),
        fov: 45,
        seconds: 0.4,
      });
      return;
    }
    if (now === 'table') {
      this.lookers.delete(key);
      const shot = this.tabletops.seatShot(table, seat);
      if (shot) {
        this.api.Camera.startShot(player, shot);
      } else {
        this.api.Camera.endShot(player);
      }
      return;
    }
    this.lookers.set(key, 'table');
    const height = this.decksOf(table)[0]?.height ?? table.height;
    const spot = this.seatSpot(table, player, height);
    const length = Math.hypot(spot.x, spot.y) || 1;
    const point = (x, y, lift) => {
      const turned = table.yaw.rotateVector(new Vector3(x, y, height + lift));
      return new Vector3(table.at.x + turned.x, table.at.y + turned.y, table.at.z + turned.z);
    };
    this.api.Camera.startShot(player, {
      position: point((spot.x / length) * LOOK_BACK, (spot.y / length) * LOOK_BACK, LOOK_HEIGHT),
      lookAt: point(0, 0, 0),
      fov: 50,
      seconds: 0.6,
    });
  }

  // --- saving, leaving, removing -----------------------------------------------------------------

  save(deck) {
    const where = (piece) => {
      if (piece.place === 'pile') {
        return { faceUp: piece.faceUp };
      }
      return {};
    };
    return {
      stack: deck.stack.map((piece) => piece.key),
      hands: [...deck.hands].map(([player, hand]) => ({
        player,
        pieces: hand.map((piece) => piece.key),
      })),
      piles: [...deck.piles.values()].map((pile) => ({
        name: pile.name,
        position: { ...pile.at },
        faceUp: pile.faceUp,
        layout: pile.layout,
        spacing: pile.spacing,
        rotation: pile.turn,
        pieces: pile.pieces.map((piece) => piece.key),
      })),
      pieces: deck.pieces.map((piece) => ({
        key: piece.key,
        id: piece.id,
        data: piece.data,
        ...where(piece),
      })),
    };
  }

  load(deck, saved) {
    const valid =
      saved &&
      typeof saved === 'object' &&
      Array.isArray(saved.pieces) &&
      Array.isArray(saved.stack);
    if (!valid) {
      throw new TypeError('TabletopDeck.load takes what save() gave');
    }
    const byKey = new Map(deck.pieces.map((piece) => [piece.key, piece]));
    for (const entry of saved.pieces) {
      if (byKey.get(entry.key)?.id !== entry.id) {
        throw new TypeError('TabletopDeck.load: the saved deck has other pieces');
      }
    }
    const holders = [...deck.hands.keys()];
    for (const piece of deck.pieces) {
      this.clearProps(piece);
      Object.assign(piece, {
        place: null,
        holder: null,
        pile: null,
        selected: false,
        revealed: false,
      });
    }
    deck.hands.clear();
    deck.covered.clear();
    const faceUp = new Map(saved.pieces.map((entry) => [entry.key, entry.faceUp === true]));
    for (const entry of saved.pieces) {
      byKey.get(entry.key).data = entry.data ?? {};
    }
    deck.stack = [];
    for (const key of saved.stack) {
      const piece = byKey.get(key);
      if (piece && piece.place === null) {
        piece.place = 'stack';
        deck.stack.push(piece);
      }
    }
    for (const entry of saved.hands ?? []) {
      const holder = this.player(entry.player);
      for (const key of holder ? entry.pieces ?? [] : []) {
        const piece = byKey.get(key);
        if (piece && piece.place === null) {
          piece.place = 'hand';
          piece.holder = holder.id;
          deck.hands.set(holder.id, [...(deck.hands.get(holder.id) ?? []), piece]);
        }
      }
    }
    for (const entry of saved.piles ?? []) {
      this.addPile(deck, entry.name, entry);
      const pile = deck.piles.get(entry.name);
      pile.pieces = [];
      for (const key of entry.pieces ?? []) {
        const piece = byKey.get(key);
        if (piece && piece.place === null) {
          Object.assign(piece, { place: 'pile', pile, faceUp: faceUp.get(key) ?? pile.faceUp });
          pile.pieces.push(piece);
        }
      }
      pile.pieces.forEach((piece) => this.drawPiece(piece));
    }
    // A piece the save puts nowhere (its holder is not here) goes under the stack.
    for (const piece of deck.pieces) {
      if (piece.place === null) {
        piece.place = 'stack';
        deck.stack.unshift(piece);
      }
    }
    this.drawStack(deck);
    for (const id of new Set([...holders, ...deck.hands.keys()])) {
      const player = this.player(id);
      if (player) {
        this.handChanged(deck, player);
      }
    }
  }

  // A player left the table or the server: their hand goes back under the stack.
  playerLeft(table, player) {
    this.lookers.delete(`${table.id}:${player.id}`);
    const here = this.host.players.get(player.id) === player;
    for (const deck of this.decksOf(table)) {
      for (const piece of deck.hands.get(player.id) ?? []) {
        this.clearProps(piece);
        Object.assign(piece, { holder: null, selected: false, revealed: false });
        this.intoStack(deck, piece, true);
      }
      deck.hands.delete(player.id);
      deck.focus.delete(player.id);
      deck.busy.delete(player.id);
      if (deck.posed.has(player.id)) {
        if (here) {
          this.unpose(deck, player);
          this.sendHand(deck, player);
        } else {
          deck.posed.delete(player.id);
          deck.covered.delete(player.id);
        }
      }
    }
  }

  destroy(deck) {
    if (!this.here(deck)) {
      return;
    }
    for (const timer of deck.timers) {
      clearTimeout(timer);
    }
    deck.timers.clear();
    for (const id of [...deck.hands.keys()]) {
      const player = this.player(id);
      deck.hands.delete(id);
      if (player) {
        this.handChanged(deck, player);
      }
    }
    for (const piece of deck.pieces) {
      this.clearProps(piece);
    }
    for (const prop of deck.stackProps) {
      if (prop.exists !== false) {
        prop.destroy();
      }
    }
    deck.stackProps = [];
    this.decks.delete(deck.id);
    this.tabletops.announce(deck.table);
  }

  tableRemoved(table) {
    for (const deck of this.decksOf(table)) {
      this.destroy(deck);
    }
  }
}

module.exports = { TabletopDecks, standardCards, DEFAULT_MODELS };
