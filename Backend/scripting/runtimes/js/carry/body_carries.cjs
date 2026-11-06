'use strict';

// A player carrying another player's knocked-out body on the shoulder, the game's own
// corpse carry. The carrier's client reports its game's pick-up and let-go ({ op:
// 'bodyCarryReport', id, kind: started | ended, carried }); the server decides (playerCarrying,
// refusable), and tells every other client to show the carry ({ op: 'bodyCarry', id, kind:
// carry | drop | putDown | refuse, carrier, carried }), the carried player's own game too.

class BodyCarries {
  constructor(host, isDown) {
    this.host = host;
    // Whether a player lies knocked out (the host's playerDown).
    this.isDown = isDown;
    // carrier id -> carried id.
    this.carries = new Map();
  }

  send(to, kind, carrier, carried) {
    if (!to?.ready) return;
    this.host.bridge.send({ op: 'bodyCarry', id: to.id, kind, carrier, carried });
  }

  // Every player but the carrier shows the carry (or its end).
  tellOthers(kind, carrierId, carriedId) {
    for (const player of this.host.players.values()) {
      if (player.id !== carrierId) this.send(player, kind, carrierId, carriedId);
    }
  }

  carrying(player) {
    const carried = this.carries.get(player?.id);
    return carried === undefined ? null : this.host.players.get(carried) ?? null;
  }

  heldBy(player) {
    for (const [carrier, carried] of this.carries) {
      if (carried === player?.id) return this.host.players.get(carrier) ?? null;
    }
    return null;
  }

  // A carrier's client reported its game's pick-up or let-go.
  report(carrier, e) {
    if (!carrier) return;
    const carried = this.host.players.get(Number(e.carried));
    if (e.kind === 'started') {
      const allowed =
        Boolean(carried) &&
        carried !== carrier &&
        this.isDown(carried) &&
        !this.carries.has(carrier.id) &&
        !this.heldBy(carried) &&
        this.host.emit('playerCarrying', carrier, carried);
      if (!allowed) {
        this.send(carrier, 'refuse', carrier.id, Number(e.carried));
        return;
      }
      this.carries.set(carrier.id, carried.id);
      this.tellOthers('carry', carrier.id, carried.id);
      this.host.emit('playerCarry', carrier, carried);
      return;
    }
    if (e.kind === 'ended' && carried && this.carries.get(carrier.id) === carried.id) {
      this.end(carrier, carried);
    }
  }

  end(carrier, carried) {
    this.carries.delete(carrier.id);
    this.tellOthers('drop', carrier.id, carried.id);
    this.host.emit('playerPutDown', carrier, carried);
  }

  // Player.putDown: the carrier's game puts the body down; the carry ends when it reports that.
  putDown(carrier) {
    const carried = this.carries.get(carrier?.id);
    if (carried === undefined || !carrier.ready) return false;
    this.send(carrier, 'putDown', carrier.id, carried);
    return true;
  }

  // The carried player was revived: the carrier's game lets go, and the carry is over.
  revived(player) {
    const carrier = this.heldBy(player);
    if (!carrier) return;
    this.send(carrier, 'putDown', carrier.id, player.id);
    this.end(carrier, player);
  }

  // A newcomer's game shows the carries under way.
  arrived(player) {
    for (const [carrier, carried] of this.carries) {
      if (carrier !== player.id) this.send(player, 'carry', carrier, carried);
    }
  }

  // A carrier or a carried player is leaving: the carry is over (the leaving player is still
  // readable in playerPutDown).
  left(player) {
    const carried = this.carrying(player);
    if (carried) {
      this.end(player, carried);
    }
    const carrier = this.heldBy(player);
    if (carrier) {
      this.send(carrier, 'putDown', carrier.id, player.id);
      this.end(carrier, player);
    }
  }
}

module.exports = { BodyCarries };
