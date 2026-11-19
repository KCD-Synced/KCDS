'use strict';

// Animations.skeleton() for client scripts: the local body's joints, each with its parent and its
// rest pose, asked of the game client (Client/runtime/animation/skeleton_info.hpp:
// { op: 'localWorldSkeleton' } answers { names, parents, rest }, compact as a message to the runtime
// holds at most 64 KB). The call waits for the game's next frame, as World.raycast does.

const ANSWER_MS = 2000;
// The rest pose's units: rotations in ten-thousandths, positions in hundredths of a millimetre.
const TURN_UNITS = 10000;
const PLACE_UNITS = 100000;

// a * b for quaternions { x, y, z, w }.
function multiply(a, b) {
  return {
    x: a.w * b.x + b.w * a.x + (a.y * b.z - a.z * b.y),
    y: a.w * b.y + b.w * a.y + (a.z * b.x - a.x * b.z),
    z: a.w * b.z + b.w * a.z + (a.x * b.y - a.y * b.x),
    w: a.w * b.w - (a.x * b.x + a.y * b.y + a.z * b.z),
  };
}

// v turned by q.
function rotate(q, v) {
  const u = {
    x: 2 * (q.y * v.z - q.z * v.y),
    y: 2 * (q.z * v.x - q.x * v.z),
    z: 2 * (q.x * v.y - q.y * v.x),
  };
  return {
    x: v.x + q.w * u.x + (q.y * u.z - q.z * u.y),
    y: v.y + q.w * u.y + (q.z * u.x - q.x * u.z),
    z: v.z + q.w * u.z + (q.x * u.y - q.y * u.x),
  };
}

// The answer's joints: [{ name, parent (index), turn, at }] in the body's space, or null for an
// answer that is not one.
function jointsOf(reply) {
  if (
    !reply ||
    typeof reply !== 'object' ||
    typeof reply.names !== 'string' ||
    typeof reply.parents !== 'string' ||
    typeof reply.rest !== 'string'
  ) {
    return null;
  }
  const names = reply.names.split(',');
  const parents = reply.parents.split(',').map(Number);
  const rest = reply.rest.split(',').map(Number);
  if (
    names.length === 0 ||
    parents.length !== names.length ||
    rest.length !== names.length * 7 ||
    !parents.every((parent) => Number.isInteger(parent) && parent >= -1 && parent < names.length) ||
    !rest.every(Number.isFinite)
  ) {
    return null;
  }
  return names.map((name, index) => {
    const at = index * 7;
    return {
      name,
      parent: parents[index],
      turn: {
        x: rest[at] / TURN_UNITS,
        y: rest[at + 1] / TURN_UNITS,
        z: rest[at + 2] / TURN_UNITS,
        w: rest[at + 3] / TURN_UNITS,
      },
      at: {
        x: rest[at + 4] / PLACE_UNITS,
        y: rest[at + 5] / PLACE_UNITS,
        z: rest[at + 6] / PLACE_UNITS,
      },
    };
  });
}

class ResourceSkeleton {
  constructor({ sync = null, Vector3, Quaternion }) {
    this.sync = sync;
    this.Vector3 = Vector3;
    this.Quaternion = Quaternion;
  }

  // [{ name, parent, position, rotation, restPosition, restRotation }], or null without a body:
  // position and rotation relative to the parent's rest pose, the rest ones in the body's space.
  skeleton() {
    const reply = this.sync ? this.sync.call({ op: 'localWorldSkeleton' }, ANSWER_MS) : undefined;
    const joints = jointsOf(reply);
    if (!joints) {
      return null;
    }
    const { Vector3, Quaternion } = this;
    return joints.map((joint) => {
      let turn = joint.turn;
      let at = joint.at;
      const parent = joint.parent >= 0 ? joints[joint.parent] : null;
      if (parent) {
        const inverse = {
          x: -parent.turn.x,
          y: -parent.turn.y,
          z: -parent.turn.z,
          w: parent.turn.w,
        };
        turn = multiply(inverse, joint.turn);
        at = rotate(inverse, {
          x: joint.at.x - parent.at.x,
          y: joint.at.y - parent.at.y,
          z: joint.at.z - parent.at.z,
        });
      }
      return {
        name: joint.name,
        parent: parent ? parent.name : null,
        position: new Vector3(at.x, at.y, at.z),
        rotation: new Quaternion(turn.w, turn.x, turn.y, turn.z),
        restPosition: new Vector3(joint.at.x, joint.at.y, joint.at.z),
        restRotation: new Quaternion(joint.turn.w, joint.turn.x, joint.turn.y, joint.turn.z),
      };
    });
  }

  facade() {
    const skeleton = this;
    return Object.freeze({
      skeleton() {
        return skeleton.skeleton();
      },
    });
  }
}

module.exports = { ResourceSkeleton };
