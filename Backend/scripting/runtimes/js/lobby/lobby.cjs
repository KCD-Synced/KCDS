'use strict';

// A player held out of the world (Player.setLobby), for a server that makes a character, logs in
// or lets the player choose before they play. While held, the player is in a world of their own
// (nobody sees, hears, hits or meets them, nor they anyone), frozen, out of voice chat both ways,
// and their game makes no sound of the world; a server's loading page stays up as long as they are
// held. setLobby(false) gives back their world, freeze and voice as they were.
//
// The game side: { op: 'playerLobby', id, lobby } to the native server, which tells the player's
// game (LOBBY) whenever it changes and again when their world gets ready.

// The numbered worlds held players stand in: one per player index, above any a script is likely
// to choose and below 0xffffff (the worlds of a one-map server).
const LOBBY_WORLD_BASE = 0xfe0000;

class Lobby {
  constructor(host) {
    this.host = host;
    // Player id -> what setLobby(false) gives back: { dimension, frozen, muted, deaf }.
    this.held = new Map();
  }

  isHeld(player) {
    return this.held.has(player.id);
  }

  // Player.setLobby. True when it changed something.
  set(player, held) {
    if (typeof held !== 'boolean') {
      throw new TypeError('Player.setLobby takes true or false');
    }
    if (!this.host.connectedPlayer(player)) {
      return false;
    }
    if (held === this.isHeld(player)) {
      return false;
    }
    const voice = this.host.voiceChat;
    if (held) {
      const state = voice.state(player.id);
      this.held.set(player.id, {
        dimension: player.dimension,
        frozen: player.frozen,
        muted: state.muted,
        deaf: state.deaf,
      });
      player.setDimension(LOBBY_WORLD_BASE + (player.playerIndex % 0xffff));
      player.frozen = true;
      voice.setPlayerMuted(player, true);
      voice.setPlayerDeaf(player, true);
    } else {
      const saved = this.held.get(player.id);
      this.held.delete(player.id);
      player.setDimension(saved.dimension);
      player.frozen = saved.frozen;
      voice.setPlayerMuted(player, saved.muted);
      voice.setPlayerDeaf(player, saved.deaf);
    }
    this.host.bridge.send({ op: 'playerLobby', id: player.id, lobby: held });
    this.host.emit('playerLobbyChanged', player, held);
    return true;
  }

  // A held player's game loaded the world (again): it hears it is held.
  arrived(player) {
    if (this.isHeld(player)) {
      this.host.bridge.send({ op: 'playerLobby', id: player.id, lobby: true });
    }
  }

  // The player left: nothing is kept.
  left(id) {
    this.held.delete(id);
  }
}

module.exports = { Lobby, LOBBY_WORLD_BASE };
