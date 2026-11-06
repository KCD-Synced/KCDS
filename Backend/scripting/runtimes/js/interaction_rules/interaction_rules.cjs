'use strict';

// The game's own interactions a server allows (a KCDS addition): its single-player prompts and
// keys on people are off on a KCDS server until a resource turns them on, for every player
// (Interactions.set) or one (Player.setInteraction).
//
//   horseWhistle  the companion key's whistle that calls the player's horse
//   npcTalk       "Talk" on an NPC (the game's own dialogue); never allowed
//   npcChat       the focus chat on an NPC (where the game provokes and insults); never allowed
//   playerTalk    "Talk" and the focus chat on another player
//   npcChatter    the NPCs' own talk (their barks and lines, in a fight too): on by default, as in
//                 the game; off, every player's game turns its dialog system's talk off
//                 (wh_dlg_Enable), so no NPC says anything
//   pointing      the pointing key (B): the arm points where the camera looks, on every screen;
//                 on by default
//   trespass      being in the game's private and prohibited areas: no warning on the HUD, and
//                 the game's AI is told not to react; off by default, as in the game
//   cooking       the prompt and the use key at the game's fires; on by default
//   smoking       the prompt and the use key at its smokehouses and drying racks; on by default
//
// The player's game gets { op: 'interactionRules', id, allowed } (the bits of
// kcd2s/integration/interaction_rules.hpp) whenever what they are allowed changes, counting from
// nothing allowed, which every game has in a new world.

const BITS = Object.freeze({
  horseWhistle: 1,
  npcTalk: 2,
  npcChat: 4,
  playerTalk: 8,
  npcChatter: 16,
  pointing: 32,
  trespass: 64,
  cooking: 128,
  smoking: 256,
});
// The NPC conversations crashed a player's game ("Vyřešíme to spolu"): they stay off
// whatever a resource sets. Set calls are accepted and have no effect; get and the player's
// interactionAllowed read false.
const NEVER = new Set(['npcTalk', 'npcChat']);
// What a new server allows: the game's prompts off, the NPCs' own talk on.
const DEFAULTS = Object.freeze({ npcChatter: true, pointing: true, cooking: true, smoking: true });
// Bits sent when the interaction is NOT allowed (the client keeps the game's talk unless told).
const INVERTED = new Set(['npcChatter', 'pointing', 'cooking', 'smoking']);
const NAMES = Object.freeze(Object.keys(BITS));

function requireName(name, what) {
  if (!NAMES.includes(name)) {
    throw new TypeError(
      `${what}: unknown interaction ${String(name)} (one of ${NAMES.join(', ')})`,
    );
  }
}

function requireAllowed(allowed, what, overrideable) {
  if (typeof allowed === 'boolean') {
    return;
  }
  if (overrideable && allowed === null) {
    return;
  }
  throw new TypeError(`${what} takes true or false${overrideable ? ' (or null)' : ''}`);
}

class InteractionRules {
  constructor(host) {
    this.host = host;
    // The server's own setting of each interaction, for every player.
    this.defaults = new Map(NAMES.map((name) => [name, DEFAULTS[name] === true]));
    // A player's own settings: player -> Map(name -> allowed).
    this.overrides = new WeakMap();
    // The bits each player's game was last told.
    this.told = new WeakMap();
  }

  allowedFor(player, name) {
    if (NEVER.has(name)) {
      return false;
    }
    const own = this.overrides.get(player);
    if (own && own.has(name)) {
      return own.get(name);
    }
    return this.defaults.get(name);
  }

  bitsOf(player) {
    let bits = 0;
    for (const name of NAMES) {
      if (this.allowedFor(player, name) !== INVERTED.has(name)) {
        bits |= BITS[name];
      }
    }
    return bits;
  }

  // Tells the player's game what it allows now, when that changed; a player still loading hears
  // it when their world is ready (arrived).
  apply(player) {
    if (!player.ready) {
      return;
    }
    const bits = this.bitsOf(player);
    if ((this.told.get(player) ?? 0) === bits) {
      return;
    }
    this.told.set(player, bits);
    this.host.bridge.send({ op: 'interactionRules', id: player.id, allowed: bits });
  }

  // Interactions.set: the interaction for every player without a setting of their own.
  set(name, allowed) {
    requireName(name, 'Interactions.set');
    requireAllowed(allowed, 'Interactions.set', false);
    this.defaults.set(name, allowed);
    for (const player of this.host.players.values()) {
      this.apply(player);
    }
  }

  // Player.setInteraction: the player's own setting; null goes back to the server's.
  setFor(player, name, allowed) {
    requireName(name, 'Player.setInteraction');
    requireAllowed(allowed, 'Player.setInteraction', true);
    if (this.host.players.get(player.id) !== player) {
      return false;
    }
    let own = this.overrides.get(player);
    if (!own) {
      own = new Map();
      this.overrides.set(player, own);
    }
    if (allowed === null) {
      own.delete(name);
    } else {
      own.set(name, allowed);
    }
    this.apply(player);
    return true;
  }

  // The player's world is ready (again): their game starts with nothing allowed, so it hears
  // what is.
  arrived(player) {
    this.told.set(player, 0);
    this.apply(player);
  }

  api() {
    return Object.freeze({
      names: NAMES,
      set: (name, allowed) => this.set(name, allowed),
      get: (name) => {
        requireName(name, 'Interactions.get');
        return NEVER.has(name) ? false : this.defaults.get(name);
      },
    });
  }
}

module.exports = { InteractionRules, INTERACTION_BITS: BITS, INTERACTION_NAMES: NAMES };
