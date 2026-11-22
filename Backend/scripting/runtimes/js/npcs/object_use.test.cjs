'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { seatGeometry } = require('./object_use.cjs');

const { helpers, downStart, multiply, quaternion } = seatGeometry;

// A turn by `degrees` about the up axis, as the seat data stores turns.
function yaw(degrees) {
  const half = (degrees * Math.PI) / 360;
  return quaternion([0, 0, Math.sin(half), Math.cos(half)]);
}

function sameTurn(a, b) {
  // q and -q are the same turn.
  const dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  return Math.abs(Math.abs(dot) - 1) < 1e-6;
}

test('a bench bed lain on from its own side ends turned away, whatever the rounding', () => {
  const bed = helpers.Bed_1Place_Bench;
  assert.equal(bed.category, 'Lie');
  const seatAt = { x: 1543.9, y: 2025.5, z: 146.8 };
  for (const approach of bed.approaches) {
    // The approach helpers are on the seat itself: both ends are equally far.
    assert.deepEqual(approach.at, [0, 0, 0]);
    for (const degrees of [0, 37.3, 90, 181.7, 270.2]) {
      const facing = multiply(yaw(degrees), quaternion(approach.turn));
      const start = downStart(approach.down, seatAt, facing, seatAt, true);
      assert.ok(start, `${approach.name} has a known motion`);
      assert.ok(
        sameTurn(start.end, multiply(facing, yaw(180))),
        `${approach.name} at ${degrees} degrees ends opposite the seat's way`,
      );
    }
  }
});

test('without a tie the start nearer the approach helper still wins', () => {
  const bed = helpers.Bed_1Place_Low;
  const approach = bed.approaches[0];
  const seatAt = { x: 0, y: 0, z: 0 };
  const facing = multiply(yaw(0), quaternion(approach.turn));
  const stand = { x: approach.at[0], y: approach.at[1], z: 0 };
  const lying = downStart(approach.down, seatAt, facing, stand, true);
  const other = downStart(approach.down, seatAt, facing, stand, false);
  // The tie rule does not change a clear choice.
  assert.ok(sameTurn(lying.end, other.end));
  // A sit with a tie keeps the seat's own way.
  const bench = helpers.Sit_1Place_Bench_Low;
  const side = bench.approaches.find((candidate) => candidate.at.every((value) => value === 0));
  if (side) {
    const sitFacing = multiply(yaw(12), quaternion(side.turn));
    const sit = downStart(side.down, seatAt, sitFacing, seatAt, false);
    if (sit) {
      assert.ok(sameTurn(sit.end, sitFacing));
    }
  }
});
