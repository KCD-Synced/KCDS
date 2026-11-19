'use strict';

// Animations.skeleton on the client (Client/resources/skeleton.cjs): the local body's joints with
// their parents and rest pose, from the game client's compact answer.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceSkeleton } = require('../../../../Client/resources/skeleton.cjs');
const { Vector3, Quaternion } = require('./math.cjs');

function skeletonWith(answer) {
  const asked = [];
  const sync = {
    call(request) {
      asked.push(request);
      return answer();
    },
  };
  return { Animations: new ResourceSkeleton({ sync, Vector3, Quaternion }).facade(), asked };
}

test('skeleton lists the joints with parents, rest pose and pose relative to the parent', () => {
  // The root at the feet; the upper arm 1.4 m up and 0.2 m right, turned a quarter about up; the
  // forearm 0.3 m below the upper arm, turned the same.
  const quarter = [0, 0, 7071, 7071];
  const { Animations, asked } = skeletonWith(() => ({
    names: 'Bip01,RightArm,RightForeArm',
    parents: '-1,0,1',
    rest: [
      [0, 0, 0, 10000, 0, 0, 0],
      [...quarter, 20000, 0, 140000],
      [...quarter, 20000, 0, 110000],
    ]
      .flat()
      .join(','),
  }));
  const joints = Animations.skeleton();
  assert.deepEqual(asked, [{ op: 'localWorldSkeleton' }]);
  assert.equal(joints.length, 3);
  assert.equal(joints[0].parent, null);
  assert.equal(joints[2].name, 'RightForeArm');
  assert.equal(joints[2].parent, 'RightArm');
  assert.deepEqual(joints[1].restPosition.toJSON(), { x: 0.2, y: 0, z: 1.4 });
  // Relative to the upper arm: 0.3 m down, no turn of its own.
  const below = joints[2].position;
  assert.ok(Math.abs(below.x) < 1e-6 && Math.abs(below.y) < 1e-6 && Math.abs(below.z + 0.3) < 1e-6);
  assert.ok(Math.abs(joints[2].rotation.w - 1) < 1e-3);
  assert.ok(Math.abs(joints[1].rotation.z - 0.7071) < 1e-6, 'the root has no turn: the same');
});

test('skeleton is null without a body or for a malformed answer', () => {
  assert.equal(skeletonWith(() => null).Animations.skeleton(), null);
  assert.equal(skeletonWith(() => undefined).Animations.skeleton(), null);
  assert.equal(
    skeletonWith(() => ({ names: 'a,b', parents: '-1,0', rest: '1,2,3' })).Animations.skeleton(),
    null,
  );
  assert.equal(
    skeletonWith(() => ({
      names: 'a',
      parents: '5',
      rest: '0,0,0,10000,0,0,0',
    })).Animations.skeleton(),
    null,
  );
});
