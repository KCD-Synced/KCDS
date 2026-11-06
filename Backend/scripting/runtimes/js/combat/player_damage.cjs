'use strict';

// Health a player's own game took off them: the reasons and body parts of the native
// PLAYER_DAMAGE report (kcd2s/integration/damage.hpp), as playerDamage names them.

// E_DamageReason, in the game's order.
const REASONS = Object.freeze([
  'unknown',
  'combat',
  'gunshot',
  'starvation',
  'collision',
  'scripted',
  'disintegrate',
  'fall',
  'poison',
  'bleeding',
  'selfHarm',
]);

// The game's body_subpart table for a human body: each body part's subparts.
const SUBPARTS = Object.freeze({
  head: [23, 24, 25, 26, 27, 28, 101],
  torso: [1, 2, 3, 62, 63, 90, 92, 93],
  leftArm: [30, 36, 37, 38, 39, 40, 41, 48],
  rightArm: [29, 42, 43, 44, 45, 46, 47],
  leftLeg: [49, 50, 51, 52, 53, 54],
  rightLeg: [55, 56, 57, 58, 59, 60, 61],
});
// An animal's, from the same table (Libs/Tables/item/body_subpart.xml with
// body_part.xml): its head, torso, legs and a dog's eyes, named alike for every animal.
const ANIMAL_SUBPARTS = Object.freeze({
  // a horse's, a sheep's, a dog's, cattle's, a pig's, a red deer's and a roe deer's
  head: [64, 65, 78, 79, 106, 107, 108, 109, 110, 111],
  // a horse's, a dog's, cattle's, a sheep's, a pig's, a stag's, a doe's, a roe deer's, a hare's
  torso: [80, 81, 82, 83, 88, 89, 91, 94, 102, 95, 96, 97, 98, 99, 100, 104],
  eyes: [105],
  // a red deer's, a roe deer's, cattle's and a dog's
  frontLegs: [112, 113, 116, 118],
  backLegs: [114, 115, 117, 119],
  // a horse's legs and feet
  legs: [84, 85, 86, 87],
  feet: [120],
});
const BODY_PART_OF = new Map(
  [SUBPARTS, ANIMAL_SUBPARTS].flatMap((table) =>
    Object.entries(table).flatMap(([part, subparts]) => subparts.map((id) => [id, part])),
  ),
);

// The body part a body subpart belongs to, or null for none in particular.
function bodyPartOf(subpart) {
  return BODY_PART_OF.get(subpart) ?? null;
}

// { amount, reason, bodyPart } of a report, or null for a malformed one.
function damageOf({ amount, reason, subpart }) {
  if (typeof amount !== 'number' || !(amount > 0) || !Number.isInteger(reason)) {
    return null;
  }
  return {
    amount,
    reason: REASONS[reason] ?? 'unknown',
    bodyPart: bodyPartOf(subpart),
  };
}

module.exports = { REASONS, bodyPartOf, damageOf };
