'use strict';

// The client-side Camera: a client script films this machine's view itself, as the
// server's Camera.startShot does for one player (Backend/scripting/runtimes/js/camera/shots.cjs,
// the same shot and its checks). The game client plays it as it plays the server's
// (Client/runtime/camera/camera_shot.hpp): { op: 'cameraShotLocal', position, rotation | lookAt
// and roll, fov, seconds } and { op: 'cameraClearLocal' } go down; a shot that ended by itself
// (the world changed, the view could not be made or was taken away) comes back as
// { op: 'cameraShotLocalEnded', reason }, raised to the client's scripts as
// cameraShotEnded(reason).
//
// A camera path (Camera.playPath) goes down as { op: 'cameraPathLocal', keys:
// [{ position, rotation | lookAt and roll, fov, time }], smooth }: the game client moves the view
// through the keys every frame (camera_shot.cpp), and { op: 'cameraPathLocalFinished' } comes
// back once it reaches the last key, raised as cameraPathFinished. Camera.endShot ends it.
//
// The camera's pose is asked of the game client at the call, as World's rays are
// (Client/runtime/physics/local_world.hpp): { op: 'localWorldCamera' } answers { m, fov }, the
// view camera's 3x4 rows (right, forward and up its columns 0, 1 and 2, the eye column 3) and its
// vertical field of view in radians, or null outside a world. rayFromScreen builds the ray through a
// point of the screen from it.

const { shotMessage } = require('../../Backend/scripting/runtimes/js/camera/shots.cjs');

const RAY_RANGE = 100;
const MAX_RAY_RANGE = 4096;
// The aspect a pose reports before the runtime knows the window's size.
const DEFAULT_ASPECT = 16 / 9;
const ANSWER_MS = 2000;
// A camera path's keys (camera_shot.cpp kMaxPathKeys) and its longest run in seconds.
const MIN_PATH_KEYS = 2;
const MAX_PATH_KEYS = 64;
const MAX_PATH_SECONDS = 600;

function unit(Vector3, x, y, z) {
  const length = Math.hypot(x, y, z) || 1;
  return new Vector3(x / length, y / length, z / length);
}

class ResourceCamera {
  // sync: the runtime's SyncChannel (the pose); screenSize(): { width, height } of the window.
  constructor({ send, emit, sync = null, screenSize = () => ({ width: 0, height: 0 }), Vector3 }) {
    this.send = send;
    this.emit = emit;
    this.sync = sync;
    this.screenSize = screenSize;
    this.Vector3 = Vector3;
    this.active = false;
    this.thirdPerson = false;
  }

  // This frame's camera pose and view; null with no world loaded.
  getPose() {
    const reply = this.sync ? this.sync.call({ op: 'localWorldCamera' }, ANSWER_MS) : undefined;
    const valid =
      reply !== null &&
      typeof reply === 'object' &&
      Array.isArray(reply.m) &&
      reply.m.length === 12 &&
      reply.m.every(Number.isFinite) &&
      Number.isFinite(reply.fov) &&
      reply.fov > 0;
    if (!valid) {
      return null;
    }
    const { m, fov } = reply;
    const { Vector3 } = this;
    const size = this.screenSize();
    const aspectRatio =
      size && size.width > 0 && size.height > 0 ? size.width / size.height : DEFAULT_ASPECT;
    return {
      position: new Vector3(m[3], m[7], m[11]),
      forward: unit(Vector3, m[1], m[5], m[9]),
      right: unit(Vector3, m[0], m[4], m[8]),
      up: unit(Vector3, m[2], m[6], m[10]),
      fov: (fov * 180) / Math.PI,
      aspectRatio,
    };
  }

  // A line from the camera through a screen position (-1 .. 1 left to right and bottom to top).
  rayFromScreen(options = {}) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('Camera.rayFromScreen: options must be an object');
    }
    const { x = 0, y = 0, range = RAY_RANGE } = options;
    for (const [name, value] of [
      ['x', x],
      ['y', y],
    ]) {
      if (typeof value !== 'number' || !Number.isFinite(value) || value < -1 || value > 1) {
        throw new RangeError(`Camera.rayFromScreen: ${name} must be from -1 to 1`);
      }
    }
    if (typeof range !== 'number' || !(range > 0) || range > MAX_RAY_RANGE) {
      throw new RangeError(
        `Camera.rayFromScreen: range must be above 0 and at most ${MAX_RAY_RANGE}`,
      );
    }
    const pose = this.getPose();
    if (!pose) {
      return null;
    }
    const tanHalf = Math.tan((pose.fov * Math.PI) / 360);
    const across = x * tanHalf * pose.aspectRatio;
    const upward = y * tanHalf;
    const direction = unit(
      this.Vector3,
      pose.forward.x + pose.right.x * across + pose.up.x * upward,
      pose.forward.y + pose.right.y * across + pose.up.y * upward,
      pose.forward.z + pose.right.z * across + pose.up.z * upward,
    );
    const origin = pose.position;
    const target = new this.Vector3(
      origin.x + direction.x * range,
      origin.y + direction.y * range,
      origin.z + direction.z * range,
    );
    return { origin, direction, target };
  }

  startShot(shot) {
    const message = shotMessage({ id: 0 }, shot);
    delete message.id;
    message.op = 'cameraShotLocal';
    this.send(message);
    this.active = true;
    return true;
  }

  // Moves the view through the keys, each { position, rotation | lookAt, roll, fov, time }, at its
  // time in seconds from now; smooth (the default) curves through them, false runs straight.
  playPath(keys, options = {}) {
    if (!Array.isArray(keys) || keys.length < MIN_PATH_KEYS || keys.length > MAX_PATH_KEYS) {
      throw new RangeError(
        `Camera.playPath: keys is an array of ${MIN_PATH_KEYS} to ${MAX_PATH_KEYS} keys`,
      );
    }
    if (options === null || typeof options !== 'object') {
      throw new TypeError('Camera.playPath: options must be an object');
    }
    const smooth = options.smooth ?? true;
    if (typeof smooth !== 'boolean') {
      throw new TypeError('Camera.playPath: smooth is true or false');
    }
    let last = -1;
    const sent = keys.map((key, index) => {
      const time = key?.time;
      if (!Number.isFinite(time) || time <= last || time > MAX_PATH_SECONDS) {
        throw new RangeError(
          `Camera.playPath: key ${index} needs a time after the key before, up to ${MAX_PATH_SECONDS} s`,
        );
      }
      last = time;
      const message = shotMessage({ id: 0 }, { ...key, seconds: 0, ease: true });
      return {
        position: message.position,
        lookAt: message.lookAt,
        roll: message.roll,
        rotation: message.rotation,
        fov: message.fov,
        time,
      };
    });
    this.send({ op: 'cameraPathLocal', keys: sent, smooth });
    this.active = true;
    return true;
  }

  // Shakes or tilts this machine's view with the game's own view shake: angle
  // { x pitch, positive down (x -20 looked 17 degrees up at 0.15 s), y roll, z yaw } in
  // degrees up to 90, shift { x, y, z } in metres up to 2,
  // duration 0.01 to 10 s, frequency 0 to 100 shakes a second (0, the default, one push that
  // settles), randomness 0 to 1.
  shake(options) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('Camera.shake takes { angle, shift, duration, frequency, randomness }');
    }
    const triple = (value, name, most) => {
      if (value === undefined) {
        return { x: 0, y: 0, z: 0 };
      }
      const fields = value && [value.x ?? 0, value.y ?? 0, value.z ?? 0];
      if (!fields || !fields.every((n) => Number.isFinite(n) && Math.abs(n) <= most)) {
        throw new RangeError(`Camera.shake: ${name} is { x, y, z }, each up to ${most} either way`);
      }
      return { x: fields[0], y: fields[1], z: fields[2] };
    };
    const ranged = (value, fallback, least, most, name) => {
      const read = value ?? fallback;
      if (!Number.isFinite(read) || read < least || read > most) {
        throw new RangeError(`Camera.shake: ${name} is ${least} to ${most}`);
      }
      return read;
    };
    this.send({
      op: 'cameraShakeLocal',
      angle: triple(options.angle, 'angle', 90),
      shift: triple(options.shift, 'shift', 2),
      duration: ranged(options.duration, 0.5, 0.01, 10, 'duration'),
      frequency: ranged(options.frequency, 0, 0, 100, 'frequency'),
      randomness: ranged(options.randomness, 0, 0, 1, 'randomness'),
    });
    return true;
  }

  // The path reached its last key; the view holds there until endShot.
  receivePathFinished() {
    if (this.active) {
      this.emit('cameraPathFinished', []);
    }
  }

  endShot() {
    if (!this.active) {
      return false;
    }
    this.active = false;
    this.send({ op: 'cameraClearLocal' });
    return true;
  }

  // This machine's view through the game's third-person camera; null turns it off.
  setThirdPerson(options) {
    if (options === null) {
      this.thirdPerson = false;
      this.send({ op: 'cameraThirdPerson', on: false });
      return;
    }
    if (options !== undefined && typeof options !== 'object') {
      throw new TypeError('Camera.setThirdPerson: options must be an object or null');
    }
    const { distance = 2.4, height = 1.6, shoulder = 0.45 } = options ?? {};
    const limits = [
      ['distance', distance, 0.3, 20],
      ['height', height, -5, 5],
      ['shoulder', shoulder, -5, 5],
    ];
    for (const [name, value, least, most] of limits) {
      if (typeof value !== 'number' || !(value >= least && value <= most)) {
        throw new RangeError(`Camera.setThirdPerson: ${name} must be from ${least} to ${most}`);
      }
    }
    this.thirdPerson = true;
    this.send({ op: 'cameraThirdPerson', on: true, distance, height, shoulder });
  }

  receiveEnded({ reason }) {
    if (!this.active) {
      return;
    }
    this.active = false;
    this.emit('cameraShotEnded', [String(reason ?? 'lost')]);
  }

  facade() {
    const camera = this;
    return Object.freeze({
      startShot(shot) {
        return camera.startShot(shot);
      },
      playPath(keys, options) {
        return camera.playPath(keys, options);
      },
      shake(options) {
        return camera.shake(options);
      },
      endShot() {
        return camera.endShot();
      },
      inShot() {
        return camera.active;
      },
      setThirdPerson(options) {
        camera.setThirdPerson(options);
      },
      isThirdPerson() {
        return camera.thirdPerson;
      },
      getPose() {
        return camera.getPose();
      },
      rayFromScreen(options) {
        return camera.rayFromScreen(options);
      },
    });
  }
}

module.exports = { ResourceCamera };
