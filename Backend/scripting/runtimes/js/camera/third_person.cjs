'use strict';

// The third-person camera of the local player (a KCDS addition). The server's mode
// is 'allowed' (the F5 key switches between the views, the default), 'disabled' (first person
// only) or 'forced' (third person only). The server tells every ready player's client
// ({ op: 'thirdPerson', id, mode }) when it changes and, while it is not 'allowed', when their
// world is ready; a client goes back to 'allowed' when its session ends, so a newcomer only
// hears it while it differs.

const MODES = Object.freeze({ allowed: 0, disabled: 1, forced: 2 });

class ThirdPerson {
  constructor(host) {
    this.host = host;
    this.mode = 'allowed';
  }

  send(player) {
    if (!player?.ready) {
      return;
    }
    this.host.bridge.send({ op: 'thirdPerson', id: player.id, mode: MODES[this.mode] });
  }

  arrived(player) {
    if (this.mode !== 'allowed') {
      this.send(player);
    }
  }

  api() {
    const self = this;
    return Object.freeze({
      // 'allowed' (default), 'disabled' or 'forced'.
      get mode() {
        return self.mode;
      },
      setMode(mode) {
        if (typeof mode !== 'string' || !Object.hasOwn(MODES, mode)) {
          throw new TypeError("ThirdPerson.setMode takes 'allowed', 'disabled' or 'forced'");
        }
        if (self.mode === mode) {
          return;
        }
        self.mode = mode;
        for (const player of self.host.players.values()) {
          self.send(player);
        }
      },
    });
  }
}

module.exports = { ThirdPerson };
