'use strict';

// A frozen player (a KCDS addition): Player.frozen, as Npc.frozen holds an NPC. The
// server holds the player's body and controls: their game gets { op: 'playerFrozen', id, frozen }
// (and gets it again when their world is ready again) and lets only looking around and the menus
// through, and a body that moves anyway (a client that ignores the order) is put back where it
// was frozen. Kept per connection: a player who leaves is no longer frozen when they come back.

// How far a frozen player's body may drift from where it was frozen before it is put back (m).
const HOLD_RANGE = 0.75;
// How often a drifting body is put back at most.
const PUT_BACK_MS = 1000;

class FrozenPlayers {
  constructor(host) {
    this.host = host;
    // player -> { at: { x, y, z }, putBackAt }
    this.frozen = new WeakMap();
  }

  isFrozen(player) {
    return this.frozen.has(player);
  }

  // player.frozen = value.
  set(player, on) {
    if (typeof on !== 'boolean') {
      throw new TypeError('Player.frozen takes true or false');
    }
    if (this.host.players.get(player.id) !== player) {
      return;
    }
    this.host.requireLivePlayerCall(player);
    if (on === this.frozen.has(player)) {
      return;
    }
    if (on) {
      const { x, y, z } = player.position;
      this.frozen.set(player, { at: { x, y, z }, putBackAt: 0 });
    } else {
      this.frozen.delete(player);
    }
    this.apply(player);
  }

  // Tells the player's game; a player still loading hears it when their world is ready (arrived).
  apply(player) {
    if (!player.ready) {
      return;
    }
    this.host.bridge.send({ op: 'playerFrozen', id: player.id, frozen: this.frozen.has(player) });
  }

  // The player's world is ready (again): a frozen player is told again, and held where they are.
  arrived(player) {
    const hold = this.frozen.get(player);
    if (!hold) {
      return;
    }
    const { x, y, z } = player.position;
    hold.at = { x, y, z };
    this.apply(player);
  }

  // A script put the player somewhere: they are held there from now on.
  teleported(player, at) {
    const hold = this.frozen.get(player);
    if (hold) {
      hold.at = { x: at.x, y: at.y, z: at.z };
    }
  }

  // The player's game reported where the body is: one that drifted is put back.
  moved(player) {
    const hold = this.frozen.get(player);
    if (!hold || !player.ready) {
      return;
    }
    const now = Date.now();
    const where = player.position;
    const drift = Math.hypot(where.x - hold.at.x, where.y - hold.at.y, where.z - hold.at.z);
    if (drift <= HOLD_RANGE || now - hold.putBackAt < PUT_BACK_MS) {
      return;
    }
    hold.putBackAt = now;
    this.host.teleportPlayer(player, hold.at);
  }
}

module.exports = { FrozenPlayers, HOLD_RANGE };
