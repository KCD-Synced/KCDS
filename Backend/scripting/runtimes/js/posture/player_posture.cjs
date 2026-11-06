'use strict';

// How a player's body stands: whether it is off the ground, whether it is crouched,
// and the physics profile it is in, as its owner's own game reports them
// (kcd2s/integration/posture.hpp), and its guard and raised weapon (`guard` 1 while its
// soul holds the guard, `guardUp` while its weapon or fists are raised, `fightZone` the
// combat-star direction they aim at, a row of the game's zone table, or -1). The native server
// reports them with the player; a client directory carries them as one number, `posture`: bit 0
// in the air, bit 1 crouched, bit 2 guard, bit 3 fists up, bits 8-15 the physics profile, bits
// 16-23 the combat zone plus one. The readings are the same on the server and on every client.

// The physics profile of a body that reports none.
const NO_PHYSICS_PROFILE = 255;
const NO_POSTURE = Object.freeze({
  inAir: false,
  crouched: false,
  guard: 0,
  guardUp: false,
  physicsProfile: NO_PHYSICS_PROFILE,
  fightZone: -1,
});
const POSTURE_READINGS = ['inAir', 'crouched', 'guard', 'guardUp', 'physicsProfile', 'fightZone'];

function physicsProfileOf(value) {
  if (Number.isInteger(value) && value >= 0 && value <= 255) return value;
  return NO_PHYSICS_PROFILE;
}

function combatZoneOf(value) {
  if (Number.isInteger(value) && value >= 0 && value <= 254) return value;
  return -1;
}

// The posture of a native report or record; what it lacks reads the defaults.
function postureFromNative(data) {
  return Object.freeze({
    inAir: data?.inAir === true,
    crouched: data?.crouched === true,
    guard: data?.guard === true ? 1 : 0,
    guardUp: data?.guardUp === true,
    physicsProfile: physicsProfileOf(data?.physicsProfile),
    fightZone: combatZoneOf(data?.fightZone),
  });
}

// The directory's number for a posture.
function directoryPosture(posture) {
  const inAir = posture.inAir ? 1 : 0;
  const crouched = posture.crouched ? 2 : 0;
  const guard = posture.guard ? 4 : 0;
  const guardUp = posture.guardUp ? 8 : 0;
  const profile = physicsProfileOf(posture.physicsProfile) << 8;
  const zone = (combatZoneOf(posture.fightZone) + 1) * 0x10000;
  return inAir | crouched | guard | guardUp | profile | zone;
}

// The posture of a client directory entry's number (the defaults without one); a malformed one
// throws.
function postureFromDirectory(value) {
  if (value === undefined) return NO_POSTURE;
  if (!Number.isInteger(value) || value < 0 || value > 0xffffff || (value & 0xf0) !== 0)
    throw new TypeError('Invalid native player posture');
  return Object.freeze({
    inAir: (value & 1) !== 0,
    crouched: (value & 2) !== 0,
    guard: (value & 4) !== 0 ? 1 : 0,
    guardUp: (value & 8) !== 0,
    physicsProfile: (value >> 8) & 0xff,
    fightZone: (value >> 16) - 1,
  });
}

// Defines the posture readings on a Player prototype: `postureOf(handle)` is the handle's posture
// (the defaults without one).
function definePostureReadings(prototype, postureOf) {
  for (const name of POSTURE_READINGS) {
    Object.defineProperty(prototype, name, {
      enumerable: true,
      get() {
        return (postureOf(this) ?? NO_POSTURE)[name];
      },
    });
  }
}

module.exports = {
  NO_PHYSICS_PROFILE,
  NO_POSTURE,
  postureFromNative,
  directoryPosture,
  postureFromDirectory,
  definePostureReadings,
};
