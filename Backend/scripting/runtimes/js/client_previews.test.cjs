'use strict';

// The client PropGhost and PropPlacer (Client/resources/previews.cjs): what a client
// script's calls send the game client, and how a session's reports reach its handlers.
const test = require('node:test');
const assert = require('node:assert/strict');
const { Vector3, Quaternion } = require('./math.cjs');
const { ResourcePreviews } = require('../../../../Client/resources/previews.cjs');

const TENT = 'objects/manmade/structures/living/tents/canopy_forest_a.cgf';

function setup() {
  const sent = [];
  const previews = new ResourcePreviews({ send: (m) => sent.push(m), Vector3, Quaternion });
  return { previews, sent, ...previews.facade('camp') };
}

test('a ghost is created from the prop catalog and every change goes down', () => {
  const { previews, sent, PropGhost } = setup();
  const ghost = PropGhost.create({ model: TENT, position: { x: 1, y: 2, z: 3 } });
  assert.equal(ghost.valid, true);
  assert.equal(sent[0].op, 'localGhost');
  assert.equal(sent[0].kind, 'create');
  assert.equal(sent[0].ghost, ghost.id);
  assert.deepEqual(sent[0].models, [TENT]);
  assert.equal(ghost.setPose({ x: 5, y: 5, z: 5 }), true);
  assert.equal(sent.at(-1).kind, 'pose');
  assert.equal(ghost.setTint({ r: 2, g: 0, b: 0, a: 0.5 }), true);
  assert.deepEqual(sent.at(-1).tint, { r: 1, g: 0, b: 0, a: 0.5 });
  // The box the game reports for the ghost (a frame after it stands), none again when hidden.
  assert.equal(ghost.getBounds(), null);
  previews.receiveBounds({
    ghost: ghost.id,
    min: { x: 4, y: 4, z: 5 },
    max: { x: 6, y: 7, z: 7.5 },
  });
  const box = ghost.getBounds();
  assert.deepEqual(box, { min: { x: 4, y: 4, z: 5 }, max: { x: 6, y: 7, z: 7.5 } });
  box.min.x = 100;
  assert.equal(ghost.getBounds().min.x, 4);
  assert.equal(ghost.setHidden(true), true);
  previews.receiveBounds({ ghost: ghost.id });
  assert.equal(ghost.getBounds(), null);
  assert.equal(ghost.destroy(), true);
  assert.equal(ghost.getBounds(), null);
  assert.equal(ghost.valid, false);
  assert.equal(ghost.destroy(), false);
  assert.equal(ghost.setPose({ x: 0, y: 0, z: 0 }), false);
});

// a ghost scaled per axis.
test('a ghost takes its scale per axis as a Vector3', () => {
  const { sent, PropGhost } = setup();
  const ghost = PropGhost.create({
    model: TENT,
    position: { x: 1, y: 2, z: 3 },
    scale: { x: 1.9, y: 1.5, z: 0.7 },
  });
  assert.equal(sent[0].scale, 1);
  assert.deepEqual(sent[0].scaleAxes, { x: 1.9, y: 1.5, z: 0.7 });
  ghost.setPose({ x: 1, y: 2, z: 3 }, undefined, 2);
  assert.equal(sent.at(-1).scale, 2);
  assert.equal(sent.at(-1).scaleAxes, undefined);
  assert.throws(
    () => PropGhost.create({ model: TENT, position: { x: 0, y: 0, z: 0 }, scale: 'big' }),
    TypeError,
  );
});

test('a ghost refuses what the server previews refuse', () => {
  const { sent, PropGhost } = setup();
  assert.throws(
    () => PropGhost.create({ model: 'objects/not/a/mesh.cgf', position: { x: 0, y: 0, z: 0 } }),
    /does not carry/,
  );
  assert.throws(() => PropGhost.create({ model: TENT }), RangeError);
  assert.deepEqual(sent, []);
});

test('a placer session runs once, reports to its handlers and ends', () => {
  const { previews, sent, PropPlacer } = setup();
  assert.equal(PropPlacer.begin({ model: TENT, rules: { maxSlope: 20 } }), true);
  assert.equal(PropPlacer.isActive(), true);
  assert.equal(PropPlacer.begin({ model: TENT }), false, 'one session at a time');
  const begin = sent.find((m) => m.op === 'localPlacer' && m.kind === 'begin');
  assert.equal(begin.maxSlope, 20);
  assert.equal(begin.minDistance, -1);
  assert.equal(begin.keepOpen, true);
  const heard = [];
  for (const event of ['move', 'confirm', 'rejected', 'cancel', 'end']) {
    PropPlacer.on(event, (pose, valid) => heard.push([event, pose?.position?.x ?? null, valid]));
  }
  const pose = {
    position: { x: 4, y: 5, z: 6 },
    rotation: { x: 0, y: 0, z: 0, w: 1 },
    normal: { x: 0, y: 0, z: 1 },
    distance: 7,
    onSurface: true,
  };
  previews.receiveReport({ session: begin.session, event: 'move', valid: true, pose });
  assert.equal(PropPlacer.getPose().position.x, 4);
  assert.equal(PropPlacer.getPose().surface, '');
  previews.receiveReport({ session: begin.session, event: 'rejected', valid: false, pose });
  previews.receiveReport({ session: begin.session + 1, event: 'confirm', valid: true, pose });
  assert.equal(PropPlacer.setRefusal(true), true);
  assert.equal(PropPlacer.rotate(45), true);
  assert.deepEqual(sent.at(-1), {
    op: 'localPlacer',
    session: begin.session,
    kind: 'rotate',
    degrees: 45,
  });
  PropPlacer.end();
  assert.equal(PropPlacer.isActive(), false);
  assert.deepEqual(heard, [
    ['move', 4, true],
    ['rejected', 4, false],
    ['end', null, false],
  ]);
  assert.equal(PropPlacer.setRefusal(false), false, 'no session');
});

test('a stopped resource takes its session and ghosts with it', () => {
  const { previews, sent, PropGhost, PropPlacer } = setup();
  PropGhost.create({ model: TENT, position: { x: 0, y: 0, z: 0 } });
  PropPlacer.begin({ model: TENT });
  previews.removeOwner('camp');
  assert.equal(PropPlacer.isActive(), false);
  assert.ok(sent.some((m) => m.op === 'localGhost' && m.kind === 'destroy'));
  assert.ok(sent.some((m) => m.op === 'localPlacer' && m.kind === 'end'));
});

// World Architect's previews are drawn see-through; a script's ghost cannot ask for it.
test("the editor's preview is see-through and a script's ghost is not", () => {
  const { sent, PropGhost } = setup();
  PropGhost.editorPreview({ models: [TENT], position: { x: 1, y: 2, z: 3 } });
  assert.equal(sent.at(-1).seeThrough, true);
  PropGhost.create({ model: TENT, position: { x: 1, y: 2, z: 3 }, seeThrough: true });
  assert.equal(sent.at(-1).seeThrough, false);
});
