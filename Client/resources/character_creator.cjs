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
const OPTION_KEYS = new Set(['appearance', 'genders', 'title']);
const APPEARANCE_KEYS = new Set(['gender', ...PARTS]);

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

function checkOptions(options) {
  if (options === undefined) {
    return { appearance: undefined, genders: 'both', title: DEFAULT_TITLE };
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
  return { appearance, genders, title };
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

  setLook(look) {
    if (!this.active) {
      return;
    }
    this.active.look = { ...look };
    this.sendLook();
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
    if (this.active) {
      this.setLook(Appearances.random(this.active.look.gender));
    }
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
        this.active.panel = this.createPanel(this, keys && typeof keys === 'object' ? keys : {});
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
    });
  }
}

module.exports = { ResourceCharacterCreator, checkOptions, checkAppearance };
