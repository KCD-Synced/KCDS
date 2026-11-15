'use strict';

// The client-only Weapons global: the local player draws, holsters or toggles the selected weapon
// set, or the torch, with the game's own actions and animations
// (Client/runtime/weapons/weapon_set.hpp). A holstered weapon stays equipped, and the torch is
// apart from the weapon set.
//
//   Weapons.state()   -> { slot, hasWeapon, drawn, allowed, torch, busy } or null outside the world
//   Weapons.select(slot) -> null once weapon set `slot` (0 to 3) is selected, without drawing it
//   Weapons.draw(), Weapons.holster(), Weapons.toggle()
//   Weapons.drawTorch(), Weapons.holsterTorch(), Weapons.toggleTorch()
//                     -> null once the game's action started, or why not: 'notInWorld',
//                        'unavailable', 'noWeapon', 'alreadyDrawn', 'notDrawn', 'notAllowed',
//                        'locked', 'busy' or 'refused'
//
//   event: weaponActionEnded(action, ok, reason) with action 'draw', 'holster', 'torchDraw' or
//   'torchHolster', and reason '' when ok, else 'interrupted' or 'refused'
//
// The game client answers { op: 'syncRequest', request: { op: 'weaponsState' | 'weaponsSet' |
// 'weaponsTorch', how } } and tells { op: 'weaponsEvent', action, ok, reason }.

const REASONS = new Set([
  'notInWorld',
  'unavailable',
  'noWeapon',
  'alreadyDrawn',
  'notDrawn',
  'notAllowed',
  'locked',
  'busy',
  'refused',
]);

class ResourceWeapons {
  // sync: the SyncChannel; emit(name, args) dispatches an event.
  constructor({ sync, emit }) {
    this.sync = sync;
    this.emit = emit;
  }

  state() {
    const reply = this.sync.call({ op: 'weaponsState' });
    if (!reply || typeof reply !== 'object' || reply.available !== true) {
      return null;
    }
    return Object.freeze({
      slot: reply.slot,
      hasWeapon: reply.hasWeapon === true,
      drawn: reply.drawn === true,
      allowed: reply.allowed === true,
      torch: reply.torch === true,
      busy: reply.busy === true,
    });
  }

  // op 'weaponsSet' or 'weaponsTorch', how 'draw', 'holster' or 'toggle'.
  run(op, how) {
    if (!this.sync.available) {
      return 'unavailable';
    }
    // 'started', or why not; no answer (the game is busy loading) is 'unavailable'.
    const reply = this.sync.call({ op, how });
    if (reply === 'started') {
      return null;
    }
    return REASONS.has(reply) ? reply : 'unavailable';
  }

  // Selects weapon set `slot` (0 to 3) without drawing it.
  select(slot) {
    if (!Number.isInteger(slot) || slot < 0 || slot > 3) {
      throw new RangeError('Weapons.select takes a weapon set from 0 to 3');
    }
    if (!this.sync.available) {
      return 'unavailable';
    }
    const reply = this.sync.call({ op: 'weaponsSelect', slot });
    if (reply === 'started') {
      return null;
    }
    return REASONS.has(reply) ? reply : 'unavailable';
  }

  receiveEvent(message) {
    const { action, ok, reason } = message;
    if (typeof action !== 'string' || typeof ok !== 'boolean') {
      return;
    }
    this.emit('weaponActionEnded', [action, ok, typeof reason === 'string' ? reason : '']);
  }

  facade() {
    return Object.freeze({
      state: () => this.state(),
      select: (slot) => this.select(slot),
      draw: () => this.run('weaponsSet', 'draw'),
      holster: () => this.run('weaponsSet', 'holster'),
      toggle: () => this.run('weaponsSet', 'toggle'),
      drawTorch: () => this.run('weaponsTorch', 'draw'),
      holsterTorch: () => this.run('weaponsTorch', 'holster'),
      toggleTorch: () => this.run('weaponsTorch', 'toggle'),
    });
  }
}

module.exports = { ResourceWeapons };
