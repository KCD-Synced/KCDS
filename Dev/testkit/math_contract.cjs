'use strict';

// Runs unchanged as a downloaded CommonJS resource in Node and the browser.
// Assertions use geometric results, value identity and documented defaults.
module.exports = function verifyMath({ Vector2, Vector3, Vector4, Quaternion, Color }) {
  let checks = 0;
  const check = (condition, message) => {
    if (!condition) throw new Error('Math contract: ' + message);
    ++checks;
  };
  const close = (actual, expected) => check(Math.abs(actual - expected) < 1e-10, 'numeric result');
  const components = (actual, expected) => {
    for (const [key, value] of Object.entries(expected)) close(actual[key], value);
  };
  for (const [Class, coordinates] of [
    [Vector2, [3, 4]],
    [Vector3, [2, 3, 6]],
    [Vector4, [1, 2, 2, 4]],
  ]) {
    const vector = new Class(...coordinates);
    const original = vector.toJSON();
    const copy = vector.clone();
    check(copy !== vector && copy instanceof Class, 'independent typed clone');
    close(vector.length, Math.hypot(...coordinates));
    close(
      vector.lengthSquared,
      coordinates.reduce((sum, value) => sum + value * value, 0),
    );
    close(
      vector.dot(Class.one()),
      coordinates.reduce((sum, value) => sum + value, 0),
    );
    close(vector.distance(Class.zero()), vector.length);
    check(copy.add(Class.one()).sub(Class.one()).mul(2).div(2) === copy, 'mutable chaining');
    components(copy, original);
    check(copy.set(...coordinates) === copy, 'set returns receiver');
    check(copy.lerp(Class.zero(), 0.5) === copy, 'lerp returns receiver');
    components(
      copy,
      Object.fromEntries(Object.entries(original).map(([key, value]) => [key, value / 2])),
    );
    check(copy.normalize() === copy, 'normalize returns receiver');
    close(copy.length, 1);
    check(Class.zero().normalize().length === 0, 'zero normalization');
    check(Class.one() !== Class.one(), 'independent constants');
    check(vector.toString() === `${Class.name}(${coordinates.join(', ')})`, 'vector formatting');
    components(JSON.parse(JSON.stringify(vector)), original);
    vector.x = 9;
    close(vector.x, 9);
    let rejected = false;
    try {
      vector.length = 100;
    } catch (error) {
      rejected = error instanceof TypeError;
    }
    check(rejected, 'readonly length');
  }
  const cross = new Vector3(1, 2, 3).cross(new Vector3(4, 5, 6));
  components(cross, { x: -3, y: 6, z: -3 });
  components(Vector3.up(), { x: 0, y: 1, z: 0 });
  components(Vector3.forward(), { x: 0, y: 0, z: 1 });
  components(Vector3.right(), { x: 1, y: 0, z: 0 });
  const q = Quaternion.fromAxisAngle(new Vector3(0, 0, 4), Math.PI / 2);
  const input = new Vector3(0, 1, 0);
  components(q.rotateVector(input), { x: -1, y: 0, z: 0 });
  components(input, { x: 0, y: 1, z: 0 });
  close(q.length, 1);
  close(q.lengthSquared, 1);
  close(q.dot(q), 1);
  components(q.clone().mul(q.inverse()), { w: 1, x: 0, y: 0, z: 0 });
  check(q.conjugate() !== q, 'conjugate copy');
  close(q.conjugate().z, -q.z);
  const halfway = Quaternion.identity();
  check(halfway.slerp(q, 0.5) === halfway, 'slerp returns receiver');
  close(halfway.toEuler().z, Math.PI / 4);
  components(new Quaternion(0, 0, 0, 0).normalize(), { w: 1, x: 0, y: 0, z: 0 });
  const replacement = Quaternion.identity();
  check(replacement.set(1, 2, 3, 4) === replacement, 'quaternion set');
  check(replacement.toString() === 'Quaternion(1, 2, 3, 4)', 'quaternion formatting');
  components(JSON.parse(JSON.stringify(replacement)), { w: 1, x: 2, y: 3, z: 4 });
  for (const angles of [
    [0.2, -0.3, 0.7],
    [0, Math.PI / 2, 0],
    [0, -Math.PI / 2, 0],
  ]) {
    const rotation = Quaternion.fromEuler(...angles);
    const euler = rotation.toEuler();
    const restored = Quaternion.fromEuler(euler.x, euler.y, euler.z);
    components(restored.rotateVector(input), rotation.rotateVector(input).toJSON());
  }
  for (const [name, expected] of Object.entries({
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
    check(Color[name]().toHex() === expected, 'color preset');
    check(Color[name]() !== Color[name](), 'independent color preset');
  }
  const color = Color.fromRGB(20, 100, 220, 128);
  check(color.toHex(true) === '#1464dc80', 'RGBA bytes');
  check(Color.fromHex('1464DC80').toHex(true) === '#1464dc80', 'hex parsing');
  check(Color.fromHex('#123').toHex(true) === '#ffffffff', 'invalid shorthand fallback');
  check(new Color(1, 0, 0).a === 1, 'alpha default');
  const colorCopy = color.clone();
  check(colorCopy !== color && colorCopy instanceof Color, 'color clone');
  check(colorCopy.lerp(Color.black(), 0.5) === colorCopy, 'color lerp');
  close(colorCopy.r, 10 / 255);
  check(colorCopy.set(0, 0, 0) === colorCopy && colorCopy.a === 1, 'color set default');
  check(Color.red().toString() === 'Color(1, 0, 0, 1)', 'color formatting');
  components(JSON.parse(JSON.stringify(color)), {
    r: 20 / 255,
    g: 100 / 255,
    b: 220 / 255,
    a: 128 / 255,
  });
  return checks;
};
