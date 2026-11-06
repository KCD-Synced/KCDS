'use strict';

// A player's whistle for their horse (a KCDS addition): the event playerWhistle(player) and
// Player.whistle().
//
// A player's game reports their whistle (the companion key, while Interactions allows
// horseWhistle, or Player.whistle) as the playerWhistled op. The server raises playerWhistle and
// tells every other player in the same world within WHISTLE_RANGE ({ op: 'whistle', id,
// whistler }), whose game plays the whistle on the whistler's body, so the game's own 3D sound
// makes it quieter with the distance. Player.whistle() makes the player's own game whistle as the
// key does (the horse comes), whatever Interactions allows.

// How far a whistle carries, in metres.
const WHISTLE_RANGE = 100;

class Whistles {
  constructor(host) {
    this.host = host;
  }

  // The playerWhistled op: the player's game whistled.
  reported(player) {
    if (!player || !player.ready) {
      return;
    }
    this.host.emit('playerWhistle', player);
    const from = player.position;
    for (const listener of this.host.players.values()) {
      if (listener === player || !listener.ready) {
        continue;
      }
      if (listener.dimension !== player.dimension) {
        continue;
      }
      const at = listener.position;
      const distance = Math.hypot(at.x - from.x, at.y - from.y, at.z - from.z);
      if (distance > WHISTLE_RANGE) {
        continue;
      }
      this.host.bridge.send({ op: 'whistle', id: listener.id, whistler: player.id });
    }
  }

  // Player.whistle: true once sent; false until the player has entered the world.
  whistle(player) {
    if (this.host.players.get(player.id) !== player || !player.ready) {
      return false;
    }
    this.host.requireLivePlayerCall(player);
    this.host.bridge.send({ op: 'whistle', id: player.id, whistler: 0 });
    return true;
  }
}

module.exports = { Whistles, WHISTLE_RANGE };
