'use strict';

// The client-only Audio global: the game's own sound triggers played, stopped and
// steered on this machine; nothing is replicated. Every call is a synchronous request to the game
// client (sync_channel.cjs; Client/runtime/audio/resource_audio.cpp answers it on its next frame)
// and returns whether the game took it: false before a world is loaded, for a name the game's
// audio data does not declare, and for an entity that is not there.
//
//   { op: 'audioPlay', trigger }                  on the local player, where the listener is
//   { op: 'audioPlayAt', trigger, x, y, z }        a free audio object at that point
//   { op: 'audioPlayOnEntity', trigger, entity }
//   { op: 'audioPlayFile', file, entity, purpose }  an .ogg through a voice host event
//   { op: 'audioStopOnEntity', trigger, entity }
//   { op: 'audioStopAll', entity }
//   { op: 'audioParameter', entity, parameter, value }
//   { op: 'audioSwitch', entity, switch, state }
//   { op: 'audioHasTrigger', trigger }
//   { op: 'audioLoopAt', trigger, x, y, z, loop }  a sound kept playing at a point, as loop (the
//                                                  number this runtime gave it)
//   { op: 'audioLoopStop', loop }

const MAX_NAME_BYTES = 256;
const MAX_COORDINATE = 16384;
// The game's voice host events a file plays through (the switch standalone_file_purpose's
// states): an inner voice, a bark heard up to that many metres, a dialogue, cutscene or music.
const FILE_PURPOSES = new Set([
  'inner',
  '10',
  '20',
  '35',
  '50',
  '100',
  '200',
  'sfp_dialog',
  'sfp_cutscene',
  'sfp_music',
]);
const DEFAULT_FILE_PURPOSE = '50';

const encoder = new TextEncoder();

function fail(message) {
  throw new TypeError(`Audio: ${message}`);
}

function nameOf(value, what) {
  if (typeof value !== 'string' || value.length === 0) {
    fail(`${what} is a non-empty name`);
  }
  if (encoder.encode(value).length > MAX_NAME_BYTES) {
    fail(`${what} is at most ${MAX_NAME_BYTES} UTF-8 bytes`);
  }
  return value;
}

// A game-root path: forward slashes, no root, drive or parent folder.
function fileOf(value) {
  const file = nameOf(value, 'a file');
  const segments = file.split('/');
  const unsafe =
    file.includes('\\') ||
    file.includes(':') ||
    segments.some((segment) => segment === '' || segment === '.' || segment === '..');
  if (unsafe) {
    fail('a file is a game-root path such as sounds/kcds/<resource>/line');
  }
  return file;
}

function purposeOf(value) {
  if (value === undefined) {
    return DEFAULT_FILE_PURPOSE;
  }
  if (typeof value !== 'string' || !FILE_PURPOSES.has(value)) {
    fail(`a purpose is one of ${[...FILE_PURPOSES].join(', ')}`);
  }
  return value;
}

function entityOf(value) {
  if (!Number.isInteger(value) || value < 1 || value > 0xffffffff) {
    fail('an entity id is a positive whole number');
  }
  return value;
}

function pointOf(value) {
  if (value === null || typeof value !== 'object') {
    fail('a position is { x, y, z }');
  }
  const point = {};
  for (const axis of ['x', 'y', 'z']) {
    const coordinate = value[axis];
    if (typeof coordinate !== 'number' || !Number.isFinite(coordinate)) {
      fail('a position is { x, y, z } in finite metres');
    }
    if (Math.abs(coordinate) > MAX_COORDINATE) {
      fail(`a coordinate is within ±${MAX_COORDINATE} m`);
    }
    point[axis] = coordinate;
  }
  return point;
}

class ResourceAudio {
  // sync: the runtime's SyncChannel.
  constructor({ sync }) {
    this.sync = sync;
    // The last number given to a kept sound (loopAt).
    this.loops = 0;
  }

  ask(request) {
    return this.sync.call(request) === true;
  }

  facade() {
    return Object.freeze({
      play: (trigger) => this.ask({ op: 'audioPlay', trigger: nameOf(trigger, 'a trigger') }),
      playAt: (trigger, position) =>
        this.ask({
          op: 'audioPlayAt',
          trigger: nameOf(trigger, 'a trigger'),
          ...pointOf(position),
        }),
      playFrom: (trigger, entityId) =>
        this.ask({
          op: 'audioPlayOnEntity',
          trigger: nameOf(trigger, 'a trigger'),
          entity: entityOf(entityId),
        }),
      playFile: (file, entityId, purpose) =>
        this.ask({
          op: 'audioPlayFile',
          file: fileOf(file),
          entity: entityOf(entityId),
          purpose: purposeOf(purpose),
        }),
      stopFrom: (trigger, entityId) =>
        this.ask({
          op: 'audioStopOnEntity',
          trigger: nameOf(trigger, 'a trigger'),
          entity: entityOf(entityId),
        }),
      silenceEntity: (entityId) => this.ask({ op: 'audioStopAll', entity: entityOf(entityId) }),
      setSoundParameter: (entityId, parameter, value) => {
        if (typeof value !== 'number' || !Number.isFinite(value)) {
          fail('a parameter value is a finite number');
        }
        return this.ask({
          op: 'audioParameter',
          entity: entityOf(entityId),
          parameter: nameOf(parameter, 'a parameter'),
          value,
        });
      },
      setSoundSwitch: (entityId, switchName, state) =>
        this.ask({
          op: 'audioSwitch',
          entity: entityOf(entityId),
          switch: nameOf(switchName, 'a switch'),
          state: nameOf(state, 'a switch state'),
        }),
      // A sound kept playing at a point until stopLoop (an ambience, a fire): its number, or 0
      // when the game did not play it.
      loopAt: (trigger, position) => {
        const request = { op: 'audioLoopAt', trigger: nameOf(trigger, 'a trigger') };
        Object.assign(request, pointOf(position));
        this.loops += 1;
        request.loop = this.loops;
        return this.ask(request) ? request.loop : 0;
      },
      stopLoop: (loop) => {
        if (!Number.isInteger(loop) || loop < 1) {
          fail('a loop is the number loopAt answered');
        }
        return this.ask({ op: 'audioLoopStop', loop });
      },
      triggerExists: (trigger) =>
        this.ask({ op: 'audioHasTrigger', trigger: nameOf(trigger, 'a trigger') }),
    });
  }
}

module.exports = { ResourceAudio };
