'use strict';

// The body parts of a hit's body subpart (combat/player_damage.cjs): a person's, and an animal's
// as playerDamage and npcDamage name them.
const test = require('node:test');
const assert = require('node:assert/strict');
const { bodyPartOf, damageOf } = require('./combat/player_damage.cjs');

test('a person body subpart names its body part', () => {
  assert.equal(bodyPartOf(23), 'head');
  assert.equal(bodyPartOf(1), 'torso');
  assert.equal(bodyPartOf(29), 'rightArm');
  assert.equal(bodyPartOf(-1), null);
});

test('an animal body subpart names its body part', () => {
  assert.equal(bodyPartOf(107), 'head'); // a dog's head
  assert.equal(bodyPartOf(94), 'torso'); // a dog's torso
  assert.equal(bodyPartOf(105), 'eyes'); // a dog's eyes
  assert.equal(bodyPartOf(118), 'frontLegs'); // a dog's front legs
  assert.equal(bodyPartOf(119), 'backLegs');
  assert.equal(bodyPartOf(98), 'torso'); // a stag's
  assert.equal(bodyPartOf(110), 'head'); // a red deer's
  assert.equal(bodyPartOf(84), 'legs'); // a horse's
  assert.equal(bodyPartOf(120), 'feet');
  assert.equal(bodyPartOf(500), null);
  assert.deepEqual(damageOf({ amount: 12, reason: 1, subpart: 100 }), {
    amount: 12,
    reason: 'combat',
    bodyPart: 'torso',
  });
});
