'use strict';

// The local player's status effects on the client: Player.buffs for the local player,
// from the native client's localBuffs message (sent when the body's effects changed). Nobody
// else's client publishes their effects, so every other handle reads none.

const { lookupBuff } = require('../../Backend/scripting/runtimes/js/buffs/catalog.cjs');
const { buffStates } = require('../../Backend/scripting/runtimes/js/buffs/player_buffs.cjs');

const MAX_BUFFS = 64;

class ResourceBuffs {
  constructor({ clock = () => performance.now() / 1000 } = {}) {
    this.clock = clock;
    this.kept = [];
  }
  // { buffs: [[GUID, instance, server, age]] }: `age` is how long the native client has seen the
  // instance, in seconds. A malformed message is dropped.
  receiveLocal({ buffs }) {
    if (!Array.isArray(buffs) || buffs.length > MAX_BUFFS) {
      return;
    }
    const now = this.clock();
    const kept = [];
    for (const item of buffs) {
      const [id, instance, server, age] = Array.isArray(item) ? item : [];
      if (
        typeof instance !== 'string' ||
        typeof server !== 'boolean' ||
        typeof age !== 'number' ||
        !Number.isFinite(age) ||
        age < 0
      ) {
        return;
      }
      const buff = lookupBuff(id);
      if (buff) {
        kept.push({ buff, instance, server, since: now - age });
      }
    }
    this.kept = kept;
  }
  // The local player's Player.buffs.
  local() {
    return buffStates(this.kept, this.clock());
  }
  reset() {
    this.kept = [];
  }
}

module.exports = { ResourceBuffs };
