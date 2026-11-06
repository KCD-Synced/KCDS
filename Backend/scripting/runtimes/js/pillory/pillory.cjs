'use strict';

// A player in a pillory (a KCDS addition): Player.putInPillory(entityName),
// Player.releaseFromPillory() and the event playerPilloryReleased(player, reason). See
//
// Being in a pillory is desired state the server keeps per player: the order goes to the player's
// own client ({ op: 'pillory', id, kind: 1 put in | 2 release | 3 the nearest, holder }) and goes
// again when the player's world gets ready again. Other players see the pose through the owner's
// replicated animation, so only the owner gets the order. The owner's client reports when it left
// the pillory, and which pillory the nearest was ({ op: 'pilloryReleased', id, reason, lock }).
// A resource keeps a player in the stocks across a relog by reading Player.pillory at
// playerDisconnect and calling putInPillory with it when they are back.
//
// Who ends a stay, and what playerPilloryReleased(player, reason) says:
//   - 'script'     releaseFromPillory, at once;
//   - 'lockpicked' the lock of the pillory was opened: the owner's client reports it, or the
//                  client of any player whose game opened that lock (the lock state is not
//                  synchronised between games, so the player who picks a pillory open does it in
//                  their own game and only their client sees it).
// A client that is out of the pillory for any other reason (the put-in could not be done, or it
// followed the server's release) reports nothing that the script hears: the state is only cleared.
//
// A lock-opened report names the lock entity; the pillory it belongs to is the one whose holder
// entity has the same prefab instance, which is the name from its first '[' to the end:
//   lock   pillory_lockpickable[Pillory/pillory1:pillory_prefab[Pillory/pillory1]_<GUID>]
//   holder so_pillory[Pillory/pillory1:pillory_prefab[Pillory/pillory1]_<GUID>]
// Every player in that pillory is released, whoever reported.

// The bounds of kcd2s/integration/pillory.hpp.
const NAME_MAX = 255;

// The kinds of the server's order and the reasons of the client's report on the wire.
const PUT_IN = 1;
const RELEASE = 2;
const PUT_IN_NEAREST = 3;
const LOCKPICKED = 1;
const LEFT = 2;
const LOCK_OPENED = 3;
const ENTERED = 4;
// The holder of a player put into the nearest pillory, until their client names it.
const NEAREST = '';

// The prefab instance of an entity name: its text from the first '[' to the end, or null when the
// name has none (a pillory placed without a prefab cannot be told from another by its lock).
function prefabInstance(name) {
  if (typeof name !== 'string') {
    return null;
  }
  const start = name.indexOf('[');
  return start === -1 ? null : name.slice(start);
}

// Whether a holder entity's name can travel: 1 to NAME_MAX printable ASCII characters.
function validName(value) {
  return (
    typeof value === 'string' &&
    value.length > 0 &&
    value.length <= NAME_MAX &&
    /^[\x20-\x7e]*$/.test(value)
  );
}

// The holder entity's name a script gave; undefined is the nearest pillory.
function holderName(value) {
  if (value === undefined) {
    return NEAREST;
  }
  if (typeof value !== 'string') {
    throw new TypeError('putInPillory takes the name of the pillory holder entity, or nothing');
  }
  if (!validName(value)) {
    throw new RangeError(
      `putInPillory takes an entity name of 1 to ${NAME_MAX} printable characters`,
    );
  }
  return value;
}

class Pillories {
  constructor(host) {
    this.host = host;
    // player id -> the holder entity name the player was put into (NEAREST until their client
    // names the nearest one).
    this.holders = new Map();
    // Handles of players that are leaving; they can no longer be put anywhere.
    this.departed = new WeakSet();
    // The pillory a leaving player was in, for Player.pillory while playerDisconnect runs.
    this.departedHolders = new WeakMap();
  }

  // Player.pillory: the holder entity name of the pillory the player is in or was put into, or
  // null. A player put into the nearest pillory reads null until their game found it.
  holderOf(player) {
    const holder = this.holders.get(player.id) ?? this.departedHolders.get(player);
    return holder === undefined || holder === NEAREST ? null : holder;
  }

  // The order that puts the player into their pillory.
  sendPutIn(player, holder) {
    if (holder === NEAREST) {
      this.send(player, PUT_IN_NEAREST, '');
    } else {
      this.send(player, PUT_IN, holder);
    }
  }

  // Whether the player can be given an order now.
  usable(player) {
    if (this.host.players.get(player.id) !== player) {
      return false;
    }
    this.host.requireLivePlayerCall(player);
    return !this.departed.has(player);
  }

  send(player, kind, holder) {
    this.host.bridge.send({ op: 'pillory', id: player.id, kind, holder });
  }

  // Player.putInPillory. True once sent; false before the player has a body. A
  // player already in a pillory is moved to the new one. Without a name, the player's game puts
  // them into the pillory nearest to them and tells the server which one it was.
  putIn(player, entityName) {
    const holder = holderName(entityName);
    if (!this.usable(player) || !player.ready) {
      return false;
    }
    this.holders.set(player.id, holder);
    this.sendPutIn(player, holder);
    return true;
  }

  // Player.releaseFromPillory. True when the player was in a pillory; playerPilloryReleased
  // follows at once with 'script'.
  release(player) {
    if (!this.usable(player) || !this.holders.has(player.id)) {
      return false;
    }
    this.finish(player, 'script');
    return true;
  }

  // Takes a player in a pillory out, tells their client and raises the event with the reason.
  finish(player, reason) {
    this.holders.delete(player.id);
    if (player.ready) {
      this.send(player, RELEASE, '');
    }
    this.host.emit('playerPilloryReleased', player, reason);
  }

  // A client reported a pillory lock opened in its game: everyone in that pillory is let out.
  lockOpened(lock) {
    const instance = prefabInstance(lock);
    if (instance === null) {
      return;
    }
    const inside = [];
    for (const [id, holder] of this.holders) {
      if (prefabInstance(holder) === instance) {
        inside.push(id);
      }
    }
    for (const id of inside) {
      const player = this.host.players.get(id);
      if (player && !this.departed.has(player)) {
        this.finish(player, 'lockpicked');
      } else {
        this.holders.delete(id);
      }
    }
  }

  // A client reported: its player is out of their pillory, or a pillory lock was opened in its game.
  report(player, e) {
    if (!player) {
      return;
    }
    if (e.reason === LOCK_OPENED) {
      this.lockOpened(e.lock);
      return;
    }
    if (!this.holders.has(player.id)) {
      // A stale or duplicate report, or one for a release the script already made.
      return;
    }
    if (e.reason === ENTERED) {
      // The nearest pillory the player's game chose; a later order named another one.
      if (this.holders.get(player.id) === NEAREST && validName(e.lock)) {
        this.holders.set(player.id, e.lock);
      }
    } else if (e.reason === LOCKPICKED) {
      this.holders.delete(player.id);
      this.host.emit('playerPilloryReleased', player, 'lockpicked');
    } else if (e.reason === LEFT) {
      this.holders.delete(player.id);
    }
  }

  // The player's world got ready again: the pillory is state, so it is ordered again.
  arrived(player) {
    const holder = this.holders.get(player.id);
    if (holder !== undefined) {
      this.sendPutIn(player, holder);
    }
  }

  // The player is leaving: no event is raised and the stay is gone with them; Player.pillory still
  // names it while playerDisconnect runs, so a resource can put them back when they return.
  left(player) {
    this.departed.add(player);
    const holder = this.holders.get(player.id);
    if (holder !== undefined) {
      this.departedHolders.set(player, holder);
    }
    this.holders.delete(player.id);
  }
}

module.exports = { Pillories };
