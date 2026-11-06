'use strict';

// Whether players see the game's restricted-area marker, the warning that they are trespassing (a
// KCDS addition). The server tells every player's client
// ({ op: 'restrictedAreaMarker', id, shown }) when it changes and, while it is off, when their
// world is ready; a client shows it again when its session ends, so a newcomer only hears it
// while it is off. A client resource can still hide the same HUD clip itself
// (Hud.setHudPart('Trespassing')).

class RestrictedAreaMarker {
  constructor(host) {
    this.host = host;
    this.enabled = true;
  }

  send(player) {
    if (!player?.ready) {
      return;
    }
    this.host.bridge.send({ op: 'restrictedAreaMarker', id: player.id, shown: this.enabled });
  }

  arrived(player) {
    if (!this.enabled) {
      this.send(player);
    }
  }

  api() {
    const self = this;
    return Object.freeze({
      // Whether players see the restricted-area marker (true by default).
      get enabled() {
        return self.enabled;
      },
      setEnabled(enabled) {
        if (typeof enabled !== 'boolean') {
          throw new TypeError('RestrictedAreaMarker.setEnabled takes true or false');
        }
        if (self.enabled === enabled) {
          return;
        }
        self.enabled = enabled;
        for (const player of self.host.players.values()) {
          self.send(player);
        }
      },
    });
  }
}

module.exports = { RestrictedAreaMarker };
