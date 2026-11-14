'use strict';

// Horses that belong to players: which horses each player was given, which of them
// were stolen, and the removal of a horse whose owner left.
//
// User decisions:
// - "When is horse given to player its not steal its your horse but despawn is needed if player
//   is offline 15 min and is horse not stealen": a horse given with giveTo is removed once its
//   owner has been disconnected for OWNER_ABSENCE_MS, unless it was stolen.
// - No scheduled sweep over all horses, a cheap clear instead, with a guard that no horse goes
//   before 15 minutes, for the server's memory and CPU: nothing goes over all horses on a
//   schedule. An owner's disconnect starts one timer for that owner's horses alone. A theft, a
//   giveTo or a destroy takes a horse off it, and the timer is cleared once none of that owner's
//   horses waits any more. When it runs out, each waiting horse is checked again (the owner away
//   for the whole delay, the horse not stolen) before it is removed; a timer that ran early is set
//   again for the rest, so no horse goes before the delay.
//
// A horse with removedWithOwner false does not wait at all: the moment its owner disconnects it is
// left ownerless (giveTo(null), horseOwnerChanged with a null player) and stays until a resource
// gives it to someone. It is acted on in ownerLeft, so no timer is kept for it.
//
// A horse is stolen when a player other than its owner mounted it (the native rider report:
// mounting another player's horse is the game's own steal). It is the owner's again once the owner
// rides it, or once a resource gives it to someone. The owner's id is never given to a later
// connection (Player.id), so a returning owner is a new player: a resource that knows them gives
// the horse back with giveTo, which ends the wait.

// How long a horse waits for its owner after they disconnected.
const OWNER_ABSENCE_MS = 15 * 60 * 1000;

// Live tests only: KCDS_HORSE_OWNER_ABSENCE_SECONDS shortens the wait when the server runs with
// KCDS_TEST_HOOKS; any other server keeps the user's 15 minutes. The wait in ms, or undefined.
function testAbsenceMs(env) {
  if (!env.KCDS_TEST_HOOKS) {
    return undefined;
  }
  const seconds = Number(env.KCDS_HORSE_OWNER_ABSENCE_SECONDS);
  if (!Number.isFinite(seconds) || seconds <= 0) {
    return undefined;
  }
  return seconds * 1000;
}

function unrefTimer(fn, ms) {
  const timer = setTimeout(fn, ms);
  // A waiting horse never keeps the resource host's process alive.
  timer.unref?.();
  return timer;
}

class HorseOwners {
  // connected(id): whether that player is connected now. remove(horse): despawns it.
  // release(horse): leaves it ownerless (giveTo(null)); a horse that is not set to be destroyed
  // with its owner (removedWithOwner false) is released the moment the owner leaves.
  constructor({
    delayMs = OWNER_ABSENCE_MS,
    clock = () => performance.now(),
    setTimer = unrefTimer,
    clearTimer = (timer) => clearTimeout(timer),
    connected,
    remove,
    release = () => {},
    log = () => {},
  }) {
    this.delayMs = delayMs;
    this.clock = clock;
    this.setTimer = setTimer;
    this.clearTimer = clearTimer;
    this.connected = connected;
    this.remove = remove;
    this.release = release;
    this.log = log;
    // owner id -> Set of the horses given to that player (connected or not)
    this.byOwner = new Map();
    // horse -> id of the player who stole it
    this.thieves = new Map();
    // owner id -> { since (clock ms), timer }, for owners who left and still have a horse waiting
    this.absences = new Map();
  }

  // The horse was given to its current ownerId (giveTo); `previousOwnerId` had it before.
  owned(horse, previousOwnerId) {
    this.thieves.delete(horse);
    this.drop(horse, previousOwnerId);
    if (!horse.ownerId) {
      return;
    }
    let horses = this.byOwner.get(horse.ownerId);
    if (!horses) {
      horses = new Set();
      this.byOwner.set(horse.ownerId, horses);
    }
    horses.add(horse);
  }

  // The native report changed the horse's rider (0 when nobody rides it).
  riderChanged(horse, riderId) {
    if (!riderId || !horse.ownerId) {
      return;
    }
    if (riderId === horse.ownerId) {
      if (this.thieves.delete(horse)) {
        this.log(`horse ${horse.id} is back with its owner ${horse.ownerId}`);
      }
      return;
    }
    if (!this.thieves.has(horse)) {
      this.log(`horse ${horse.id} stolen from player ${horse.ownerId} by player ${riderId}`);
    }
    this.thieves.set(horse, riderId);
    this.settle(horse.ownerId);
  }

  stolen(horse) {
    return this.thieves.has(horse);
  }

  // The id of the player who stole the horse, 0 when it is not stolen.
  thiefOf(horse) {
    return this.thieves.get(horse) ?? 0;
  }

  // Whether `riderId` mounting the horse now steals it: it has an owner, the rider is not the
  // owner, and the rider has not stolen it already.
  wouldSteal(horse, riderId) {
    return (
      Boolean(riderId) &&
      Boolean(horse.ownerId) &&
      riderId !== horse.ownerId &&
      this.thieves.get(horse) !== riderId
    );
  }

  // The horse is being removed.
  destroyed(horse) {
    this.thieves.delete(horse);
    this.drop(horse, horse.ownerId);
  }

  // The horses given to this player, in no particular order (an index per owner, so nothing
  // scans all horses). A horse still named for an owner who left, while it waits, is included.
  horsesOf(ownerId) {
    return [...(this.byOwner.get(ownerId) ?? [])];
  }

  // The player disconnected: their horses that were not stolen wait for them, and those set to
  // outlive their owner are left ownerless at once.
  ownerLeft(ownerId) {
    for (const horse of this.horsesOf(ownerId)) {
      if (horse.removedWithOwner === false) {
        this.log(`horse ${horse.id} stays ownerless: its owner ${ownerId} left`);
        this.release(horse);
      }
    }
    if (this.absences.has(ownerId)) {
      return;
    }
    const waiting = this.waitingHorses(ownerId);
    if (!waiting.length) {
      return;
    }
    const since = this.clock();
    const timer = this.setTimer(() => this.expire(ownerId), this.delayMs);
    this.absences.set(ownerId, { since, timer });
    const ids = waiting.map((horse) => horse.id).join(', ');
    this.log(`horse ${ids} waits ${this.delayMs / 1000} s for its owner ${ownerId}`);
  }

  // Whether the horse waits for its disconnected owner.
  waiting(horse) {
    return this.absences.has(horse.ownerId) && !this.thieves.has(horse);
  }

  // The owner's timer ran out: their horses that still wait are removed.
  expire(ownerId) {
    const absence = this.absences.get(ownerId);
    if (!absence) {
      return;
    }
    const away = this.clock() - absence.since;
    if (away < this.delayMs) {
      absence.timer = this.setTimer(() => this.expire(ownerId), this.delayMs - away);
      return;
    }
    this.absences.delete(ownerId);
    if (this.connected(ownerId)) {
      return;
    }
    const seconds = Math.round(away / 1000);
    for (const horse of this.waitingHorses(ownerId)) {
      this.log(`horse ${horse.id} removed: its owner ${ownerId} has been away ${seconds} s`);
      this.remove(horse);
    }
  }

  // Every timer goes (the resource host shuts down).
  clear() {
    for (const absence of this.absences.values()) {
      this.clearTimer(absence.timer);
    }
    this.absences.clear();
  }

  waitingHorses(ownerId) {
    const horses = this.byOwner.get(ownerId);
    if (!horses) {
      return [];
    }
    return [...horses].filter(
      (horse) => !this.thieves.has(horse) && horse.removedWithOwner !== false,
    );
  }

  // The horse is no longer ownerId's.
  drop(horse, ownerId) {
    const horses = this.byOwner.get(ownerId);
    if (!horses) {
      return;
    }
    horses.delete(horse);
    if (!horses.size) {
      this.byOwner.delete(ownerId);
    }
    this.settle(ownerId);
  }

  // A departed owner whose horses no longer wait needs no timer.
  settle(ownerId) {
    const absence = this.absences.get(ownerId);
    if (!absence || this.waitingHorses(ownerId).length) {
      return;
    }
    this.clearTimer(absence.timer);
    this.absences.delete(ownerId);
  }
}

module.exports = { HorseOwners, OWNER_ABSENCE_MS, testAbsenceMs };
