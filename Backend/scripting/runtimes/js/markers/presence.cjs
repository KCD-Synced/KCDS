'use strict';

// Who has been told about which marker or blip. Both are drawn by each player's own
// game, so the server sends one op to a player when an object comes to be shown to them and one
// when it stops; nothing is sent while nothing changes, and there is no timer.
//
// An object is shown to a ready player when it is meant for them (its `viewers` is null for every
// player, or the set of their ids) and it is in their dimension (the global world is in all of
// them). The server re-decides that only when something that feeds it changed: the object itself,
// a player's world, a player arriving or a player leaving.
//
// A "kind" (the markers, the blips) supplies `objects()`, the states it holds, and `show(player,
// state)` / `hide(player, state)`, which send the ops. A state carries `viewers`, `world` and
// `shownTo`, the ids of the players the server has shown it to.

const { GLOBAL_WORLD } = require('./values.cjs');

class Presence {
  constructor(host) {
    this.host = host;
    this.kinds = [];
    // player id -> the world the objects were last decided for; absent until the player arrived
    this.worlds = new Map();
  }

  add(kind) {
    this.kinds.push(kind);
  }

  // Whether an object is meant to be on a player's screen right now.
  wanted(state, player) {
    if (!player.ready) {
      return false;
    }
    if (state.viewers !== null && !state.viewers.has(player.id)) {
      return false;
    }
    return state.world === GLOBAL_WORLD || state.world === player.dimension;
  }

  // Shows or hides the object to one player, but only if that differs from what they were told.
  sync(kind, state, player) {
    const wanted = this.wanted(state, player);
    const shown = state.shownTo.has(player.id);
    if (wanted && !shown) {
      state.shownTo.add(player.id);
      kind.show(player, state);
    } else if (!wanted && shown) {
      state.shownTo.delete(player.id);
      kind.hide(player, state);
    }
  }

  // The object's viewers or world changed, or it is new: every player is brought up to date.
  reconcile(kind, state) {
    for (const player of this.host.players.values()) {
      this.sync(kind, state, player);
    }
  }

  // A shown object's look changed: the players who see it are sent the whole of it again.
  refresh(kind, state) {
    for (const id of state.shownTo) {
      const player = this.host.players.get(id);
      if (player) {
        kind.show(player, state);
      }
    }
  }

  // The object is going away: every player who sees it is told.
  withdraw(kind, state) {
    for (const id of state.shownTo) {
      const player = this.host.players.get(id);
      if (player) {
        kind.hide(player, state);
      }
    }
    state.shownTo.clear();
  }

  // A player's position or world was learned. Only a change of world matters here, and only for a
  // player who already arrived (arrived() sends them everything).
  placed(player, world) {
    if (!this.worlds.has(player.id) || this.worlds.get(player.id) === world) {
      return;
    }
    this.worlds.set(player.id, world);
    for (const kind of this.kinds) {
      for (const state of kind.objects()) {
        this.sync(kind, state, player);
      }
    }
  }

  // The player's world is ready (again): their game has none of it, so all of it goes to them.
  arrived(player) {
    this.worlds.set(player.id, player.dimension);
    for (const kind of this.kinds) {
      for (const state of kind.objects()) {
        state.shownTo.delete(player.id);
        this.sync(kind, state, player);
      }
    }
  }

  // The player left the server; there is no one to tell.
  left(id) {
    this.worlds.delete(id);
    for (const kind of this.kinds) {
      for (const state of kind.objects()) {
        state.shownTo.delete(id);
      }
    }
  }
}

module.exports = { Presence };
