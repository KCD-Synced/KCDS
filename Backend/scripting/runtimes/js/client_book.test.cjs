'use strict';

// The client Book global (Client/resources/book.cjs): option checks, the refusals open
// returns null for, the request sent to the game client, and the book events.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceBook } = require('../../../../Client/resources/book.cjs');

function setup({ inWorld = true, canRead = true } = {}) {
  const sent = [];
  const emitted = [];
  const book = new ResourceBook({
    send: (message) => sent.push(message),
    emit: (name, args) => emitted.push([name, ...args]),
  });
  book.receiveHost({ inWorld, canRead });
  return { book, sent, emitted, facade: book.facade() };
}

test('open sends the pages with the defaults and returns the id', () => {
  const { sent, facade } = setup();
  const id = facade.open({ pages: ['<title>A</title>', '<paragraph>B</paragraph>'] });
  assert.equal(id, 1);
  assert.equal(facade.lastFailure(), '');
  assert.deepEqual(sent, [
    {
      op: 'bookOpen',
      id: 1,
      style: 0,
      type: 1,
      visual: 1,
      legibility: 1,
      pages: ['<title>A</title>', '<paragraph>B</paragraph>'],
      images: [],
    },
  ]);
});

test('a letter defaults to the letter layout; legibility is clamped', () => {
  const { sent, facade } = setup();
  facade.open({ style: 'letter', pages: ['x'], legibility: 3 });
  assert.equal(sent[0].style, 2);
  assert.equal(sent[0].type, 5);
  assert.equal(sent[0].legibility, 1);
});

test('malformed options throw', () => {
  const { facade } = setup();
  assert.throws(() => facade.open(null), TypeError);
  assert.throws(() => facade.open({ pages: [] }), TypeError);
  assert.throws(() => facade.open({ pages: [1] }), TypeError);
  assert.throws(() => facade.open({ pages: ['x'], style: 'scroll' }), RangeError);
  assert.throws(() => facade.open({ pages: ['x'], type: 7 }), RangeError);
  assert.throws(() => facade.open({ pages: ['x'], visual: 0 }), RangeError);
  assert.throws(() => facade.open({ pages: ['x'], legibility: 'high' }), TypeError);
  assert.throws(
    () => facade.open({ pages: ['x'], images: [{ page: 1, image: 'swords1' }] }),
    RangeError,
  );
  assert.throws(
    () => facade.open({ pages: ['x'], images: [{ page: 0, image: '../a' }] }),
    TypeError,
  );
});

test('open returns null with the reason when the player cannot read', () => {
  const noBody = setup({ inWorld: false });
  assert.equal(noBody.facade.open({ pages: ['x'] }), null);
  assert.match(noBody.facade.lastFailure(), /no body/);
  const notHere = setup({ canRead: false });
  assert.equal(notHere.facade.open({ pages: ['x'] }), null);
  assert.match(notHere.facade.lastFailure(), /cannot read/);
  const { facade, sent } = setup();
  facade.open({ pages: ['x'] });
  assert.equal(facade.open({ pages: ['y'] }), null);
  assert.match(facade.lastFailure(), /already open/);
  assert.equal(sent.length, 1);
});

test('bookOpened and bookClosed follow the game client; close and setReadable need a book', () => {
  const { book, sent, emitted, facade } = setup();
  assert.equal(facade.close(), false);
  assert.equal(facade.setReadable(0.5), false);
  const id = facade.open({ pages: ['x'] });
  assert.equal(facade.currentBook(), null);
  assert.equal(facade.setReadable(0.5), false);
  book.receiveEvent({ id, name: 'opened' });
  assert.equal(facade.currentBook(), id);
  assert.equal(facade.setReadable(-2), true);
  assert.throws(() => facade.setReadable('x'), TypeError);
  assert.equal(facade.close(), true);
  book.receiveEvent({ id, name: 'closed', reason: 'script' });
  assert.equal(facade.currentBook(), null);
  assert.deepEqual(emitted, [
    ['bookOpened', id],
    ['bookClosed', id, 'script'],
  ]);
  assert.deepEqual(sent.slice(1), [{ op: 'bookLegibility', value: 0 }, { op: 'bookClose' }]);
});

test('a book that never reached the hands closes as failed; the next one can open', () => {
  const { book, emitted, facade } = setup();
  const first = facade.open({ pages: ['x'] });
  book.receiveEvent({ id: first, name: 'closed', reason: 'failed' });
  assert.deepEqual(emitted, [['bookClosed', first, 'failed']]);
  assert.equal(facade.open({ pages: ['y'] }), first + 1);
});

test('texture is not supported yet; image takes a shipped .dds', () => {
  const { facade } = setup();
  assert.throws(() => facade.open({ pages: ['x'], texture: 'a' }), /texture/);
  assert.throws(() => facade.image('a.png'), TypeError);
  assert.throws(() => facade.image('a.dds'), /not available/);
  const { ResourceBook } = require('../../../../Client/resources/book.cjs');
  const asked = [];
  const book = new ResourceBook({
    send: () => {},
    emit: () => {},
    shipImage: (owner, path) => {
      asked.push([owner, path]);
      return `img://Libs/UI/Textures/kcds/${owner}/${path}`;
    },
  });
  assert.equal(
    book.facade('letters').image('ui/seal.dds'),
    'img://Libs/UI/Textures/kcds/letters/ui/seal.dds',
  );
  assert.deepEqual(asked, [['letters', 'ui/seal.dds']]);
});

test('setPages and write work on the open book; typed text and its end are raised', () => {
  const { book, sent, emitted, facade } = setup();
  assert.equal(facade.setPages(['x']), false);
  const id = facade.open({ pages: ['a', 'b'] });
  assert.equal(facade.write(true), false);
  book.receiveEvent({ id, name: 'opened' });
  assert.equal(facade.setPages(['c', 'd', 'e']), true);
  assert.deepEqual(sent.at(-1), { op: 'bookPages', pages: ['c', 'd', 'e'] });
  assert.equal(facade.write(true), true);
  assert.deepEqual(sent.at(-1), { op: 'bookWrite', enabled: true, page: 2 });
  assert.throws(() => facade.write(true, 3), RangeError);
  assert.throws(() => facade.write('yes'), TypeError);
  book.receiveEvent({ id, name: 'edited', reason: 'Hello\nworld' });
  book.receiveEvent({ id, name: 'writingEnded' });
  assert.deepEqual(emitted.slice(-2), [
    ['bookTextChanged', id, 2, 'Hello\nworld'],
    ['bookWritingEnded', id, 2],
  ]);
  assert.equal(facade.write(false), true);
  assert.deepEqual(sent.at(-1), { op: 'bookWrite', enabled: false });
});
