'use strict';

// The client-only CharacterCreator global: the game's own character
// stage, where the local player chooses how they look.
//
// The game client puts the stage up (Client/runtime/character_creator/creator_stage.cpp): the
// pause screen on its player page with the page's own panels hidden, and the paper-doll figure
// dressed in the look being chosen. The runtime asks with { op: 'creatorOpen' },
// { op: 'creatorClose' } and { op: 'creatorLook', gender, body, head, hair, beard }; the game
// client answers { op: 'creatorStage', state, keys } with 'open', 'unavailable', 'left' (the
// player closed the pause screen) or 'interrupted', and reports the creator's keys as
// { op: 'creatorKey', key }: the pause screen holds the game's input, so key binds do not fire.
// The panel beside the figure is a NativeUI screen (creator_panel.cjs). Nothing here touches the player's body or
// leaves this machine: characterCreatorClosed hands the accepted look to the resources.

const {
  Appearances,
  PARTS,
} = require('../../Backend/scripting/runtimes/js/appearance/appearances.cjs');

const GENDER_CHOICES = Object.freeze(['male', 'female', 'both']);
const TITLE_MAX_BYTES = 64;
const DEFAULT_TITLE = 'Character';
const OPTION_KEYS = new Set(['appearance', 'genders', 'title', 'panel', 'choices']);
const APPEARANCE_KEYS = new Set(['gender', ...PARTS]);
// Most names a part's choices may list.
const MOST_CHOICES = 512;
// How the camera frames the figure: the whole of it, or close on the face.
const FRAMES = Object.freeze(['body', 'face']);

function utf8Length(text) {
  return new TextEncoder().encode(text).length;
}

// A look the creator can start on: every part a name of that gender's catalog or empty, the
// beard one the face can wear. Throws for a malformed one.
function checkAppearance(appearance) {
  if (appearance === null || typeof appearance !== 'object' || Array.isArray(appearance)) {
    throw new TypeError('CharacterCreator: appearance must be an Appearance');
  }
  for (const key of Object.keys(appearance)) {
    if (!APPEARANCE_KEYS.has(key)) {
      throw new TypeError(`CharacterCreator: unknown appearance key ${key}`);
    }
  }
  const gender = appearance.gender;
  if (gender !== 'male' && gender !== 'female') {
    throw new TypeError('CharacterCreator: appearance.gender must be male or female');
  }
  const look = { gender };
  for (const part of PARTS) {
    const name = appearance[part] ?? '';
    if (typeof name !== 'string') {
      throw new TypeError(`CharacterCreator: appearance.${part} must be a string`);
    }
    look[part] = name;
  }
  for (const part of ['body', 'head', 'hair']) {
    if (look[part] !== '' && Appearances.find(part, look[part], gender) === null) {
      throw new RangeError(`CharacterCreator: no ${gender} ${part} named ${look[part]}`);
    }
  }
  if (
    look.beard !== '' &&
    !Appearances.beards(look.head, gender).some((option) => option.name === look.beard)
  ) {
    throw new RangeError(`CharacterCreator: the face cannot wear the beard ${look.beard}`);
  }
  return look;
}

// The names a part may be chosen from ({ part: [name, ...] }, '' for the game's own part): every
// name one of that part in a gender's catalogue. Null when there is no limit.
function checkChoices(choices) {
  if (choices === undefined || choices === null) {
    return null;
  }
  if (typeof choices !== 'object' || Array.isArray(choices)) {
    throw new TypeError('CharacterCreator: choices must be an object of part name lists');
  }
  const checked = {};
  for (const [part, names] of Object.entries(choices)) {
    if (!PARTS.includes(part)) {
      throw new TypeError(`CharacterCreator: choices has an unknown part ${part}`);
    }
    if (!Array.isArray(names) || names.length === 0 || names.length > MOST_CHOICES) {
      throw new TypeError(`CharacterCreator: choices.${part} must list 1 to ${MOST_CHOICES} names`);
    }
    for (const name of names) {
      if (typeof name !== 'string') {
        throw new TypeError(`CharacterCreator: choices.${part} must list strings`);
      }
      const known =
        name === '' ||
        Appearances.find(part, name, 'male') !== null ||
        Appearances.find(part, name, 'female') !== null;
      if (!known) {
        throw new RangeError(`CharacterCreator: choices.${part} names no ${part} ${name}`);
      }
    }
    checked[part] = Object.freeze([...new Set(names)]);
  }
  return Object.freeze(checked);
}

function checkOptions(options) {
  if (options === undefined) {
    return {
      appearance: undefined,
      genders: 'both',
      title: DEFAULT_TITLE,
      panel: true,
      choices: null,
    };
  }
  if (options === null || typeof options !== 'object' || Array.isArray(options)) {
    throw new TypeError('CharacterCreator: options must be an object');
  }
  for (const key of Object.keys(options)) {
    if (!OPTION_KEYS.has(key)) {
      throw new TypeError(`CharacterCreator: unknown option ${key}`);
    }
  }
  const genders = options.genders ?? 'both';
  if (!GENDER_CHOICES.includes(genders)) {
    throw new TypeError('CharacterCreator: genders must be male, female or both');
  }
  const title = options.title ?? DEFAULT_TITLE;
  if (typeof title !== 'string' || utf8Length(title) > TITLE_MAX_BYTES) {
    throw new TypeError(
      `CharacterCreator: title must be a string of at most ${TITLE_MAX_BYTES} bytes`,
    );
  }
  const appearance =
    options.appearance === undefined ? undefined : checkAppearance(options.appearance);
  if (appearance && genders !== 'both' && appearance.gender !== genders) {
    throw new RangeError(
      'CharacterCreator: the appearance is of a gender the creator does not offer',
    );
  }
  const panel = options.panel ?? true;
  if (typeof panel !== 'boolean') {
    throw new TypeError('CharacterCreator: panel must be a boolean');
  }
  return { appearance, genders, title, panel, choices: checkChoices(options.choices) };
}

function randomIndex(count) {
  return Math.min(count - 1, Math.floor(Math.random() * count));
}

function emptyLook(gender) {
  return { gender, body: '', head: '', hair: '', beard: '' };
}

class ResourceCharacterCreator {
  // ownLook() is the local player's published appearance; createPanel(creator, keys) puts the
  // panel up and returns { close(), key(name), afterLookReset() }, or null where there is none.
  constructor({ send, emit, log, ownLook, createPanel = () => null }) {
    this.send = send;
    this.emit = emit;
    this.log = log;
    this.ownLook = ownLook;
    this.createPanel = createPanel;
    this.active = null;
  }

  // The look a creator starts on: the given one, else the player's own on the man and the game's
  // default on the woman, for the first gender offered.
  startingLook(options) {
    if (options.appearance) {
      return { ...options.appearance };
    }
    const gender = options.genders === 'female' ? 'female' : 'male';
    const own = this.ownLook();
    if (gender === 'male' && own && own.gender === 'male') {
      return { gender, body: own.body, head: own.head, hair: own.hair, beard: own.beard };
    }
    return emptyLook(gender);
  }

  open(owner, rawOptions) {
    const options = checkOptions(rawOptions);
    if (this.active) {
      this.finish(null, 'replaced', false);
    }
    const look = this.startingLook(options);
    this.active = { owner, options, look, initial: { ...look }, up: false, panel: null };
    this.sendLook();
    this.send({ op: 'creatorOpen' });
    return true;
  }

  close() {
    if (!this.active) {
      return false;
    }
    this.finish(null, 'closed', true);
    return true;
  }

  isOpen() {
    return this.active !== null;
  }

  // The panel's choices.
  look() {
    return this.active ? { ...this.active.look } : null;
  }

  options() {
    return this.active ? this.active.options : null;
  }

  // Whether the creator offers `name` for `part` (the open creator's choices; all without).
  allows(part, name) {
    const choices = this.active?.options.choices;
    const names = choices ? choices[part] : undefined;
    return names === undefined || names.includes(name);
  }

  // The figure shows `look`; the resources hear characterCreatorChanged when it is another look.
  setLook(look) {
    if (!this.active) {
      return;
    }
    const before = this.active.look;
    this.active.look = { ...look };
    this.sendLook();
    const changed = ['gender', ...PARTS].some((key) => before[key] !== this.active.look[key]);
    if (changed) {
      this.emit('characterCreatorChanged', [Object.freeze({ ...this.active.look })]);
    }
  }

  // A script's look on the figure (a server's own editor): checked as open() checks one.
  preview(appearance) {
    if (!this.active) {
      return false;
    }
    const look = checkAppearance(appearance);
    const { genders } = this.active.options;
    if (genders !== 'both' && look.gender !== genders) {
      throw new RangeError(
        'CharacterCreator: the appearance is of a gender the creator does not offer',
      );
    }
    this.setLook(look);
    this.active.panel?.afterLookReset();
    return true;
  }

  // A script frames the figure: 'body' (all of it) or 'face'.
  frameAs(frame) {
    if (!FRAMES.includes(frame)) {
      throw new TypeError("CharacterCreator: frame must be 'body' or 'face'");
    }
    if (!this.active) {
      return false;
    }
    this.frame(frame === 'body' ? 'body' : 'head');
    return true;
  }

  // The camera on the whole figure, or close on its head (the face, hair and beard tabs).
  frame(part) {
    if (this.active) {
      this.send({ op: 'creatorFrame', close: part !== 'body' });
    }
  }

  reset() {
    if (this.active) {
      this.setLook(this.active.initial);
    }
  }

  // The other gender, starting from its defaults; only when both are offered.
  switchGender() {
    if (!this.active || this.active.options.genders !== 'both') {
      return false;
    }
    this.setLook(emptyLook(this.active.look.gender === 'male' ? 'female' : 'male'));
    return true;
  }

  randomize() {
    if (!this.active) {
      return;
    }
    const { gender } = this.active.look;
    if (!this.active.options.choices) {
      this.setLook(Appearances.random(gender));
      return;
    }
    // Within the choices: a part with none of this gender keeps the game's own.
    const pick = (names) => (names.length === 0 ? '' : names[randomIndex(names.length)]);
    const offered = (part, options) =>
      ['', ...options.map((option) => option.name)].filter((name) => this.allows(part, name));
    const look = { gender };
    for (const part of ['body', 'head', 'hair']) {
      look[part] = pick(offered(part, Appearances.options(part, gender)));
    }
    look.beard = pick(offered('beard', Appearances.beards(look.head, gender)));
    this.setLook(look);
  }

  accept() {
    if (this.active) {
      this.finish(Object.freeze({ ...this.active.look }), 'accepted', true);
    }
  }

  cancel() {
    if (this.active) {
      this.finish(null, 'cancelled', true);
    }
  }

  sendLook() {
    const { gender, body, head, hair, beard } = this.active.look;
    this.send({ op: 'creatorLook', gender, body, head, hair, beard });
  }

  // A press of the creator's keys, from the game client or the panel's buttons.
  key(name) {
    if (!this.active) {
      return;
    }
    const { panel } = this.active;
    if (name === 'accept') {
      this.accept();
    } else if (name === 'cancel') {
      this.cancel();
    } else if (name === 'gender' || name === 'random' || name === 'reset') {
      if (name === 'gender') {
        this.switchGender();
      } else if (name === 'random') {
        this.randomize();
      } else {
        this.reset();
      }
      panel?.afterLookReset();
    } else {
      panel?.key(name);
    }
  }

  receiveKey({ key }) {
    if (this.active?.up && typeof key === 'string') {
      this.key(key);
    }
  }

  receiveStage({ state, keys }) {
    if (!this.active) {
      return;
    }
    if (state === 'open') {
      if (!this.active.up) {
        this.active.up = true;
        // Without the panel a server's own editor drives the figure (preview, frame, accept).
        if (this.active.options.panel) {
          const shownKeys = keys && typeof keys === 'object' ? keys : {};
          this.active.panel = this.createPanel(this, shownKeys);
        }
      }
    } else if (state === 'unavailable') {
      this.finish(null, 'unavailable', false);
    } else if (state === 'left') {
      this.finish(null, 'cancelled', false);
    } else if (state === 'interrupted') {
      this.finish(null, 'interrupted', false);
    }
  }

  // The session or the world went away.
  interrupt() {
    if (this.active) {
      this.finish(null, 'interrupted', false);
    }
  }

  // The resource that opened the creator stopped.
  removeOwner(owner) {
    if (this.active && this.active.owner === owner) {
      this.finish(null, 'interrupted', true);
    }
  }

  finish(appearance, reason, closeStage) {
    const { panel } = this.active;
    this.active = null;
    if (panel) {
      try {
        panel.close();
      } catch (error) {
        this.log('host', `CharacterCreator panel: ${error}`);
      }
    }
    if (closeStage) {
      this.send({ op: 'creatorClose' });
    }
    this.emit('characterCreatorClosed', [appearance, reason]);
  }

  facade(owner) {
    const creator = this;
    return Object.freeze({
      open(options) {
        return creator.open(owner, options);
      },
      close() {
        return creator.close();
      },
      isOpen() {
        return creator.isOpen();
      },
      look() {
        return creator.isOpen() ? Object.freeze(creator.look()) : null;
      },
      preview(appearance) {
        return creator.preview(appearance);
      },
      frame(frame) {
        return creator.frameAs(frame);
      },
      accept() {
        if (!creator.isOpen()) {
          return false;
        }
        creator.accept();
        return true;
      },
      cancel() {
        if (!creator.isOpen()) {
          return false;
        }
        creator.cancel();
        return true;
      },
    });
  }
}

module.exports = { ResourceCharacterCreator, checkOptions, checkAppearance };
