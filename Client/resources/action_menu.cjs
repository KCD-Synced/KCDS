'use strict';

// The client-only ActionMenu global: a choice of a few actions on one of the game's controls (E by
// default), on one line of the game's own hint: "Talk · [Trade] · Give". The mouse wheel moves the
// mark, the control's press runs the menu's handler with the marked entry's id.
//
// A menu is an ActionHint (action_hints.cjs) whose text is the line. The newest open menu takes
// the wheel: while any is open the runtime asks the game client for it ({ op: 'wheel', wanted }),
// which then reports its whole notches in a world with nothing taking typed input
// ({ op: 'wheelTurned', notches }, away from the player positive: the entry before).
//
// A choice is the player's: a resource sends it to its server, which checks it
// (player.canReach, its own rules) before it acts.

const MIN_ENTRIES = 2;
const MAX_ENTRIES = 6;
const MAX_ENTRY_TEXT = 32;
const MAX_LINE_BYTES = 256;
const ENTRY_ID = /^[A-Za-z0-9_-]{1,32}$/;

function readEntries(entries) {
  if (!Array.isArray(entries)) {
    throw new TypeError('ActionMenu: entries must be an array');
  }
  if (entries.length < MIN_ENTRIES || entries.length > MAX_ENTRIES) {
    throw new RangeError(`ActionMenu: a menu has ${MIN_ENTRIES} to ${MAX_ENTRIES} entries`);
  }
  const seen = new Set();
  return entries.map((entry) => {
    if (entry === null || typeof entry !== 'object') {
      throw new TypeError('ActionMenu: an entry is { id, text }');
    }
    const { id, text } = entry;
    if (typeof id !== 'string' || !ENTRY_ID.test(id)) {
      throw new TypeError('ActionMenu: an entry id is 1 to 32 letters, digits, _ or -');
    }
    if (seen.has(id)) {
      throw new RangeError(`ActionMenu: entry id ${id} is there twice`);
    }
    seen.add(id);
    if (typeof text !== 'string' || text.trim() === '') {
      throw new TypeError('ActionMenu: an entry text is a non-empty string');
    }
    if (text.length > MAX_ENTRY_TEXT) {
      throw new RangeError(`ActionMenu: an entry text is at most ${MAX_ENTRY_TEXT} characters`);
    }
    return Object.freeze({ id, text });
  });
}

// The hint's line: every entry, the marked one in brackets.
function line(entries, selected) {
  return entries
    .map((entry, index) => (index === selected ? `[${entry.text}]` : entry.text))
    .join(' · ');
}

class ResourceActionMenus {
  constructor({ actionHints, send }) {
    this.actionHints = actionHints;
    this.send = send;
    // id -> { owner, hints, hint, entries, selected }; `order` holds the open ones, newest last.
    this.menus = new Map();
    this.order = [];
    this.nextId = 1;
    this.wheelWanted = false;
  }

  // Asks the game client for the wheel while a menu is open, and lets it go after the last.
  updateWheel() {
    const wanted = this.order.length > 0;
    if (wanted !== this.wheelWanted) {
      this.wheelWanted = wanted;
      this.send({ op: 'wheel', wanted });
    }
  }

  // { op: 'wheelTurned', notches }: the newest open menu moves its mark, round the ends.
  receiveWheel({ notches }) {
    if (!Number.isInteger(notches) || notches === 0 || this.order.length === 0) {
      return;
    }
    const menu = this.menus.get(this.order[this.order.length - 1]);
    const count = menu.entries.length;
    menu.selected = (((menu.selected - notches) % count) + count) % count;
    menu.hints.setText(menu.hint, line(menu.entries, menu.selected));
  }

  close(id) {
    const menu = this.menus.get(id);
    if (!menu) {
      return false;
    }
    menu.hints.remove(menu.hint);
    this.menus.delete(id);
    this.order = this.order.filter((open) => open !== id);
    this.updateWheel();
    return true;
  }

  // A stopping resource's menus go (its hints go with ActionHint's removeOwner).
  removeOwner(owner) {
    for (const [id, menu] of [...this.menus]) {
      if (menu.owner === owner) {
        this.close(id);
      }
    }
  }

  facade(owner) {
    const menus = this;
    const hints = this.actionHints.facade(owner);
    const own = (id) => {
      const menu = menus.menus.get(id);
      return menu && menu.owner === owner ? menu : null;
    };
    return Object.freeze({
      // options: { entries: [{ id, text }], key = 'primary_action' }. Returns the menu's id.
      open(options, handler) {
        if (options === null || typeof options !== 'object') {
          throw new TypeError('ActionMenu: options must be an object');
        }
        if (typeof handler !== 'function') {
          throw new TypeError('ActionMenu: handler must be a function');
        }
        const entries = readEntries(options.entries);
        const key = options.key ?? 'primary_action';
        const text = line(entries, 0);
        if (new TextEncoder().encode(text).length > MAX_LINE_BYTES) {
          throw new RangeError(
            `ActionMenu: the entries' texts are longer than ${MAX_LINE_BYTES} bytes`,
          );
        }
        const id = menus.nextId++;
        const menu = { owner, hints, hint: 0, entries, selected: 0 };
        menu.hint = hints.create({ key, text }, () => handler(menu.entries[menu.selected].id));
        menus.menus.set(id, menu);
        menus.order.push(id);
        menus.updateWheel();
        return id;
      },
      close(id) {
        return own(id) ? menus.close(id) : false;
      },
      // Marks the entry with that id; false for a menu or entry that is not there.
      select(id, entryId) {
        const menu = own(id);
        const index = menu ? menu.entries.findIndex((entry) => entry.id === entryId) : -1;
        if (index < 0) {
          return false;
        }
        menu.selected = index;
        hints.setText(menu.hint, line(menu.entries, index));
        return true;
      },
      // The marked entry's id, or null for a menu that is not open.
      selected(id) {
        const menu = own(id);
        return menu ? menu.entries[menu.selected].id : null;
      },
    });
  }
}

module.exports = { ResourceActionMenus, line };
