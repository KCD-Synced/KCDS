'use strict';

// Scripted camera shots (Camera.startShot / Camera.endShot). The
// server places one player's camera at a point, aimed by a rotation or at a point to look at, with
// a field of view, and moves it there over some seconds; clearing gives the player their own view
// back at once. The player's body, input and the server's state of them are not touched.
//
//   { op: 'cameraShot', id: <player id>, position, rotation | lookAt and roll, fov, seconds }
//     rotation is a quaternion { x, y, z, w } (its roll included); lookAt a point { x, y, z } with
//     roll the turn about the line of sight (-180 to 180 degrees, positive turns the view's right
//     side down, 0 by default); fov in degrees (0 keeps the game's own, else 5 to 150); seconds
//     the move from where the camera is (0 to 60, 0 cuts).
//   { op: 'cameraClear', id }
//   { op: 'cameraShotEnded', id, reason: 'worldChanged' | 'unavailable' | 'lost' }
//     from the client: the shot ended without the server clearing it.
//
// A shot is kept per player while it holds; it is not sent again on a world-ready (the loading
// screen ends it, and the client says so).
//
// Not covered: the client-side Camera (getPose, rayFromScreen, startShot from a client script) and
// NoClip's members.

const MAX_COORDINATE = 100000;

function point(value, name) {
  const valid =
    value &&
    Number.isFinite(value.x) &&
    Number.isFinite(value.y) &&
    Number.isFinite(value.z) &&
    Math.abs(value.x) <= MAX_COORDINATE &&
    Math.abs(value.y) <= MAX_COORDINATE &&
    Math.abs(value.z) <= MAX_COORDINATE;
  if (!valid) {
    throw new TypeError(`Camera.startShot: ${name} must be a { x, y, z } in the world`);
  }
  return { x: value.x, y: value.y, z: value.z };
}

// The shot as it goes out; throws on a shot the client could not film.
function shotMessage(player, shot) {
  if (!shot || typeof shot !== 'object') {
    throw new TypeError(
      'Camera.startShot takes a shot { position, rotation | lookAt, fov, seconds }',
    );
  }
  const message = { op: 'cameraShot', id: player.id, position: point(shot.position, 'position') };
  if (shot.lookAt !== undefined) {
    message.lookAt = point(shot.lookAt, 'lookAt');
    const { x, y, z } = message.lookAt;
    const p = message.position;
    if (Math.hypot(x - p.x, y - p.y, z - p.z) < 1e-3) {
      throw new RangeError('Camera.startShot: lookAt must lie away from position');
    }
    const roll = shot.roll ?? 0;
    if (!Number.isFinite(roll) || Math.abs(roll) > 180) {
      throw new RangeError('Camera.startShot: roll is -180 to 180 degrees');
    }
    message.roll = roll;
  } else if (shot.rotation !== undefined) {
    if (shot.roll !== undefined) {
      throw new TypeError('Camera.startShot: roll goes with lookAt; a rotation carries its own');
    }
    const q = shot.rotation;
    const valid =
      q && [q.x, q.y, q.z, q.w].every(Number.isFinite) && Math.hypot(q.x, q.y, q.z, q.w) > 1e-3;
    if (!valid) {
      throw new TypeError('Camera.startShot: rotation must be a quaternion { x, y, z, w }');
    }
    message.rotation = { x: q.x, y: q.y, z: q.z, w: q.w };
  } else {
    throw new TypeError('Camera.startShot: a shot needs rotation or lookAt');
  }
  const fov = shot.fov ?? 0;
  if (!Number.isFinite(fov) || (fov !== 0 && (fov < 5 || fov > 150))) {
    throw new RangeError("Camera.startShot: fov is 0 (the game's own) or 5 to 150 degrees");
  }
  const seconds = shot.seconds ?? 0;
  if (!Number.isFinite(seconds) || seconds < 0 || seconds > 60) {
    throw new RangeError('Camera.startShot: seconds is 0 to 60');
  }
  // Eased by default: the move starts and ends slowly. ease: false moves at one speed, so shots
  // chained end to end run on without stopping.
  const ease = shot.ease ?? true;
  if (typeof ease !== 'boolean') {
    throw new TypeError('Camera.startShot: ease is true or false');
  }
  message.fov = fov;
  message.seconds = seconds;
  message.ease = ease;
  Object.assign(message, lookOf(shot.look));
  return message;
}

// look: the mouse turns the shot's view, `yaw` degrees to each side (1 to 180) and `up` and
// `down` (0 to 89) from where the shot looks, as the game's dice players look about the table
// Wire: lookYaw, lookUp, lookDown; none for a still shot.
const LOOK_KEYS = ['yaw', 'up', 'down'];

function lookOf(look) {
  if (look === undefined) {
    return {};
  }
  if (!look || typeof look !== 'object' || Array.isArray(look)) {
    throw new TypeError('Camera.startShot: look is { yaw, up, down } in degrees');
  }
  for (const key of Object.keys(look)) {
    if (!LOOK_KEYS.includes(key)) {
      throw new TypeError(`Camera.startShot: look does not take ${key}`);
    }
  }
  const degrees = (key, lowest, highest, fallback) => {
    const value = look[key] ?? fallback;
    if (!Number.isFinite(value) || value < lowest || value > highest) {
      throw new RangeError(`Camera.startShot: look.${key} is ${lowest} to ${highest} degrees`);
    }
    return Math.round(value);
  };
  return {
    lookYaw: degrees('yaw', 1, 180, 30),
    lookUp: degrees('up', 0, 89, 15),
    lookDown: degrees('down', 0, 89, 15),
  };
}

class CameraShots {
  constructor(host) {
    this.host = host;
    this.shots = new Map();
  }

  player(player, name) {
    const target = player && this.host.players.get(player.id);
    if (!target || target !== player) {
      throw new TypeError(`Camera.${name} takes a connected player`);
    }
    return target;
  }

  startShot(player, shot) {
    const target = this.player(player, 'startShot');
    const message = shotMessage(target, shot);
    if (!target.ready) {
      return false;
    }
    this.shots.set(target.id, message);
    this.host.bridge.send(message);
    return true;
  }

  endShot(player) {
    const target = this.player(player, 'endShot');
    if (!this.shots.delete(target.id)) {
      return false;
    }
    if (target.ready) {
      this.host.bridge.send({ op: 'cameraClear', id: target.id });
    }
    return true;
  }

  // cameraShotEnded: the client's shot ended by itself.
  ended(player, reason) {
    if (!player || !this.shots.delete(player.id)) {
      return;
    }
    this.host.emit('cameraShotEnded', player, String(reason ?? 'lost'));
  }

  left(player) {
    this.shots.delete(player.id);
  }

  api() {
    const self = this;
    return Object.freeze({
      startShot: (player, shot) => self.startShot(player, shot),
      endShot: (player) => self.endShot(player),
      inShot: (player) => Boolean(player) && self.shots.has(player.id),
    });
  }
}

module.exports = { CameraShots, shotMessage };
