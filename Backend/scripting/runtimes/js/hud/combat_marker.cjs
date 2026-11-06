'use strict';

// Whether players see the game's combat marker, the crossed swords under the compass while they
// fight (a KCDS addition). The server tells every player's client
// ({ op: 'combatMarker', id, shown }) when it changes and, while it is off, when their world is
// ready; a client shows it again when its session ends, so a newcomer only hears it while it is
// off. A client resource can still hide the same HUD clip itself (Hud.setHudPart('Crime')).
// One player can have their own setting (player.setCombatMarker), as Player.setTrespassAllowed has
// for trespass: true or false over the server's, null back to the server's.

class CombatMarker {
  constructor(host) {
    this.host = host;
    this.enabled = true;
    // player -> true or false, the player's own setting.
    this.own = new WeakMap();
  }

  // Whether this player sees the marker.
  shownFor(player) {
    const own = this.own.get(player);
    return own === undefined ? this.enabled : own;
  }

  send(player) {
    if (!player?.ready) {
      return;
    }
    this.host.bridge.send({ op: 'combatMarker', id: player.id, shown: this.shownFor(player) });
  }

  arrived(player) {
    if (!this.shownFor(player)) {
      this.send(player);
    }
  }

  // player.setCombatMarker: true or false for this player, null for the server's setting.
  setFor(player, shown) {
    if (shown !== null && typeof shown !== 'boolean') {
      throw new TypeError('Player.setCombatMarker takes true, false or null');
    }
    const before = this.shownFor(player);
    if (shown === null) {
      this.own.delete(player);
    } else {
      this.own.set(player, shown);
    }
    if (this.shownFor(player) !== before) {
      this.send(player);
    }
  }

  api() {
    const self = this;
    return Object.freeze({
      // Whether players see the combat marker (true by default).
      get enabled() {
        return self.enabled;
      },
      setEnabled(enabled) {
        if (typeof enabled !== 'boolean') {
          throw new TypeError('CombatMarker.setEnabled takes true or false');
        }
        if (self.enabled === enabled) {
          return;
        }
        self.enabled = enabled;
        for (const player of self.host.players.values()) {
          if (!self.own.has(player)) {
            self.send(player);
          }
        }
      },
    });
  }
}

module.exports = { CombatMarker };
