'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { ResourceGizmos } = require('../../../../Client/resources/gizmos.cjs');

function fixture() {
  const sent = [];
  let now = 1000;
  const gizmos = new ResourceGizmos({ send: (message) => sent.push(message), clock: () => now });
  return {
    gizmos,
    sent,
    advance(ms) {
      now += ms;
    },
  };
}

test('client Gizmos.set: a line goes down with the style defaults', () => {
  const { gizmos, sent } = fixture();
  const api = gizmos.facade('demo');
  api.set('path', { type: 'line', from: { x: 1, y: 2, z: 3 }, to: { x: 4, y: 5, z: 6 } });
  assert.deepEqual(sent, [
    {
      op: 'gizmoSet',
      owner: 'demo',
      id: 'path',
      points: [1, 2, 3, 4, 5, 6],
      lines: [0, 1],
      triangles: [],
      color: 0xff6ee678,
      fill: 0,
      width: 1.5,
      depthTest: true,
      durationMs: 0,
    },
  ]);
});

test('client Gizmos: every primitive builds lines (and faces when filled)', () => {
  const { gizmos, sent } = fixture();
  const api = gizmos.facade('demo');
  const origin = { x: 0, y: 0, z: 0 };
  const up = { x: 0, y: 0, z: 2 };
  const fill = { fillColor: 0x80ff0000 };
  api.set('box', { type: 'box' }, fill);
  api.set('sphere', { type: 'sphere', radius: 1 }, fill);
  api.set('shape', {
    type: 'shape',
    points: [origin, { x: 1, y: 0, z: 0 }, { x: 0, y: 1, z: 0.5 }],
    height: 2,
  });
  api.set('arrow', { type: 'arrow', from: origin, to: up }, fill);
  api.set('capsule', { type: 'capsule', from: origin, to: up, radius: 0.5 }, fill);
  api.set('cone', { type: 'cone', from: origin, to: up, radius: 0.5, segments: 8 });
  api.set('cylinder', { type: 'cylinder', from: origin, to: up, radius: 0.5 }, fill);
  api.set('circle', { type: 'circle', position: origin, radius: 3 }, fill);
  api.set('grid', { type: 'grid', position: origin, divisions: 4 });
  api.set('axes', { type: 'axes', position: origin, size: 2 });
  api.set('polyline', {
    type: 'polyline',
    points: [origin, up, { x: 1, y: 1, z: 1 }],
    closed: true,
  });
  api.set(
    'mesh',
    { type: 'mesh', points: [origin, up, { x: 1, y: 0, z: 0 }], indices: [0, 1, 2] },
    fill,
  );
  const byId = Object.fromEntries(sent.map((message) => [message.id, message]));
  assert.equal(byId.box.lines.length / 2, 12);
  assert.equal(byId.box.triangles.length / 3, 12);
  assert.equal(byId.sphere.lines.length / 2, 72);
  assert.ok(byId.sphere.triangles.length > 0);
  // A ring below, a ring 2 m above the lowest point, three thin vertical edges.
  assert.equal(byId.shape.lines.length / 2, 9);
  assert.deepEqual(byId.shape.thinLines, [4, 6, 8]);
  assert.deepEqual(byId.shape.points.slice(11, 12), [2]);
  assert.equal(byId.cone.lines.length / 2, 12);
  assert.equal(byId.cone.triangles.length, 0);
  assert.equal(byId.circle.triangles.length / 3, 24);
  assert.equal(byId.grid.lines.length / 2, 10);
  assert.deepEqual(byId.axes.lineColors, [0xffff0000, 0xff00ff00, 0xff0000ff]);
  assert.deepEqual(byId.axes.points.slice(3, 6), [2, 0, 0]);
  assert.equal(byId.polyline.lines.length / 2, 3);
  assert.deepEqual(byId.mesh.lines, [0, 1, 1, 2, 2, 0]);
  assert.deepEqual(byId.mesh.triangles, [0, 1, 2]);
  for (const message of Object.values(byId)) {
    const count = message.points.length / 3;
    assert.ok(
      [...message.lines, ...message.triangles].every((index) => index >= 0 && index < count),
      message.id,
    );
  }
});

test('client Gizmos: invalid input throws and keeps the previous drawing', () => {
  const { gizmos, sent } = fixture();
  const api = gizmos.facade('demo');
  const line = { type: 'line', from: { x: 0, y: 0, z: 0 }, to: { x: 1, y: 0, z: 0 } };
  api.set('a', line);
  assert.throws(() => api.set('a', line, { fillColor: 0xffffffff }), /fillColor/);
  assert.throws(() => api.set('a', { type: 'line', from: { x: 0, y: 0, z: 20000 }, to: line.to }));
  assert.throws(() => api.set('a', line, { width: 9 }), /width/);
  assert.throws(() => api.set('a', line, { durationMs: 1.5 }), /durationMs/);
  assert.throws(() => api.set('a', line, { color: -1 }), /color/);
  assert.throws(() => api.set('', line), /ID/);
  assert.throws(() => api.set('x'.repeat(129), line), /ID/);
  assert.throws(() => api.set('a', { type: 'arrow', from: line.from, to: line.to, headLength: 2 }));
  assert.throws(() => api.set('a', { type: 'cone', from: line.from, to: line.from, radius: 1 }));
  assert.throws(() =>
    api.set('a', { type: 'mesh', points: [line.from, line.to, line.to], indices: [0, 1, 3] }),
  );
  assert.throws(() =>
    api.set('a', { type: 'box', min: { x: 1, y: 0, z: 0 }, max: { x: 0, y: 1, z: 1 } }),
  );
  assert.throws(() => api.set('a', { type: 'teapot' }), /unknown/);
  assert.equal(sent.length, 1);
  assert.equal(api.setTransform('a', { x: 1, y: 1, z: 1 }), true);
});

test('client Gizmos: 128 IDs per resource, 16384 units across resources', () => {
  const { gizmos } = fixture();
  const api = gizmos.facade('demo');
  const line = { type: 'line', from: { x: 0, y: 0, z: 0 }, to: { x: 1, y: 0, z: 0 } };
  for (let i = 0; i < 128; i += 1) {
    api.set(`l${i}`, line);
  }
  assert.throws(() => api.set('one more', line), /128/);
  api.set('l0', line);
  // A 64-division grid is 130 lines; mesh of 1024 triangles with edges is 3072 + 3072.
  const points = [];
  for (let i = 0; i < 1024; i += 1) {
    points.push({ x: i, y: 0, z: 0 });
  }
  const indices = [];
  for (let i = 0; i < 1024; i += 1) {
    indices.push(i, (i + 1) % 1024, (i + 2) % 1024);
  }
  const other = gizmos.facade('other');
  other.set('m1', { type: 'mesh', points, indices }, { fillColor: 0xffffffff });
  other.set('m2', { type: 'mesh', points, indices }, { fillColor: 0xffffffff });
  assert.throws(
    () => other.set('m3', { type: 'mesh', points, indices }, { fillColor: 0xffffffff }),
    /16384/,
  );
});

test('client Gizmos: transform, remove, clear, expiry and a stopped resource', () => {
  const { gizmos, sent, advance } = fixture();
  const api = gizmos.facade('demo');
  const other = gizmos.facade('other');
  const circle = { type: 'circle', position: { x: 0, y: 0, z: 0 }, radius: 1 };
  api.set('c', circle);
  other.set('c', circle);
  assert.equal(api.setTransform('c', { x: 1, y: 2, z: 3 }, { x: 0, y: 0, z: 2, w: 0 }), true);
  assert.deepEqual(sent.at(-1), {
    op: 'gizmoTransform',
    owner: 'demo',
    id: 'c',
    position: { x: 1, y: 2, z: 3 },
    rotation: { x: 0, y: 0, z: 1, w: 0 },
  });
  assert.equal(api.setTransform('missing', { x: 0, y: 0, z: 0 }), false);
  assert.equal(api.remove('c'), true);
  assert.equal(api.remove('c'), false);
  assert.deepEqual(sent.at(-1), { op: 'gizmoRemove', owner: 'demo', id: 'c' });
  api.set('t', circle, { durationMs: 500 });
  advance(499);
  assert.equal(api.setTransform('t', { x: 0, y: 0, z: 0 }), true);
  advance(1);
  assert.equal(api.setTransform('t', { x: 0, y: 0, z: 0 }), false);
  api.set('d', circle);
  api.clear();
  assert.deepEqual(sent.at(-1), { op: 'gizmoRemoveOwner', owner: 'demo' });
  assert.equal(other.remove('c'), true);
  other.set('e', circle);
  gizmos.removeOwner('other');
  assert.deepEqual(sent.at(-1), { op: 'gizmoRemoveOwner', owner: 'other' });
  other.set('f', circle);
  gizmos.levelChanged();
  assert.equal(other.remove('f'), false);
});
