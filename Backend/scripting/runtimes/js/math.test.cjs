'use strict';
const test = require('node:test'),
  assert = require('node:assert/strict');
const fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path');
const { Vector2, Vector3, Vector4, Quaternion, Color, rotation } = require('./math.cjs');
const { encode, decode } = require('./codec.cjs');
const { ResourceHost } = require('./host.cjs');
const close = (a, b, tolerance = 1e-12) => assert.ok(Math.abs(a - b) <= tolerance, `${a} != ${b}`);
function components(actual, expected) {
  for (const [key, value] of Object.entries(expected)) close(actual[key], value);
}

for (const [Class, coordinates] of [
  [Vector2, [3, 4]],
  [Vector3, [2, 3, 6]],
  [Vector4, [1, 2, 2, 4]],
]) {
  test(`${Class.name}: geometry, mutable chaining, independent copies and JSON/wire values`, () => {
    const value = new Class(...coordinates),
      original = value.toJSON(),
      ones = Class.one();
    close(value.length, Math.hypot(...coordinates));
    close(
      value.lengthSquared,
      coordinates.reduce((sum, x) => sum + x * x, 0),
    );
    close(
      value.dot(ones),
      coordinates.reduce((a, b) => a + b),
    );
    close(value.distance(Class.zero()), value.length);
    assert.deepEqual(value.toJSON(), original); // geometry queries do not mutate
    const clone = value.clone();
    assert.notEqual(clone, value);
    assert.ok(clone instanceof Class);
    assert.equal(clone.add(ones).sub(ones).mul(2).div(2), clone);
    assert.deepEqual(clone.toJSON(), original);
    assert.equal(clone.lerp(Class.zero(), 0.5), clone);
    components(clone, Object.fromEntries(Object.entries(original).map(([key, v]) => [key, v / 2])));
    assert.equal(clone.normalize(), clone);
    close(clone.length, 1);
    const zero = Class.zero();
    assert.equal(zero.normalize(), zero);
    assert.equal(zero.length, 0);
    assert.throws(() => value.div(0), /non-zero/);
    assert.deepEqual(value.toJSON(), original);
    assert.throws(() => {
      value.length = 4;
    }, TypeError);
    assert.equal(value.toString(), `${Class.name}(${coordinates.join(', ')})`);
    assert.deepEqual(JSON.parse(JSON.stringify(value)), original);
    assert.deepEqual(JSON.parse(JSON.stringify(decode(encode(value)))), original);
    assert.notEqual(Class.one(), Class.one());
  });
}

test('cross product is perpendicular, self-alias safe and public axes stay distinct from game axes', () => {
  const a = new Vector3(1, 2, 3),
    b = new Vector3(4, 5, 6),
    cross = a.clone().cross(b);
  components(cross, { x: -3, y: 6, z: -3 });
  close(cross.dot(a), 0);
  close(cross.dot(b), 0);
  assert.equal(a.cross(a), a);
  assert.deepEqual(a.toJSON(), { x: 0, y: 0, z: 0 });
  assert.deepEqual(Vector3.up().toJSON(), { x: 0, y: 1, z: 0 });
  assert.deepEqual(Vector3.forward().toJSON(), { x: 0, y: 0, z: 1 });
  assert.deepEqual(Vector3.right().toJSON(), { x: 1, y: 0, z: 0 });
});

test('quaternion rotates game +Y facing about Z, distinguishes degrees from radians and does not mutate inputs', () => {
  const q = Quaternion.fromAxisAngle(new Vector3(0, 0, 20), Math.PI / 2),
    v = new Vector3(0, 1, 0);
  const qBefore = q.toJSON(),
    vBefore = v.toJSON();
  components(q.rotateVector(v), { x: -1, y: 0, z: 0 });
  assert.deepEqual(q.toJSON(), qBefore);
  assert.deepEqual(v.toJSON(), vBefore);
  components(rotation(new Vector3(0, 0, 90)).rotateVector(v), { x: -1, y: 0, z: 0 });
  close(q.toEuler().z, Math.PI / 2);
  components(Quaternion.fromEuler(0, Math.PI / 2, 0).rotateVector(new Vector3(0, 0, 1)), {
    x: 1,
    y: 0,
    z: 0,
  });
  assert.deepEqual(rotation({ w: 1, x: 0, y: 0, z: 0 }).toJSON(), Quaternion.identity().toJSON());
});

test('composition is noncommutative, alias safe; inverse and conjugate are independent values', () => {
  const z = Quaternion.fromAxisAngle(new Vector3(0, 0, 1), Math.PI / 2);
  const x = Quaternion.fromAxisAngle(new Vector3(1, 0, 0), Math.PI / 2);
  components(z.clone().mul(x).rotateVector(new Vector3(0, 1, 0)), { x: 0, y: 0, z: 1 });
  components(x.clone().mul(z).rotateVector(new Vector3(0, 1, 0)), { x: -1, y: 0, z: 0 });
  const squared = z.clone();
  assert.equal(squared.mul(squared), squared);
  components(squared.rotateVector(new Vector3(0, 1, 0)), { x: 0, y: -1, z: 0 });
  const q = new Quaternion(2, 3, 4, 5),
    before = q.toJSON();
  components(q.clone().mul(q.inverse()), { w: 1, x: 0, y: 0, z: 0 });
  assert.notEqual(q.conjugate(), q);
  assert.deepEqual(q.toJSON(), before);
  assert.equal(new Quaternion(0, 0, 0, 0).normalize().w, 1);
  close(q.clone().normalize().length, 1);
  assert.equal(q.lengthSquared, 54);
});

test('slerp uses the shortest arc including equivalent opposite quaternions and endpoint rotations', () => {
  const q = Quaternion.fromAxisAngle(new Vector3(0, 0, 1), Math.PI * 0.9);
  const before = q.toJSON(),
    middle = Quaternion.identity();
  assert.equal(middle.slerp(q, 0.5), middle);
  close(middle.toEuler().z, Math.PI * 0.45);
  const opposite = new Quaternion(-q.w, -q.x, -q.y, -q.z);
  const same = q.clone().slerp(opposite, 0.5);
  close(same.length, 1);
  components(same, before);
  components(Quaternion.identity().slerp(q, 1), before);
  components(Quaternion.identity().slerp(q, 0), Quaternion.identity().toJSON());
  assert.deepEqual(q.toJSON(), before);
});

test('Euler/quaternion round trips reproduce rotations including gimbal lock, without NaN', () => {
  for (const angles of [
    [0, 0, 0],
    [0.2, -0.3, 0.7],
    [-1.2, 0.8, -2],
    [0, Math.PI / 2, 0],
    [0, -Math.PI / 2, 0],
  ]) {
    const q = Quaternion.fromEuler(...angles),
      e = q.toEuler(),
      reconstructed = Quaternion.fromEuler(e.x, e.y, e.z);
    for (const v of [Vector3.right(), Vector3.up(), Vector3.forward()])
      components(reconstructed.rotateVector(v), q.rotateVector(v).toJSON());
  }
  const q = new Quaternion(1, 2, 3, 4);
  assert.equal(q.toString(), 'Quaternion(1, 2, 3, 4)');
  assert.deepEqual(JSON.parse(JSON.stringify(q)), { w: 1, x: 2, y: 3, z: 4 });
});

test('color byte/hex contract, invalid shorthand fallback, alpha defaults and independent presets', () => {
  for (const [name, hex] of Object.entries({
    white: '#ffffff',
    black: '#000000',
    red: '#ff0000',
    green: '#00ff00',
    blue: '#0000ff',
    yellow: '#ffff00',
    cyan: '#00ffff',
    magenta: '#ff00ff',
    transparent: '#000000',
  })) {
    assert.equal(Color[name]().toHex(), hex);
    assert.notEqual(Color[name](), Color[name]());
  }
  for (const text of ['#123', '#abcd', 'x', '#ff0000oops', ' ff0000', null])
    assert.equal(Color.fromHex(text).toHex(true), '#ffffffff');
  const c = Color.fromRGB(20, 100, 220, 128);
  assert.equal(c.toHex(true), '#1464dc80');
  assert.equal(Color.fromHex('1464DC80').toHex(true), '#1464dc80');
  assert.equal(Color.fromHex('#1464dc').a, 1);
  const copy = c.clone();
  assert.equal(copy.lerp(Color.black(), 0.5), copy);
  close(copy.r, 10 / 255);
  assert.equal(copy.set(0, 0, 0), copy);
  assert.equal(copy.a, 1);
  assert.equal(Color.transparent().toHex(true), '#00000000');
  assert.deepEqual(JSON.parse(JSON.stringify(c)), {
    r: 20 / 255,
    g: 100 / 255,
    b: 220 / 255,
    a: 128 / 255,
  });
  assert.equal(Color.red().toString(), 'Color(1, 0, 0, 1)');
});

test('all five math globals execute from a production CommonJS resource; existing horse positioning stays valid', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-math-resource-')),
    logs = [];
  fs.mkdirSync(path.join(root, 'math'));
  fs.writeFileSync(
    path.join(root, 'math/package.json'),
    JSON.stringify({ name: 'math', kcds: { serverScripts: ['index.js'] } }),
  );
  fs.writeFileSync(
    path.join(root, 'math/index.js'),
    `console.log(JSON.stringify({v2:kcds.math.Vector2.one(),v3:kcds.math.Vector3.right(),v4:kcds.math.Vector4.one(),q:kcds.math.Quaternion.identity(),color:kcds.math.Color.red()}));`,
  );
  const host = new ResourceHost(root, { send() {}, log: (_, text) => logs.push(text) });
  try {
    await host.startAll();
    assert.deepEqual(JSON.parse(logs[0]), {
      v2: { x: 1, y: 1 },
      v3: { x: 1, y: 0, z: 0 },
      v4: { x: 1, y: 1, z: 1, w: 1 },
      q: { w: 1, x: 0, y: 0, z: 0 },
      color: { r: 1, g: 0, b: 0, a: 1 },
    });
  } finally {
    await host.stop('math');
    fs.rmSync(root, { recursive: true, force: true });
  }
});
