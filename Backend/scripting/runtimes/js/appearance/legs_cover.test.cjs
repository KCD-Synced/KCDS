'use strict';

// a woman in a men's top gets hose; nobody else does.

const test = require('node:test');
const assert = require('node:assert/strict');

const { needsCover } = require('./legs_cover.cjs');

const row = (name, equipped = 1) => ({ name, equipped });

test("a woman in a men's top without leg cover needs hose", () => {
  assert.equal(needsCover('female', [row('TunicShort03_m01_D')]), true);
  assert.equal(needsCover('female', [row('TunicLong01_m01_C'), row('belt_2slot')]), true);
});

test('no cover when her legs are covered, she has no top on, or the body is a man', () => {
  assert.equal(needsCover('female', [row('TunicShort03_m01_D'), row('HoseJoined01_m01_C')]), false);
  assert.equal(needsCover('female', [row('F_SimpleDress01_m01_D')]), false);
  assert.equal(needsCover('female', [row('TunicShort03_m01_D', 0)]), false);
  assert.equal(needsCover('female', [row('belt_2slot')]), false);
  assert.equal(needsCover('male', [row('TunicShort03_m01_D')]), false);
  assert.equal(needsCover('female', [{ equipped: 1 }]), false);
});
