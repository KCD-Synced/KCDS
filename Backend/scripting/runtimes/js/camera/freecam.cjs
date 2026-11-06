'use strict';

// Whether players may open the game's photo mode, its free camera on F1 (a KCDS addition). The
// server tells every player's client ({ op: 'freecam', id, allowed }) when it changes and, while
// it is off, when their world is ready; a client that is not allowed keeps the game's photo mode
// key off. A client allows it again whenever it leaves a world, so a newcomer only hears it while
// it is off. NoClip (a resource's own flight) is not affected.

class Freecam {
  constructor(host) {
    this.host = host;
    this.enabled = true;
  }

  send(player) {
    if (!player?.ready) return;
    this.host.bridge.send({ op: 'freecam', id: player.id, allowed: this.enabled });
  }

  arrived(player) {
    if (!this.enabled) this.send(player);
  }

  api() {
    const self = this;
    return Object.freeze({
      // Whether players may open the game's photo mode (true by default).
      get enabled() {
        return self.enabled;
      },
      setEnabled(enabled) {
        if (typeof enabled !== 'boolean') {
          throw new TypeError('Freecam.setEnabled takes true or false');
        }
        if (self.enabled === enabled) return;
        self.enabled = enabled;
        for (const player of self.host.players.values()) self.send(player);
      },
    });
  }
}

module.exports = { Freecam };
