'use strict';

// The client-only NoClip global: this machine's free camera.
//
// The game client flies the camera (Client/runtime/noclip/free_camera.cpp): a view of the
// engine's view system made the active camera and given its pose every frame. It reports the
// camera's state ({ op: 'noclipState', ... }) and the end of a flight it did not ask for
// ({ op: 'noclipEnded', reason }); the calls here go down as noclipEnable, noclipDisable,
// noclipPose, noclipSettings and noclipMove. Poses, aims, rays and projections are worked out
// here from the last reported state, so the reads answer at once.
//
// The server may forbid the local player NoClip (Player.noclipAllowed): the game
// client says so ({ op: 'noclipAllowed', allowed }), refuses a flight and ends one that runs.
//
// Yaw is in degrees from +Y, rising to the left; pitch in degrees, positive up.

const MODES = Object.freeze(['body', 'camera']);
const LOOKS = Object.freeze(['always', 'rightButton']);
// An orbit's distance in metres.
const ORBIT_NEAREST = 0.5;
const ORBIT_FARTHEST = 50;
const ORBIT_DEFAULT = 3;
const MIN_SPEED = 0.05;
const MAX_SPEED = 400;
const DEFAULT_SPEED = 12;
const MIN_FOV = 20;
const MAX_FOV = 140;
const MAX_STEP = 0.1;

function finite(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function isVector(value) {
  return (
    value !== null &&
    typeof value === 'object' &&
    finite(value.x) &&
    finite(value.y) &&
    finite(value.z)
  );
}

function radians(degrees) {
  return (degrees * Math.PI) / 180;
}

function degrees(radians) {
  return (radians * 180) / Math.PI;
}

function wrapYaw(yaw) {
  const wrapped = (((yaw + 180) % 360) + 360) % 360;
  return wrapped - 180;
}

function clampPitch(pitch) {
  return Math.max(-89, Math.min(89, pitch));
}

// The camera's unit forward, right (level) and up for a heading and an elevation.
function basis(yaw, pitch) {
  const y = radians(yaw);
  const p = radians(pitch);
  return {
    forward: [-Math.sin(y) * Math.cos(p), Math.cos(y) * Math.cos(p), Math.sin(p)],
    right: [Math.cos(y), Math.sin(y), 0],
    up: [Math.sin(y) * Math.sin(p), -Math.cos(y) * Math.sin(p), Math.cos(p)],
  };
}

// The heading and elevation that look along a direction; null for one of no length.
function aim(x, y, z) {
  const length = Math.hypot(x, y, z);
  if (!(length > 0)) {
    return null;
  }
  return {
    yaw: degrees(Math.atan2(-x, y)),
    pitch: clampPitch(degrees(Math.asin(z / length))),
  };
}

class ResourceNoClip {
  constructor({ send, emit, Vector3 }) {
    this.send = send;
    this.emit = emit;
    this.Vector3 = Vector3;
    this.active = false;
    this.mode = 'body';
    this.input = true;
    // 'always', or 'rightButton': the camera turns and moves only while the right mouse button is
    // held, so a page with the focus keeps the pointer and the keys otherwise.
    this.look = 'always';
    this.speed = DEFAULT_SPEED;
    this.fov = 0;
    this.state = null;
    // Calls that move the camera made after enable() and before the game reported the camera's
    // first state: they run, in order, as soon as it does.
    this.waiting = [];
    // Whether the server lets the local player fly.
    this.allowed = true;
  }

  // True while enable() was called and the game has not reported the camera yet.
  starting() {
    return this.active && this.state === null;
  }

  // Keeps a call for the camera's first state; true when it was kept.
  later(call) {
    if (!this.starting()) {
      return false;
    }
    this.waiting.push(call);
    return true;
  }

  vector([x, y, z]) {
    return new this.Vector3(x, y, z);
  }

  settings() {
    return { input: this.input, look: this.look, speed: this.speed, fov: this.fov };
  }

  enable(options = {}) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('NoClip.enable: options must be an object');
    }
    const mode = options.mode ?? 'body';
    if (!MODES.includes(mode)) {
      throw new TypeError('NoClip.enable: mode must be body or camera');
    }
    const input = options.input ?? true;
    if (typeof input !== 'boolean') {
      throw new TypeError('NoClip.enable: input must be a boolean');
    }
    const look = options.look ?? 'always';
    if (!LOOKS.includes(look)) {
      throw new TypeError("NoClip.enable: look must be 'always' or 'rightButton'");
    }
    // The player sees their own body during the flight.
    const showBody = options.showBody ?? false;
    if (typeof showBody !== 'boolean') {
      throw new TypeError('NoClip.enable: showBody must be a boolean');
    }
    // The camera circles a point: the mouse turns it around, W/S nearer or farther.
    let orbit = null;
    if (options.orbit !== undefined) {
      const { target, distance = ORBIT_DEFAULT } = options.orbit ?? {};
      if (!isVector(target)) {
        throw new TypeError('NoClip.enable: orbit.target must be a Vector3');
      }
      if (!finite(distance) || distance < ORBIT_NEAREST || distance > ORBIT_FARTHEST) {
        throw new RangeError(
          `NoClip.enable: orbit.distance must be from ${ORBIT_NEAREST} to ${ORBIT_FARTHEST}`,
        );
      }
      orbit = { target: { x: target.x, y: target.y, z: target.z }, distance };
    }
    const keepAboveGround = options.keepAboveGround ?? true;
    if (typeof keepAboveGround !== 'boolean') {
      throw new TypeError('NoClip.enable: keepAboveGround must be a boolean');
    }
    if (options.speed !== undefined && !finite(options.speed)) {
      throw new TypeError('NoClip.enable: speed must be a finite number');
    }
    if (options.fov !== undefined && (!finite(options.fov) || options.fov > MAX_FOV)) {
      throw new RangeError(`NoClip.enable: fov must be a number up to ${MAX_FOV}`);
    }
    if (options.position !== undefined && !isVector(options.position)) {
      throw new TypeError('NoClip.enable: position must be a Vector3');
    }
    if (options.forward !== undefined && !isVector(options.forward)) {
      throw new TypeError('NoClip.enable: forward must be a Vector3');
    }
    if (!this.allowed) {
      return 'notAllowed';
    }
    if (this.active) {
      return 'alreadyActive';
    }
    this.mode = mode;
    this.input = input;
    this.look = look;
    if (options.speed !== undefined) {
      this.speed = Math.max(MIN_SPEED, Math.min(MAX_SPEED, options.speed));
    }
    this.fov = options.fov === undefined ? 0 : Math.max(MIN_FOV, options.fov);
    this.active = true;
    this.state = null;
    this.waiting = [];
    const message = { op: 'noclipEnable', mode, showBody, ...this.settings() };
    if (orbit) {
      message.orbit = orbit;
      message.keepAboveGround = keepAboveGround;
    }
    this.send(message);
    // A starting pose is the first thing the camera does.
    const { position, forward } = options;
    if (position !== undefined || forward !== undefined) {
      this.waiting.push((facade) => {
        if (position !== undefined && forward !== undefined) {
          facade.setPose(position, forward);
        } else if (position !== undefined) {
          facade.setPosition(position);
        } else {
          const at = facade.getPosition();
          facade.lookAt(new this.Vector3(at.x + forward.x, at.y + forward.y, at.z + forward.z));
        }
      });
    }
    this.emit('noclipChanged', [true, 'script']);
    return 'enabled';
  }

  disable(options = {}) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('NoClip.disable: options must be an object');
    }
    const keepPosition = options.keepPosition ?? true;
    if (typeof keepPosition !== 'boolean') {
      throw new TypeError('NoClip.disable: keepPosition must be a boolean');
    }
    if (!this.active) {
      return;
    }
    this.active = false;
    this.state = null;
    this.waiting = [];
    this.send({ op: 'noclipDisable', keepPosition });
    this.emit('noclipChanged', [false, 'script']);
  }

  receiveState(message) {
    if (!this.active || !message.active) {
      return;
    }
    this.state = {
      x: message.x,
      y: message.y,
      z: message.z,
      yaw: message.yaw,
      pitch: message.pitch,
      fov: message.fov,
      renderedFov: message.renderedFov,
      aspect: message.aspect,
    };
    if (this.waiting.length > 0 && this.facadeObject) {
      const waiting = this.waiting;
      this.waiting = [];
      for (const call of waiting) {
        call(this.facadeObject);
      }
    }
  }

  receiveEnded({ reason }) {
    if (!this.active) {
      return;
    }
    this.active = false;
    this.state = null;
    this.waiting = [];
    let mapped = 'viewLost';
    if (reason === 'worldChanged') {
      mapped = 'sessionOver';
    } else if (reason === 'notAllowed') {
      mapped = 'notAllowed';
    }
    this.emit('noclipChanged', [false, mapped]);
  }

  // The server allowed or forbade the local player NoClip. A flight that runs when
  // it is forbidden ends (the game client ended it already).
  receiveAllowed({ allowed }) {
    if (typeof allowed !== 'boolean' || allowed === this.allowed) {
      return;
    }
    this.allowed = allowed;
    if (!allowed && this.active) {
      this.active = false;
      this.state = null;
      this.waiting = [];
      this.emit('noclipChanged', [false, 'notAllowed']);
    }
    this.emit('noclipAllowedChanged', [allowed]);
  }

  // Puts the camera at a pose now, and tells the game client.
  pose(x, y, z, yaw, pitch) {
    const state = this.state ?? { fov: this.fov, renderedFov: this.fov, aspect: 16 / 9 };
    this.state = { ...state, x, y, z, yaw: wrapYaw(yaw), pitch: clampPitch(pitch) };
    this.send({ op: 'noclipPose', x, y, z, yaw: this.state.yaw, pitch: this.state.pitch });
    return true;
  }

  facade() {
    const noclip = this;
    const flying = () => noclip.active && noclip.state !== null;
    const facade = Object.freeze({
      enable(options) {
        return noclip.enable(options);
      },
      disable(options) {
        noclip.disable(options);
      },
      isActive() {
        return noclip.active;
      },
      isAllowed() {
        return noclip.allowed;
      },
      getState() {
        if (!flying()) {
          return null;
        }
        const { x, y, z, yaw, pitch, fov, renderedFov, aspect } = noclip.state;
        const { forward, right, up } = basis(yaw, pitch);
        return {
          mode: noclip.mode,
          input: noclip.input,
          position: noclip.vector([x, y, z]),
          forward: noclip.vector(forward),
          right: noclip.vector(right),
          up: noclip.vector(up),
          yaw,
          pitch,
          speed: noclip.speed,
          fov,
          renderedFov,
          aspectRatio: aspect,
        };
      },
      getPosition() {
        return flying() ? noclip.vector([noclip.state.x, noclip.state.y, noclip.state.z]) : null;
      },
      setPosition(position) {
        if (!flying()) {
          return noclip.later((self) => self.setPosition(position));
        }
        if (!isVector(position)) {
          return true;
        }
        const { yaw, pitch } = noclip.state;
        return noclip.pose(position.x, position.y, position.z, yaw, pitch);
      },
      getRotation() {
        return flying() ? { yaw: noclip.state.yaw, pitch: noclip.state.pitch } : null;
      },
      setRotation(rotation) {
        if (noclip.starting() && (rotation === null || typeof rotation !== 'object')) {
          throw new TypeError('NoClip.setRotation: rotation must be an object');
        }
        if (!flying()) {
          return noclip.later((self) => self.setRotation(rotation));
        }
        if (rotation === null || typeof rotation !== 'object') {
          throw new TypeError('NoClip.setRotation: rotation must be an object');
        }
        const { x, y, z } = noclip.state;
        const yaw = finite(rotation.yaw) ? rotation.yaw : noclip.state.yaw;
        const pitch = finite(rotation.pitch) ? rotation.pitch : noclip.state.pitch;
        return noclip.pose(x, y, z, yaw, pitch);
      },
      getForward() {
        if (!flying()) {
          return null;
        }
        return noclip.vector(basis(noclip.state.yaw, noclip.state.pitch).forward);
      },
      setPose(position, forward) {
        if (!flying()) {
          return noclip.later((self) => self.setPose(position, forward));
        }
        const angles = isVector(forward) ? aim(forward.x, forward.y, forward.z) : null;
        if (!isVector(position) || !angles) {
          return true;
        }
        return noclip.pose(position.x, position.y, position.z, angles.yaw, angles.pitch);
      },
      lookAt(point) {
        if (noclip.starting() && !isVector(point)) {
          throw new TypeError('NoClip.lookAt: point must be a Vector3');
        }
        if (!flying()) {
          return noclip.later((self) => self.lookAt(point));
        }
        if (!isVector(point)) {
          throw new TypeError('NoClip.lookAt: point must be a Vector3');
        }
        const { x, y, z } = noclip.state;
        const angles = aim(point.x - x, point.y - y, point.z - z);
        return angles ? noclip.pose(x, y, z, angles.yaw, angles.pitch) : true;
      },
      focus(target, distance = 6) {
        if (noclip.starting() && (!isVector(target) || !finite(distance))) {
          throw new TypeError(
            'NoClip.focus: target must be a Vector3 and distance a finite number',
          );
        }
        if (!flying()) {
          return noclip.later((self) => self.focus(target, distance));
        }
        if (!isVector(target)) {
          throw new TypeError('NoClip.focus: target must be a Vector3');
        }
        if (!finite(distance)) {
          throw new TypeError('NoClip.focus: distance must be a finite number');
        }
        const back = Math.max(1, Math.min(4096, distance));
        const { x, y, z } = noclip.state;
        const angles = aim(target.x - x, target.y - y, target.z - z) ?? {
          yaw: noclip.state.yaw,
          pitch: noclip.state.pitch,
        };
        const { forward } = basis(angles.yaw, angles.pitch);
        return noclip.pose(
          target.x - forward[0] * back,
          target.y - forward[1] * back,
          target.z - forward[2] * back,
          angles.yaw,
          angles.pitch,
        );
      },
      getSpeed() {
        return noclip.speed;
      },
      setSpeed(metresPerSecond) {
        if (!finite(metresPerSecond)) {
          throw new TypeError('NoClip.setSpeed: speed must be a finite number');
        }
        noclip.speed = Math.max(MIN_SPEED, Math.min(MAX_SPEED, metresPerSecond));
        if (noclip.active) {
          noclip.send({ op: 'noclipSettings', ...noclip.settings() });
        }
      },
      getFov() {
        return noclip.fov > 0 ? noclip.fov : noclip.state?.fov ?? 0;
      },
      setFov(value) {
        if (!finite(value)) {
          throw new TypeError('NoClip.setFov: degrees must be a finite number');
        }
        noclip.fov = Math.max(MIN_FOV, Math.min(MAX_FOV, value));
        if (noclip.active) {
          noclip.send({ op: 'noclipSettings', ...noclip.settings() });
        }
      },
      isInputEnabled() {
        return noclip.input;
      },
      setInputEnabled(enabled) {
        if (typeof enabled !== 'boolean') {
          throw new TypeError('NoClip.setInputEnabled: enabled must be a boolean');
        }
        noclip.input = enabled;
        if (noclip.active) {
          noclip.send({ op: 'noclipSettings', ...noclip.settings() });
        }
      },
      move(input, deltaSeconds) {
        if (input === null || typeof input !== 'object') {
          throw new TypeError('NoClip.move: input must be an object');
        }
        if (!finite(deltaSeconds)) {
          throw new TypeError('NoClip.move: deltaSeconds must be a finite number');
        }
        if (!flying() || noclip.input) {
          return false;
        }
        const axis = (value) => (finite(value) ? Math.max(-1, Math.min(1, value)) : 0);
        noclip.send({
          op: 'noclipMove',
          forward: axis(input.forward),
          strafe: axis(input.strafe),
          lift: axis(input.lift),
          yaw: finite(input.yaw) ? input.yaw : 0,
          pitch: finite(input.pitch) ? input.pitch : 0,
          boost: input.boost === true,
          crawl: input.crawl === true,
          dt: Math.max(0, Math.min(MAX_STEP, deltaSeconds)),
        });
        return true;
      },
      screenLine(x, y) {
        if (!finite(x) || !finite(y)) {
          throw new TypeError('NoClip.screenLine: x and y must be finite numbers');
        }
        if (!flying()) {
          return null;
        }
        const { yaw, pitch, renderedFov, aspect } = noclip.state;
        const { forward, right, up } = basis(yaw, pitch);
        const tanV = Math.tan(radians(renderedFov) / 2);
        const tanH = tanV * (aspect > 0 ? aspect : 16 / 9);
        const direction = [0, 1, 2].map(
          (axis) => forward[axis] + right[axis] * x * tanH + up[axis] * y * tanV,
        );
        const length = Math.hypot(...direction);
        return noclip.vector(direction.map((value) => value / length));
      },
      toScreen(point) {
        if (!isVector(point)) {
          throw new TypeError('NoClip.toScreen: point must be a Vector3');
        }
        if (!flying()) {
          return null;
        }
        const { x, y, z, yaw, pitch, renderedFov, aspect } = noclip.state;
        const { forward, right, up } = basis(yaw, pitch);
        const offset = [point.x - x, point.y - y, point.z - z];
        const dot = (axis) => axis[0] * offset[0] + axis[1] * offset[1] + axis[2] * offset[2];
        const depth = dot(forward);
        if (!(depth > 0)) {
          return null;
        }
        const tanV = Math.tan(radians(renderedFov) / 2);
        const tanH = tanV * (aspect > 0 ? aspect : 16 / 9);
        return { x: dot(right) / (depth * tanH), y: dot(up) / (depth * tanV) };
      },
    });
    noclip.facadeObject = facade;
    return facade;
  }
}

module.exports = { ResourceNoClip, basis, aim };
