'use strict';

const { bodyPartOf } = require('./player_damage.cjs');

// playerHit: a blow from another player, before its health is taken. The server
// raises playerHit and answers the hit player's client (the op hitVerdict, `ruling`): refused
// (0), taken as the game works it out (1), with the health set to `value` (2), the game's health
// scaled by `value` (3), or priced again by the game with the modifiers (4: `attack`, `defense`,
// `block`, `health`).
//
// A melee blow the hit player's game priced (`price` in the report, `priced: true`): the game on
// that client worked the blow out at once against the guard they held and the armour they wear,
// with its own reaction, and holds only its health for the ruling. `damage` and `engineDamage`
// are the health its pricing took; `attack`, `armor`, `blockDefense`, `bodyPartCoefficient`,
// `bodyPart` and the block flags are the numbers it priced it with; `stamina` is already spent.
// Modifiers on the attack, the armour or the block make the game price the blow again with them,
// on the same numbers, so a heavier attack still has to get through the same armour and block;
// `health` scales the health it then takes, after the game's cap.
//
// An arrow, bolt or ball (`source: 'missile'`) is held after the hit player's game
// worked it out: `damage` and `engineDamage` are that health, and the
// modifiers are ignored for it (only setting `damage` changes it).
//
// A melee blow the game could not price on its client (its pricing hooks are missing) is held
// before it is worked out, as before: `priced` is false, `damage` is the health known before the
// blow is worked out (0 when none is), and of the modifiers only `health` applies.

const MAX_DAMAGE = 100000;
const MODIFIERS = Object.freeze(['attack', 'defense', 'block', 'health']);

function amount(value) {
  return Number.isFinite(value) ? Math.min(MAX_DAMAGE, Math.max(0, value)) : 0;
}

function passes(values) {
  const [stab, slash, smash] = Array.isArray(values) ? values : [];
  return Object.freeze({ stab: amount(stab), slash: amount(slash), smash: amount(smash) });
}

// A handler's modifier, 1 when it is not a number of 0 or more.
function modifier(value) {
  return typeof value === 'number' && Number.isFinite(value) && value >= 0
    ? Math.min(MAX_DAMAGE, value)
    : 1;
}

// The PlayerHit a handler sees: `damage` and `modifiers` are theirs to change; setting `damage`
// makes it stand as set.
function playerHitOf({ damage, stamina, weapon, missile = false, price = null }) {
  const engineDamage = amount(damage);
  const priced = !missile && price !== null && typeof price === 'object';
  let ruled = engineDamage;
  let set = false;
  const hit = {
    engineDamage,
    priced,
    weapon: typeof weapon === 'string' && weapon !== '' ? weapon : null,
    attack: passes(priced ? price.attack : null),
    armor: passes(priced ? price.armor : null),
    blockDefense: priced ? amount(price.blockDefense) : 0,
    bodyPartCoefficient: priced ? amount(price.bodyPartCoefficient) : 1,
    modifiers: { attack: 1, defense: 1, block: 1, health: 1 },
    stamina: amount(stamina),
    bodyPart: priced ? bodyPartOf(price.subpart) : null,
    reason: 'combat',
    source: missile ? 'missile' : 'melee',
    blocked: priced && price.blocked === true,
    perfectBlock: priced && price.perfectBlock === true,
    blockBroken: priced && price.blockBroken === true,
  };
  Object.defineProperty(hit, 'damage', {
    enumerable: true,
    get() {
      return ruled;
    },
    set(value) {
      if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) {
        throw new RangeError('PlayerHit.damage must be a number, 0 or more');
      }
      ruled = Math.min(MAX_DAMAGE, value);
      set = true;
    },
  });
  Object.defineProperty(hit, 'ruling', {
    // The verdict once the handlers ran: { ruling, value } and, priced again, the modifiers.
    value() {
      if (set) {
        return { ruling: RULING.set, value: ruled };
      }
      if (missile) {
        return { ruling: RULING.native, value: 0 };
      }
      const scales = Object.fromEntries(
        MODIFIERS.map((name) => [name, modifier(hit.modifiers[name])]),
      );
      if (priced && (scales.attack !== 1 || scales.defense !== 1 || scales.block !== 1)) {
        return { ruling: RULING.priced, value: 0, ...scales };
      }
      if (scales.health !== 1) {
        return { ruling: RULING.scaled, value: scales.health };
      }
      return { ruling: RULING.native, value: 0 };
    },
  });
  return hit;
}

const RULING = Object.freeze({ refused: 0, native: 1, set: 2, scaled: 3, priced: 4 });

module.exports = { RULING, playerHitOf };
