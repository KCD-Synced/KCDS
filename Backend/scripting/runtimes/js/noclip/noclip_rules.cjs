'use strict';

// Whether a player may fly with NoClip (a KCDS addition): Player.noclipAllowed. NoClip
// is a client script's flight, so a server that ships a fly key gives it to everyone; this lets
// the server keep it for some players. A player who is not allowed has their game told
// ({ op: 'playerNoClipAllowed', id, allowed }, and again when their world is ready), which
// refuses NoClip.enable and ends a flight that runs. Kept per connection: a player who comes back
// is allowed again until a resource says otherwise (in playerJoin, before their world loads).

class NoClipRules {
  constructor(host) {
    this.host = host;
    // The players who may not fly.
    this.forbidden = new WeakSet();
  }

  isAllowed(player) {
    return !this.forbidden.has(player);
  }

  // player.noclipAllowed = value.
  set(player, allowed) {
    if (typeof allowed !== 'boolean') {
      throw new TypeError('Player.noclipAllowed takes true or false');
    }
    if (this.host.players.get(player.id) !== player) {
      return;
    }
    this.host.requireLivePlayerCall(player);
    if (allowed === this.isAllowed(player)) {
      return;
    }
    if (allowed) {
      this.forbidden.delete(player);
    } else {
      this.forbidden.add(player);
    }
    this.apply(player);
  }

  // Tells the player's game; a player still loading hears it when their world is ready (arrived).
  apply(player) {
    if (!player.ready) {
      return;
    }
    this.host.bridge.send({
      op: 'playerNoClipAllowed',
      id: player.id,
      allowed: this.isAllowed(player),
    });
  }

  // The player's world is ready (again): a game that left a world allows NoClip again, so a
  // player who may not fly is told again.
  arrived(player) {
    if (!this.isAllowed(player)) {
      this.apply(player);
    }
  }
}

module.exports = { NoClipRules };
