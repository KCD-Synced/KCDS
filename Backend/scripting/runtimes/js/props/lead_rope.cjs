'use strict';

// Player.setLeadRope: a rope from a joint of the player's body to a joint of another body (a
// player, an NPC or a horse), which every client draws each frame, a player who joins later too.
// The NetworkPlayer field `leadRope` carries it as "<target>|<from>|<to>|<length>"
// (kcd2s/integration/lead_rope.hpp), "" for none.

const LENGTH_MIN = 0.5;
const LENGTH_MAX = 20;
const LENGTH_DEFAULT = 2.5;
const JOINT_MAX = 48;
const HANDS = Object.freeze({ right: 'RightHand', left: 'LeftHand' });
const OPTIONS = new Set(['from', 'to', 'length']);

function jointOf(value, fallback, what) {
  if (value === undefined) {
    return fallback;
  }
  const joint = HANDS[value] ?? value;
  if (
    typeof joint !== 'string' ||
    joint.length === 0 ||
    joint.length > JOINT_MAX ||
    !/^[\x20-\x7e]+$/.test(joint) ||
    joint.includes('|')
  ) {
    throw new TypeError(`Player.setLeadRope ${what} must be 'right', 'left' or a joint's name`);
  }
  return joint;
}

// The rope of setLeadRope's target and options: { target, from, to, length }. Throws for
// anything else.
function leadRopeOf(target, options = {}) {
  const id = typeof target === 'number' ? target : target?.id;
  if (!Number.isInteger(id) || id < 1 || id > 0xffffffff) {
    throw new TypeError('Player.setLeadRope target must be a Player, an Npc, a Horse or null');
  }
  if (options === null || typeof options !== 'object') {
    throw new TypeError('Player.setLeadRope options must be an object');
  }
  for (const key of Object.keys(options)) {
    if (!OPTIONS.has(key)) {
      throw new TypeError(`unknown Player.setLeadRope option ${key}`);
    }
  }
  const length = options.length ?? LENGTH_DEFAULT;
  if (typeof length !== 'number' || !(length >= LENGTH_MIN && length <= LENGTH_MAX)) {
    throw new RangeError(`Player.setLeadRope length is ${LENGTH_MIN} to ${LENGTH_MAX} metres`);
  }
  return {
    target: id,
    from: jointOf(options.from, 'RightHand', 'from'),
    to: jointOf(options.to, 'Spine', 'to'),
    length,
  };
}

function leadRopeField(rope) {
  return `${rope.target}|${rope.from}|${rope.to}|${rope.length.toFixed(2)}`;
}

module.exports = { leadRopeOf, leadRopeField, LENGTH_MIN, LENGTH_MAX };
