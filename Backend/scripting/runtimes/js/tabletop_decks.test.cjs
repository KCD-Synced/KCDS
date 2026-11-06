'use strict';

// Decks of pieces players hold at a Tabletop (playing cards by default): dealing
// into hands whose faces only the holder's game gets, pointing and selecting from the holder's
// keys, playing onto a pile, covering, revealing, collecting, save and load, a player who leaves,
// and flights that glide in the players' games.

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  serverFixture,
  serverResource,
  delay,
} = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');

const SETUP = `
const { Tabletop } = kcds.world;
globalThis.table = Tabletop.create({
  position: { x: 100, y: 200, z: 30 },
  board: { rows: 1, columns: 1, height: 0.8 },
  seats: [
    { position: { x: 100, y: 199, z: 30 } },
    { position: { x: 100, y: 201, z: 30 } },
  ],
});
globalThis.deck = globalThis.table.createDeck();
const log = (...parts) => console.log(parts.join(' '));
kcds.core.Events.on('deckSelect', (deck, player, piece, selected) => {
  log('select', player.id, piece.id, selected);
  return true;
});
kcds.core.Events.on('deckPlayRequest', (deck, player, pieces) => {
  log('play', player.id, pieces.map((piece) => piece.id).join(','));
  return true;
});
kcds.core.Events.on('deckCover', (deck, player, covered) => log('cover', player.id, covered));
kcds.core.Events.on('deckDrawRequest', (deck, player) => {
  log('draw', player.id);
  return globalThis.refuseDraw !== true;
});
kcds.core.Events.on('deckPass', (deck, player) => log('pass', player.id));
`;

async function setup(t) {
  const f = serverFixture(t);
  serverResource(f.root, 'decks', SETUP);
  await f.host.startAll();
  const vm = f.host.resources.get('decks').vm;
  return {
    ...f,
    table: vm.table,
    deck: vm.deck,
    lines: () => f.linesOf('decks').filter((line) => line !== 'started'),
  };
}

const key = (f, player, action) =>
  f.host.tabletops.receive(player, 'kcds:tabletop', { op: 'deck', table: f.table.id, action });

// The prop spawns that reached a player's game (to: id) or everyone's (no to).
function spawnsFor(f, id) {
  return f.sent.filter(
    (message) => message.op === 'propSpawn' && (message.to === undefined || message.to === id),
  );
}

const faceOf = (ids) => (model) => ids.some((id) => model.endsWith(`/${id}.cgf`));

test('decks: a standard deck of cards lies face down, its faces nowhere', async (t) => {
  const f = await setup(t);
  assert.equal(f.deck.pieces.length, 52);
  assert.equal(f.deck.remaining, 52);
  assert.deepEqual(f.deck.piles, ['table']);
  const ace = f.deck.pieces[0];
  assert.equal(ace.id, 'AS');
  assert.equal(ace.rank, 'A');
  assert.equal(ace.suit, 'S');
  assert.equal(ace.place, 'stack');
  const models = f.sent
    .filter((message) => message.op === 'propSpawn')
    .map((message) => message.model);
  assert.ok(models.length > 0);
  assert.ok(models.every((model) => model === 'objects/kcds_cards/back.cgf'));
  assert.equal(f.table.createDeck({ jokers: true, models: 'objects/kcds/mine' }).pieces.length, 54);
  const tiles = f.table.createDeck({ pieces: ['d0_0', 'd0_1'], models: 'objects/kcds/dominoes' });
  assert.equal(tiles.pieces[1].rank, null);
  assert.throws(() => f.table.createDeck({ pieces: ['bad id!'] }), /id/);
  assert.throws(() => f.table.createDeck({ models: '../x' }), /models/);
});

test('decks: dealt pieces glide into the hands; only the holder gets their faces', async (t) => {
  const f = await setup(t);
  const henry = f.report(7);
  const hans = f.report(8);
  f.table.sit(henry, 0);
  f.table.sit(hans, 1);
  const dealt = f.deck.deal([henry, hans], 2);
  assert.equal(dealt.length, 4);
  assert.equal(f.deck.remaining, 48);
  // A flight is one glide for the players' games, not a step every frame.
  const glides = f.sent.filter((message) => message.op === 'propMove' && message.ms > 0);
  await delay(1300);
  assert.ok(f.sent.filter((message) => message.op === 'propMove' && message.ms > 0).length >= 4);
  assert.ok(glides.length <= 4);
  assert.equal(f.deck.hand(henry).length, 2);
  assert.equal(f.deck.hand(hans).length, 2);
  const henrys = faceOf(f.deck.hand(henry).map((piece) => piece.id.toLowerCase()));
  const facesTo = (id) =>
    spawnsFor(f, id)
      .map((message) => message.model)
      .filter(henrys);
  assert.ok(facesTo(7).length >= 2, 'Henry sees his faces');
  assert.equal(facesTo(8).length, 0, "Hans's game never gets Henry's faces");
  const hand = f.sent.filter((message) => message.name === 'kcds:tabletop' && message.player === 7);
  assert.equal(JSON.parse(hand.at(-1).payload).count, 2);
  // The hold pose and animation while they hold pieces.
  assert.ok(henry.poseAnimation);
});

test('decks: the holder points, selects and plays; the piece lands on the pile', async (t) => {
  const f = await setup(t);
  const henry = f.report(7);
  f.table.sit(henry, 0);
  f.deck.deal(henry, 3);
  await delay(1200);
  const [first, second] = f.deck.hand(henry);
  key(f, henry, 'next');
  key(f, henry, 'select');
  assert.equal(second.selected, true);
  assert.equal(first.selected, false);
  assert.deepEqual(
    f.deck.selected(henry).map((piece) => piece.id),
    [second.id],
  );
  key(f, henry, 'play');
  await delay(1300);
  assert.equal(second.place, 'pile');
  assert.equal(second.pile, 'table');
  assert.equal(second.faceUp, true);
  assert.deepEqual(
    f.deck.pile('table').map((piece) => piece.id),
    [second.id],
  );
  assert.equal(f.deck.hand(henry).length, 2);
  assert.deepEqual(f.lines(), [`select 7 ${second.id} true`, `play 7 ${second.id}`]);
});

test('decks: cover, reveal, dealTo, flip, collect', async (t) => {
  const f = await setup(t);
  const henry = f.report(7);
  f.report(8);
  f.table.sit(henry, 0);
  f.deck.deal(henry, 2);
  await delay(900);
  key(f, henry, 'cover');
  assert.equal(f.deck.covered(henry), true);
  key(f, henry, 'cover');
  assert.equal(f.deck.covered(henry), false);
  f.deck.reveal(henry);
  const ids = faceOf(f.deck.hand(henry).map((piece) => piece.id.toLowerCase()));
  const shown = spawnsFor(f, 8).filter((message) => ids(message.model));
  assert.ok(shown.length >= 2, 'revealed faces reach everyone');
  f.deck.addPile('board', { position: { x: 0, y: 0.1 }, layout: 'row' });
  const flop = f.deck.dealTo('board', 3);
  await delay(1200);
  assert.deepEqual(
    f.deck.pile('board').map((piece) => piece.id),
    flop.map((piece) => piece.id),
  );
  f.deck.flip(flop[0], false);
  assert.equal(flop[0].faceUp, false);
  await f.deck.collect();
  assert.equal(f.deck.remaining, 52);
  assert.equal(f.deck.hand(henry).length, 0);
  assert.equal(henry.poseAnimation, null);
  assert.deepEqual(f.lines(), ['cover 7 true', 'cover 7 false']);
});

test('decks: save and load; a player who leaves gives their hand back', async (t) => {
  const f = await setup(t);
  const henry = f.report(7);
  f.table.sit(henry, 0);
  f.deck.shuffle();
  f.deck.deal(henry, 2);
  await delay(900);
  const saved = f.deck.save();
  const held = f.deck.hand(henry).map((piece) => piece.id);
  await f.deck.collect();
  f.deck.load(saved);
  assert.deepEqual(
    f.deck.hand(henry).map((piece) => piece.id),
    held,
  );
  assert.equal(f.deck.remaining, 50);
  f.table.stand(henry);
  assert.equal(f.deck.hand(henry).length, 0);
  assert.equal(f.deck.remaining, 52);
  f.table.destroy();
  assert.equal(f.deck.exists, false);
});

// The last message of the tabletop's to a player (its payload).
function lastTabletop(f, id) {
  const sent = f.sent.filter(
    (message) => message.name === 'kcds:tabletop' && message.player === id,
  );
  return JSON.parse(sent.at(-1).payload);
}

test('decks: the fan hangs on the left hand; the throw takes it in the right hand to the end', async (t) => {
  const f = await setup(t);
  const henry = f.report(7);
  f.table.sit(henry, 0);
  f.deck.deal(henry, 1);
  await delay(700);
  const [card] = f.deck.hand(henry);
  const inHand = spawnsFor(f, 7).filter((message) =>
    message.model.endsWith(`/${card.id.toLowerCase()}.cgf`),
  );
  assert.equal(inHand.at(-1).joint, 'LeftHand');
  f.deck.select(card, true);
  const landed = f.deck.play(henry, f.deck.selected(henry));
  await delay(500);
  // Between the thumb and index of the right hand, the throw still playing with the hand empty.
  assert.equal(card.place, 'moving');
  assert.ok(f.sent.some((message) => message.op === 'propSpawn' && message.joint === 'RightHand'));
  assert.ok(henry.poseAnimation, 'the throw plays on with the last piece');
  assert.equal(await landed, true);
  await delay(300);
  assert.equal(card.pile, 'table');
  assert.equal(henry.poseAnimation, null, 'the hand goes down after the throw');
});

test('decks: draw and pass keys with their labels, restock, a 32-card deck', async (t) => {
  const f = await setup(t);
  const henry = f.report(7);
  f.table.sit(henry, 0);
  // At first a pass key and a look key, no draw key.
  const look = 'Look closer';
  assert.deepEqual(lastTabletop(f, 7).actions, { draw: false, pass: 'Pass', look });
  key(f, henry, 'draw');
  key(f, henry, 'pass');
  f.deck.setActions({ draw: 'Draw a card', pass: 'Stand' });
  assert.deepEqual(lastTabletop(f, 7).actions, { draw: 'Draw a card', pass: 'Stand', look });
  key(f, henry, 'draw');
  await delay(700);
  assert.equal(f.deck.hand(henry).length, 1, 'drawn into an empty hand');
  f.deck.setActions({ draw: false, look: false }, henry);
  assert.deepEqual(f.deck.actions(henry), { draw: false, pass: 'Stand', look: false });
  assert.deepEqual(f.deck.actions(), { draw: 'Draw a card', pass: 'Stand', look });
  key(f, henry, 'draw');
  await delay(100);
  assert.equal(f.deck.hand(henry).length, 1, 'no draw key for him now');
  f.deck.setActions({ draw: 'Draw' });
  f.host.resources.get('decks').vm.refuseDraw = true;
  key(f, henry, 'draw');
  await delay(100);
  assert.equal(f.deck.hand(henry).length, 1, 'a refused draw draws nothing');
  assert.deepEqual(f.lines(), ['pass 7', 'draw 7', 'draw 7']);
  assert.throws(() => f.deck.setActions({ check: 'Check' }), /draw, pass and look/);
  assert.throws(() => f.deck.setActions({ pass: 7 }), /text/);
  // A pile back into the stack but its top piece.
  f.deck.dealTo('table', 3);
  await delay(1000);
  const top = f.deck.pile('table').at(-1);
  assert.equal(await f.deck.restock('table'), 2);
  assert.deepEqual(
    f.deck.pile('table').map((piece) => piece.id),
    [top.id],
  );
  assert.equal(f.deck.remaining, 50);
  // Prší's and Mariáš's deck: 7 to the ace in four suits.
  const short = f.table.createDeck({ ranks: ['7', '8', '9', 'T', 'J', 'Q', 'K', 'A'] });
  assert.equal(short.pieces.length, 32);
  assert.ok(short.pieces.every((piece) => !['2', '3', '4', '5', '6'].includes(piece.rank)));
  assert.throws(() => f.table.createDeck({ ranks: ['1'] }), /ranks/);
  assert.throws(() => f.table.createDeck({ ranks: ['7'], pieces: ['7S'] }), /not both/);
});

test("tabletop: a seat's camera 'eyes' starts no shot: the game's own first person", async (t) => {
  const f = await setup(t);
  const vm = f.host.resources.get('decks').vm;
  const tableHandle = vm.kcds.world.Tabletop.create({
    position: { x: 10, y: 20, z: 1 },
    seats: [{ position: { x: 10, y: 19.3, z: 0.25 }, rotation: 0, camera: 'eyes' }],
  });
  const state = [...f.host.tabletops.tables.values()].find((one) => one.handle === tableHandle);
  assert.equal(f.host.tabletops.seatShot(state, 0), null);
  const described = f.host.tabletops.describe(state).seats[0];
  assert.equal(described.fixedView, false);
  assert.equal(described.lookView, false);
  assert.throws(
    () => vm.kcds.world.Tabletop.create({ position: { x: 0, y: 0, z: 0 }, camera: 'above' }),
    /camera shot/,
  );
});

test('Prop.glideTo: one move with its time and arc, the end pose at once', async (t) => {
  const f = await setup(t);
  const { Prop } = f.host.api;
  const prop = Prop.spawn('objects/kcds_cards/as.cgf', { x: 1, y: 2, z: 3 });
  prop.glideTo({ x: 2, y: 2, z: 3 }, { x: 0, y: 0, z: 90 }, { ms: 500, arc: 0.2 });
  const move = f.sent.filter((message) => message.op === 'propMove').at(-1);
  assert.equal(move.ms, 500);
  assert.equal(move.arc, 0.2);
  assert.equal(prop.position.x, 2);
  assert.throws(() => prop.glideTo({ x: 0, y: 0, z: 0 }, undefined, { ms: 20000 }), /ms/);
  assert.throws(() => prop.glideTo({ x: 0, y: 0, z: 0 }, undefined, { arc: 9 }), /arc/);
});
