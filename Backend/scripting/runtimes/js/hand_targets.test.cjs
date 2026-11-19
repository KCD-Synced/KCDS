'use strict';

// Player.setHandTarget: what goes to the clients in the field "handTargets"
// (kcd2s/integration/hand_targets.hpp parses the same text).

const test = require('node:test');
const assert = require('node:assert/strict');
const { serverFixture } = require('../../../../Dev/testkit/guide_scenarios/fixtures.cjs');
const { fieldOf, handTargetOf } = require('./animation/hand_targets.cjs');

function lastField(sent) {
  return sent.filter((message) => message.op === 'playerHandTargets').pop();
}

test('the field has the integration header format', () => {
  const right = handTargetOf('right', {
    position: { x: 0.25, y: 0.45, z: 1.3 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
    weight: 0.8,
    blend: 0.25,
    firstPerson: { position: { x: 0.2, y: 0.55, z: 1.45 } },
  });
  const left = handTargetOf('left', {
    position: { x: -0.1, y: 0.3, z: -0.2 },
    relativeTo: 'LeftArm',
    pole: { x: -0.5, y: -0.2, z: 1 },
    blend: 0,
  });
  assert.equal(
    fieldOf({ right, left }),
    'r||250,450,1300|0,0,0,10000||80|250|200,550,1450|;' +
      'l|LeftArm|-100,300,-200||-500,-200,1000|100|0||',
  );
  assert.equal(fieldOf({ right: null, left: null }), '');
  // Euler degrees are a quaternion on the wire, as Player.rotation takes them.
  const turned = handTargetOf('right', { position: { x: 0, y: 0, z: 1 }, rotation: { z: 90 } });
  assert.ok(
    Math.abs(
      Math.hypot(turned.rotation.x, turned.rotation.y, turned.rotation.z, turned.rotation.w) - 1,
    ) < 1e-9,
  );
});

test('setHandTarget is checked, sent on change and let go with null', async (t) => {
  const { host, sent, report } = serverFixture(t);
  await host.startAll();
  const player = report(3);
  assert.deepEqual(player.handTargets, { right: null, left: null });

  player.setHandTarget('right', { position: { x: 0.2, y: 0.4, z: 1.2 } });
  assert.deepEqual(lastField(sent), {
    op: 'playerHandTargets',
    id: 3,
    field: 'r||200,400,1200|||100|200||',
  });
  assert.deepEqual(player.handTargets.right.position, { x: 0.2, y: 0.4, z: 1.2 });
  assert.equal(player.handTargets.left, null);

  const count = sent.filter((message) => message.op === 'playerHandTargets').length;
  player.setHandTarget('right', { position: { x: 0.2, y: 0.4, z: 1.2 } });
  assert.equal(sent.filter((message) => message.op === 'playerHandTargets').length, count);

  player.setHandTarget('left', { position: { x: -0.2, y: 0.4, z: 1.2 }, weight: 0.5 });
  assert.equal(lastField(sent).field, 'r||200,400,1200|||100|200||;l||-200,400,1200|||50|200||');
  player.setHandTarget('right', null);
  assert.equal(lastField(sent).field, 'l||-200,400,1200|||50|200||');
  player.setHandTarget('left', null);
  assert.equal(lastField(sent).field, '');

  assert.throws(
    () => player.setHandTarget('middle', { position: { x: 0, y: 0, z: 1 } }),
    TypeError,
  );
  assert.throws(() => player.setHandTarget('right', {}), TypeError);
  assert.throws(
    () => player.setHandTarget('right', { position: { x: 0, y: 0, z: 9 } }),
    RangeError,
  );
  assert.throws(
    () => player.setHandTarget('right', { position: { x: 0, y: 0, z: 1 }, weight: 2 }),
    RangeError,
  );
  assert.throws(
    () => player.setHandTarget('right', { position: { x: 0, y: 0, z: 1 }, relativeTo: 'a|b' }),
    TypeError,
  );
  assert.throws(
    () =>
      player.setHandTarget('right', {
        position: { x: 0, y: 0, z: 1 },
        rotation: { w: 0, x: 0, y: 0, z: 0 },
      }),
    TypeError,
  );
  assert.throws(() => player.setHandTarget('right', 5), TypeError);
});
