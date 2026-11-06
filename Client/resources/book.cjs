'use strict';

// The client-only Book global: a resource's pages shown through
// the game's own reading, at this machine only.
//
// Book.open sends { op: 'bookOpen', id, style, type, visual, legibility, pages, images }; the
// game client (Client/runtime/book) opens it on the next frame and reports
// { op: 'bookEvent', id, name: 'opened' | 'closed', reason }. Whether the player has a body and
// can read where they stand comes as { op: 'bookHost', inWorld, canRead }.

const STYLES = Object.freeze({ book: 0, plainBook: 1, letter: 2 });
const LETTER_TYPE = 5;
const CLOSE_REASONS = new Set(['player', 'script', 'failed', 'sessionOver']);

function integerIn(value, low, high, what) {
  if (!Number.isInteger(value) || value < low || value > high) {
    throw new RangeError(`Book: ${what} must be an integer from ${low} to ${high}`);
  }
  return value;
}

function legibilityOf(value) {
  if (typeof value !== 'number' || Number.isNaN(value)) {
    throw new TypeError('Book: legibility must be a number');
  }
  return Math.min(1, Math.max(0, value));
}

// The options as the game client takes them; malformed options throw.
function bookRequest(options) {
  if (options === null || typeof options !== 'object') {
    throw new TypeError('Book: options must be an object');
  }
  const { pages, style = 'book', type, visual = 1, legibility = 1, texture, images = [] } = options;
  if (!Array.isArray(pages) || pages.length === 0) {
    throw new TypeError('Book: pages must be a non-empty array of strings');
  }
  for (const page of pages) {
    if (typeof page !== 'string') {
      throw new TypeError('Book: pages must be a non-empty array of strings');
    }
  }
  if (!Object.hasOwn(STYLES, style)) {
    throw new RangeError(`Book: unknown style ${style}`);
  }
  if (texture !== undefined) {
    throw new Error('Book: texture is not supported by this game client yet');
  }
  if (!Array.isArray(images)) {
    throw new TypeError('Book: images must be an array');
  }
  const pictures = images.map((picture) => {
    if (picture === null || typeof picture !== 'object') {
      throw new TypeError('Book: an image must be { page, image }');
    }
    integerIn(picture.page, 0, pages.length - 1, 'image page');
    if (typeof picture.image !== 'string' || !/^[A-Za-z0-9_]+$/.test(picture.image)) {
      throw new TypeError('Book: image must be a native picture name');
    }
    return { page: picture.page, image: picture.image };
  });
  return {
    style: STYLES[style],
    type: integerIn(
      type === undefined ? (style === 'letter' ? LETTER_TYPE : 1) : type,
      1,
      6,
      'type',
    ),
    visual: integerIn(visual, 1, 7, 'visual'),
    legibility: legibilityOf(legibility),
    pages: [...pages],
    images: pictures,
  };
}

class ResourceBook {
  // shipImage(path): mounts a .dds the resource ships for the game's texture loader and
  // answers its img:// address; throws for a missing or unusable file.
  constructor({ send, emit, shipImage = null }) {
    this.send = send;
    this.emit = emit;
    this.shipImage = shipImage;
    this.inWorld = false;
    this.canRead = false;
    this.nextId = 1;
    // the book asked for or in the hands: { id, opened }
    this.current = null;
    this.lastError = '';
  }

  receiveHost({ inWorld, canRead }) {
    this.inWorld = Boolean(inWorld);
    this.canRead = Boolean(canRead);
  }

  receiveEvent({ id, name, reason }) {
    if (!this.current || this.current.id !== id) {
      return;
    }
    if (name === 'opened') {
      this.current.opened = true;
      this.emit('bookOpened', [id]);
    } else if (name === 'edited') {
      const writing = this.current.writing;
      if (writing !== null) {
        this.emit('bookTextChanged', [id, writing, typeof reason === 'string' ? reason : '']);
      }
      return;
    } else if (name === 'writingEnded') {
      const writing = this.current.writing;
      this.current.writing = null;
      if (writing !== null) {
        this.emit('bookWritingEnded', [id, writing]);
      }
      return;
    } else if (name === 'closed') {
      this.current = null;
      this.emit('bookClosed', [id, CLOSE_REASONS.has(reason) ? reason : 'player']);
    }
  }

  facade(owner = '') {
    const book = this;
    return Object.freeze({
      open(options) {
        const request = bookRequest(options);
        if (!book.inWorld) {
          book.lastError = 'The player has no body yet';
          return null;
        }
        if (book.current) {
          book.lastError = 'A book is already open';
          return null;
        }
        if (!book.canRead) {
          book.lastError = 'The player cannot read here';
          return null;
        }
        const id = book.nextId++;
        book.current = { id, opened: false, pages: request.pages.length, writing: null };
        book.lastError = '';
        book.send({ op: 'bookOpen', id, ...request });
        return id;
      },
      close() {
        if (!book.current) {
          return false;
        }
        book.send({ op: 'bookClose' });
        return true;
      },
      setReadable(value) {
        const legibility = legibilityOf(value);
        if (!book.current || !book.current.opened) {
          return false;
        }
        book.send({ op: 'bookLegibility', value: legibility });
        return true;
      },
      // Replaces the open book's pages while it is read (the same markup as open's pages).
      setPages(pages) {
        if (
          !Array.isArray(pages) ||
          pages.length === 0 ||
          pages.some((p) => typeof p !== 'string')
        ) {
          throw new TypeError('Book: pages must be a non-empty array of strings');
        }
        if (!book.current || !book.current.opened) {
          return false;
        }
        book.current.pages = pages.length;
        book.current.writing = null;
        book.send({ op: 'bookPages', pages: [...pages] });
        return true;
      },
      // Lets the player write on a page of the open book (the last one by default): typed text
      // appears on the page as it is typed ('bookTextChanged'), Enter starts a new line and Esc
      // ends writing ('bookWritingEnded'). write(false) ends it from the script.
      write(enabled, page) {
        if (typeof enabled !== 'boolean') {
          throw new TypeError('Book: write takes true or false');
        }
        if (!book.current || !book.current.opened) {
          return false;
        }
        if (!enabled) {
          book.current.writing = null;
          book.send({ op: 'bookWrite', enabled: false });
          return true;
        }
        const last = book.current.pages - 1;
        const at = page === undefined ? last : integerIn(page, 0, last, 'page');
        book.current.writing = at;
        book.send({ op: 'bookWrite', enabled: true, page: at });
        return true;
      },
      currentBook() {
        return book.current && book.current.opened ? book.current.id : null;
      },
      lastFailure() {
        return book.lastError;
      },
      // The <img src> value of a .dds the resource ships: the game client packs it into an
      // archive and mounts it where the book screen looks for a picture of that name.
      image(path) {
        if (typeof path !== 'string' || !/\.dds$/i.test(path)) {
          throw new TypeError('Book: image takes the path of a .dds the resource ships');
        }
        if (!book.shipImage) {
          throw new Error('Book: pictures are not available in this game client');
        }
        return book.shipImage(owner, path);
      },
    });
  }
}

module.exports = { ResourceBook, bookRequest };
