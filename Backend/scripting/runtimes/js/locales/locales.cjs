'use strict';

// Texts KCDS itself shows to players, rewritable per language. Every KCDS text has a
// key and an English default; a server or resource sets its own texts for a language (the
// language a player's game shows, Player.language: 'english', 'czech', 'german', ...), and a
// player is shown the text for their language, else the server's English one, else the default.
// Resources may add keys of their own for their custom content. Placeholders ({thief}) are
// filled in by name.

const DEFAULT_LANGUAGE = 'english';

// The KCDS texts and their English defaults.
const DEFAULTS = Object.freeze({
  // To a player who caught a thief at their pockets (Pickpocketing.setVictimNotice).
  'pickpocket.victimNotice': '{thief} tried to pick your pocket',
  // The tooltip of the inventory's max column (item-config.json `max`).
  'items.maxTooltip': 'Most you can carry',
  // To a player who would take more of an item than its limit.
  'items.limitReached': "You can't carry more than {max} of this item",
  // To a player who tries to open a container another player has open.
  'stash.inUse': 'Someone else is using this',
});

// The longest key and text a locale takes.
const MAX_KEY = 128;
const MAX_TEXT = 1024;

function languageOf(language) {
  if (typeof language !== 'string' || !/^[a-z_]{1,32}$/i.test(language)) {
    throw new TypeError('a language is a name such as "english" or "czech"');
  }
  return language.toLowerCase();
}

function fill(text, params) {
  if (!params || typeof params !== 'object') {
    return text;
  }
  return text.replace(/\{([A-Za-z0-9_]+)\}/g, (whole, name) =>
    Object.prototype.hasOwnProperty.call(params, name) ? String(params[name]) : whole,
  );
}

class Locales {
  constructor() {
    // language -> Map(key -> text), what the server set.
    this.languages = new Map();
  }

  // Locales.set(language, { key: text, ... }): the server's texts for a language; an empty text
  // gives a key back to the default.
  set(language, texts) {
    const name = languageOf(language);
    if (!texts || typeof texts !== 'object' || Array.isArray(texts)) {
      throw new TypeError('Locales.set takes a language and an object of key: text');
    }
    const entries = Object.entries(texts);
    for (const [key, text] of entries) {
      if (key.length < 1 || key.length > MAX_KEY) {
        throw new TypeError(`a text key is 1 to ${MAX_KEY} characters`);
      }
      if (typeof text !== 'string' || text.length > MAX_TEXT) {
        throw new TypeError(`the text of ${key} is a string of at most ${MAX_TEXT} characters`);
      }
    }
    const table = this.languages.get(name) ?? new Map();
    for (const [key, text] of entries) {
      if (text === '') table.delete(key);
      else table.set(key, text);
    }
    this.languages.set(name, table);
  }

  // The text of a key in a language: the server's text for it, else the server's English text,
  // else the default; undefined for a key nobody gave a text.
  get(key, language = DEFAULT_LANGUAGE) {
    const name = typeof language === 'string' ? language.toLowerCase() : DEFAULT_LANGUAGE;
    return (
      this.languages.get(name)?.get(key) ??
      this.languages.get(DEFAULT_LANGUAGE)?.get(key) ??
      DEFAULTS[key]
    );
  }

  // The text of a key for a player (their game's language), with its placeholders filled in;
  // the key itself when nobody gave it a text.
  text(player, key, params) {
    const language = player?.language || DEFAULT_LANGUAGE;
    return fill(this.get(key, language) ?? key, params);
  }

  api() {
    const self = this;
    return Object.freeze({
      defaultLanguage: DEFAULT_LANGUAGE,
      // The KCDS keys and their English defaults.
      defaults: DEFAULTS,
      set: (language, texts) => self.set(language, texts),
      get: (key, language) => self.get(key, language),
      text: (player, key, params) => self.text(player, key, params),
    });
  }
}

module.exports = { Locales, DEFAULTS, DEFAULT_LANGUAGE };
