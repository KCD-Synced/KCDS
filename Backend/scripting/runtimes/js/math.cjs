'use strict';
function finite(value, fallback = 0) {
  const number = Number(value ?? fallback);
  if (!Number.isFinite(number)) throw new TypeError('Expected finite coordinate');
  return number;
}

class VectorBase {
  #keys;
  constructor(keys, values) {
    this.#keys = keys;
    this.set(...values);
  }
  set(...values) {
    const numbers = this.#keys.map((_, i) => finite(values[i]));
    this.#keys.forEach((key, i) => {
      this[key] = numbers[i];
    });
    return this;
  }
  add(other) {
    return this.set(...this.#keys.map((key) => this[key] + finite(other[key])));
  }
  sub(other) {
    return this.set(...this.#keys.map((key) => this[key] - finite(other[key])));
  }
  mul(scalar) {
    scalar = finite(scalar);
    return this.set(...this.#keys.map((key) => this[key] * scalar));
  }
  div(scalar) {
    scalar = finite(scalar);
    if (scalar === 0) throw new RangeError('Vector divisor must be non-zero');
    return this.mul(1 / scalar);
  }
  dot(other) {
    return this.#keys.reduce((sum, key) => sum + this[key] * finite(other[key]), 0);
  }
  get lengthSquared() {
    return this.dot(this);
  }
  get length() {
    return Math.hypot(...this.#keys.map((key) => this[key]));
  }
  normalize() {
    const magnitude = this.length;
    return magnitude === 0 ? this : this.div(magnitude);
  }
  lerp(target, t) {
    t = finite(t);
    return this.set(...this.#keys.map((key) => this[key] + (finite(target[key]) - this[key]) * t));
  }
  distance(other) {
    return Math.hypot(...this.#keys.map((key) => this[key] - finite(other[key])));
  }
  clone() {
    return new this.constructor(...this.#keys.map((key) => this[key]));
  }
  toString() {
    return `${this.constructor.name}(${this.#keys.map((key) => this[key]).join(', ')})`;
  }
  toJSON() {
    return Object.fromEntries(this.#keys.map((key) => [key, this[key]]));
  }
}
class Vector2 extends VectorBase {
  constructor(x = 0, y = 0) {
    super(['x', 'y'], [x, y]);
  }
  static zero() {
    return new Vector2(0, 0);
  }
  static one() {
    return new Vector2(1, 1);
  }
}
class Vector3 extends VectorBase {
  constructor(x = 0, y = 0, z = 0) {
    super(['x', 'y', 'z'], [x, y, z]);
  }
  cross(other) {
    const x = finite(other.x),
      y = finite(other.y),
      z = finite(other.z);
    return this.set(this.y * z - this.z * y, this.z * x - this.x * z, this.x * y - this.y * x);
  }
  static zero() {
    return new Vector3(0, 0, 0);
  }
  static one() {
    return new Vector3(1, 1, 1);
  }
  // The SDK's own axis convention; engine world itself is Z-up and faces +Y.
  static up() {
    return new Vector3(0, 1, 0);
  }
  static forward() {
    return new Vector3(0, 0, 1);
  }
  static right() {
    return new Vector3(1, 0, 0);
  }
}
class Vector4 extends VectorBase {
  constructor(x = 0, y = 0, z = 0, w = 0) {
    super(['x', 'y', 'z', 'w'], [x, y, z, w]);
  }
  static zero() {
    return new Vector4(0, 0, 0, 0);
  }
  static one() {
    return new Vector4(1, 1, 1, 1);
  }
}

class Quaternion {
  constructor(w = 1, x = 0, y = 0, z = 0) {
    this.set(w, x, y, z);
  }
  set(w, x, y, z) {
    const components = [finite(w, 1), finite(x), finite(y), finite(z)];
    [this.w, this.x, this.y, this.z] = components;
    return this;
  }
  clone() {
    return new Quaternion(this.w, this.x, this.y, this.z);
  }
  dot(q) {
    return this.w * q.w + this.x * q.x + this.y * q.y + this.z * q.z;
  }
  get lengthSquared() {
    return this.dot(this);
  }
  get length() {
    return Math.hypot(this.w, this.x, this.y, this.z);
  }
  normalize() {
    const magnitude = this.length;
    return magnitude === 0
      ? this.set(1, 0, 0, 0)
      : this.set(this.w / magnitude, this.x / magnitude, this.y / magnitude, this.z / magnitude);
  }
  mul(q) {
    const { w, x, y, z } = this;
    return this.set(
      w * q.w - x * q.x - y * q.y - z * q.z,
      w * q.x + x * q.w + y * q.z - z * q.y,
      w * q.y - x * q.z + y * q.w + z * q.x,
      w * q.z + x * q.y - y * q.x + z * q.w,
    );
  }
  conjugate() {
    return new Quaternion(this.w, -this.x, -this.y, -this.z);
  }
  inverse() {
    const norm = this.lengthSquared;
    if (norm === 0) throw new RangeError('Zero quaternion has no inverse');
    return new Quaternion(this.w / norm, -this.x / norm, -this.y / norm, -this.z / norm);
  }
  slerp(target, t) {
    t = finite(t);
    let cosine = this.dot(target),
      sign = 1;
    if (cosine < 0) {
      cosine = -cosine;
      sign = -1;
    }
    let a = 1 - t,
      b = t;
    if (cosine < 1 - Number.EPSILON) {
      const angle = Math.acos(Math.max(-1, Math.min(1, cosine))),
        divisor = Math.sin(angle);
      a = Math.sin((1 - t) * angle) / divisor;
      b = Math.sin(t * angle) / divisor;
    }
    return this.set(
      a * this.w + b * sign * target.w,
      a * this.x + b * sign * target.x,
      a * this.y + b * sign * target.y,
      a * this.z + b * sign * target.z,
    );
  }
  rotateVector(v) {
    // Quaternion-vector rotation for unit rotations: v + 2w(u x v) + 2(u x (u x v)).
    const u = new Vector3(this.x, this.y, this.z),
      input = vector(v),
      uv = u.clone().cross(input);
    return input.add(uv.clone().mul(2 * this.w)).add(u.cross(uv).mul(2));
  }
  toEuler() {
    const { w, x, y, z } = this;
    const pitchNumerator = 2 * (y * z + w * x),
      pitchDenominator = w * w - x * x - y * y + z * z;
    const pitch =
      Math.abs(pitchNumerator) < Number.EPSILON && Math.abs(pitchDenominator) < Number.EPSILON
        ? 2 * Math.atan2(x, w)
        : Math.atan2(pitchNumerator, pitchDenominator);
    const yaw = Math.asin(Math.max(-1, Math.min(1, -2 * (x * z - w * y))));
    const roll = Math.atan2(2 * (x * y + w * z), w * w + x * x - y * y - z * z);
    return new Vector3(pitch, yaw, roll);
  }
  toString() {
    return `Quaternion(${this.w}, ${this.x}, ${this.y}, ${this.z})`;
  }
  toJSON() {
    return { w: this.w, x: this.x, y: this.y, z: this.z };
  }
  static identity() {
    return new Quaternion(1, 0, 0, 0);
  }
  static fromEuler(pitch, yaw, roll) {
    const x = finite(pitch) / 2,
      y = finite(yaw) / 2,
      z = finite(roll) / 2;
    const cx = Math.cos(x),
      sx = Math.sin(x),
      cy = Math.cos(y),
      sy = Math.sin(y),
      cz = Math.cos(z),
      sz = Math.sin(z);
    return new Quaternion(
      cx * cy * cz + sx * sy * sz,
      sx * cy * cz - cx * sy * sz,
      cx * sy * cz + sx * cy * sz,
      cx * cy * sz - sx * sy * cz,
    );
  }
  static fromAxisAngle(axis, angle) {
    const unit = vector(axis).normalize(),
      half = finite(angle) / 2,
      sine = Math.sin(half);
    return new Quaternion(Math.cos(half), unit.x * sine, unit.y * sine, unit.z * sine);
  }
}

class Color {
  constructor(r, g, b, a = 1) {
    this.set(r, g, b, a);
  }
  set(r, g, b, a = 1) {
    [this.r, this.g, this.b, this.a] = [finite(r), finite(g), finite(b), finite(a, 1)];
    return this;
  }
  lerp(target, t) {
    t = finite(t);
    return this.set(
      this.r + (target.r - this.r) * t,
      this.g + (target.g - this.g) * t,
      this.b + (target.b - this.b) * t,
      this.a + (target.a - this.a) * t,
    );
  }
  clone() {
    return new Color(this.r, this.g, this.b, this.a);
  }
  toHex(includeAlpha = false) {
    const components = includeAlpha ? [this.r, this.g, this.b, this.a] : [this.r, this.g, this.b];
    return (
      '#' +
      components
        .map((value) =>
          Math.round(Math.max(0, Math.min(1, value)) * 255)
            .toString(16)
            .padStart(2, '0'),
        )
        .join('')
    );
  }
  toString() {
    return `Color(${this.r}, ${this.g}, ${this.b}, ${this.a})`;
  }
  toJSON() {
    return { r: this.r, g: this.g, b: this.b, a: this.a };
  }
  static fromHex(hex) {
    if (typeof hex !== 'string' || !/^#?(?:[\da-f]{6}|[\da-f]{8})$/i.test(hex))
      return Color.white();
    hex = hex.replace(/^#/, '');
    return Color.fromRGB(
      ...[0, 2, 4].map((index) => parseInt(hex.slice(index, index + 2), 16)),
      hex.length === 8 ? parseInt(hex.slice(6), 16) : 255,
    );
  }
  static fromRGB(r, g, b, a = 255) {
    return new Color(finite(r) / 255, finite(g) / 255, finite(b) / 255, finite(a, 255) / 255);
  }
  static white() {
    return new Color(1, 1, 1, 1);
  }
  static black() {
    return new Color(0, 0, 0, 1);
  }
  static red() {
    return new Color(1, 0, 0, 1);
  }
  static green() {
    return new Color(0, 1, 0, 1);
  }
  static blue() {
    return new Color(0, 0, 1, 1);
  }
  static yellow() {
    return new Color(1, 1, 0, 1);
  }
  static cyan() {
    return new Color(0, 1, 1, 1);
  }
  static magenta() {
    return new Color(1, 0, 1, 1);
  }
  static transparent() {
    return new Color(0, 0, 0, 0);
  }
}

function vector(value = {}) {
  return new Vector3(value.x, value.y, value.z);
}
function rotation(value = {}) {
  if ('w' in value) return new Quaternion(value.w, value.x, value.y, value.z);
  // Engine spawn/teleport Euler input is in degrees; quaternion API uses radians.
  return Quaternion.fromEuler(
    (finite(value.x) * Math.PI) / 180,
    (finite(value.y) * Math.PI) / 180,
    (finite(value.z) * Math.PI) / 180,
  );
}
module.exports = { Vector2, Vector3, Vector4, Quaternion, Color, vector, rotation };
