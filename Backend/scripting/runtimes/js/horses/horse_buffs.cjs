'use strict';

// The status effects a server puts on a horse (Horse.addBuff). The server keeps the
// list; every client gives its body of the horse those buffs and keeps them on it (a buff that
// wears off or a body made anew gets them again), so the list is state, not a one-off order:
//
//   { op: 'horseBuffs', id, buffs: [GUID text] }   the whole list, in the order it was added.
//
// The game's buffs only: a custom buff (Buffs.register) is the server's own and runs on players.
// This list holds only script effects; those from the game stay untouched.

const { lookupBuff } = require('../buffs/catalog.cjs');

// The most buffs a horse carries from the server (kcd2s/integration/horse_buffs.hpp).
const MAX_HORSE_BUFFS = 16;

class HorseBuffs {
  constructor(host) {
    this.host = host;
    // horse -> [{ name, id }], in the order they were added
    this.lists = new WeakMap();
  }

  listOf(horse) {
    return this.lists.get(horse) ?? [];
  }

  // The game buff a GUID or table name stands for; throws for anything that is not one.
  infoOf(buff, what) {
    if (typeof buff !== 'string') {
      throw new TypeError(`${what} takes a buff's GUID or name`);
    }
    const info = lookupBuff(buff);
    if (info) {
      return info;
    }
    if (this.host.customBuffs.resolve(buff)) {
      throw new TypeError(`${what}: ${buff} is a custom buff; horses carry the game's buffs only`);
    }
    throw new TypeError(`${what}: ${buff} is not a buff`);
  }

  present(horse) {
    return this.host.horses.get(horse.id) === horse;
  }

  send(horse, list) {
    this.lists.set(horse, list);
    this.host.bridge.send({
      op: 'horseBuffs',
      id: horse.id,
      buffs: list.map((entry) => entry.id),
    });
  }

  // Horse.buffs: the names as the game's tables write them.
  names(horse) {
    return this.listOf(horse).map((entry) => entry.name);
  }

  // True when the buff is on the list, already or now; false if the horse no longer exists or one
  // that carries MAX_HORSE_BUFFS already.
  add(horse, buff) {
    const info = this.infoOf(buff, 'Horse.addBuff');
    if (!this.present(horse)) {
      return false;
    }
    const list = this.listOf(horse);
    if (list.some((entry) => entry.id === info.id)) {
      return true;
    }
    if (list.length >= MAX_HORSE_BUFFS) {
      return false;
    }
    this.send(horse, [...list, { name: info.name, id: info.id }]);
    return true;
  }

  // True when it was on the list.
  remove(horse, buff) {
    const info = this.infoOf(buff, 'Horse.removeBuff');
    const list = this.listOf(horse);
    const kept = list.filter((entry) => entry.id !== info.id);
    if (kept.length === list.length) {
      return false;
    }
    if (this.present(horse)) {
      this.send(horse, kept);
    } else {
      this.lists.set(horse, kept);
    }
    return true;
  }

  has(horse, buff) {
    const info = this.infoOf(buff, 'Horse.hasBuff');
    return this.listOf(horse).some((entry) => entry.id === info.id);
  }

  // True if a buff was removed.
  clear(horse) {
    if (this.listOf(horse).length === 0) {
      return false;
    }
    if (this.present(horse)) {
      this.send(horse, []);
    } else {
      this.lists.set(horse, []);
    }
    return true;
  }
}

module.exports = { HorseBuffs, MAX_HORSE_BUFFS };
