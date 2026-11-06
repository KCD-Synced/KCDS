'use strict';

// The client-only LocalVoice global: this player's voice chat. The game client sends what it knows
// ({ op: 'voiceState', enabled, tier, muted, deaf, transmitting, hasMicrophone }) when it changes; the
// calls go down as voiceSettings, voiceTierAsk and voiceZones. Zones are checked here, so a bad file
// leaves voice working without effects and tells the script why.

const MODES = Object.freeze(['push', 'voice']);
const TIERS = Object.freeze(['whisper', 'normal', 'shout']);
const ROOMS = Object.freeze(['outdoors', 'hut', 'tavern', 'hall', 'church', 'dungeon']);
const MAX_ZONES = 512;

function isPoint(value) {
  return (
    value !== null &&
    typeof value === 'object' &&
    Number.isFinite(value.x) &&
    Number.isFinite(value.y) &&
    Number.isFinite(value.z)
  );
}

// The zones as the game client takes them: eight numbers for each.
function flatZones(zones) {
  if (!Array.isArray(zones) || zones.length > MAX_ZONES) {
    throw new TypeError(`LocalVoice.setZones takes a list of at most ${MAX_ZONES} zones`);
  }
  const flat = [];
  zones.forEach((zone, index) => {
    const name = `LocalVoice.setZones: zone ${index}`;
    if (zone === null || typeof zone !== 'object' || !isPoint(zone.min) || !isPoint(zone.max)) {
      throw new TypeError(`${name} needs min and max points`);
    }
    if (zone.min.x > zone.max.x || zone.min.y > zone.max.y || zone.min.z > zone.max.z) {
      throw new RangeError(`${name}: min must not be above max`);
    }
    const room = ROOMS.indexOf(zone.reverb ?? 'outdoors');
    if (room < 0) {
      throw new TypeError(`${name}: reverb is one of ${ROOMS.join(', ')}`);
    }
    const occlusion = zone.occlusion ?? 0;
    if (!Number.isFinite(occlusion) || occlusion < 0 || occlusion > 1) {
      throw new RangeError(`${name}: occlusion is 0 to 1`);
    }
    flat.push(
      zone.min.x,
      zone.min.y,
      zone.min.z,
      zone.max.x,
      zone.max.y,
      zone.max.z,
      room,
      occlusion,
    );
  });
  return flat;
}

class ResourceLocalVoice {
  constructor({ send, emit }) {
    this.send = send;
    this.emit = emit;
    this.state = {
      enabled: false,
      tier: 1,
      muted: false,
      deaf: false,
      transmitting: false,
      hasMicrophone: false,
    };
    this.mode = 'push';
  }

  // { op: 'voiceState', ... } from the game client.
  update(message) {
    const before = this.state.transmitting;
    this.state = {
      enabled: message.enabled === true,
      tier: Number.isInteger(message.tier) ? message.tier : 1,
      muted: message.muted === true,
      deaf: message.deaf === true,
      transmitting: message.transmitting === true,
      hasMicrophone: message.hasMicrophone === true,
    };
    if (typeof message.mode === 'string') {
      this.mode = message.mode;
    }
    if (before !== this.state.transmitting) {
      this.emit('localVoiceTransmitting', [this.state.transmitting]);
    }
  }

  facade() {
    const voice = this;
    return Object.freeze({
      isEnabled: () => voice.state.enabled,
      hasMicrophone: () => voice.state.hasMicrophone,
      isTransmitting: () => voice.state.transmitting,
      isMuted: () => voice.state.muted,
      isDeaf: () => voice.state.deaf,
      getTier: () => TIERS[voice.state.tier],
      requestTier(tier) {
        const index = TIERS.indexOf(tier);
        if (index < 0) {
          throw new TypeError("LocalVoice.requestTier: tier is 'whisper', 'normal' or 'shout'");
        }
        voice.send({ op: 'voiceTierAsk', tier: index });
      },
      getMode: () => voice.mode,
      setMode(mode) {
        if (!MODES.includes(mode)) {
          throw new TypeError("LocalVoice.setMode: mode is 'push' or 'voice'");
        }
        voice.mode = mode;
        voice.send({ op: 'voiceSettings', mode });
      },
      setZones(zones) {
        voice.send({ op: 'voiceZones', zones: flatZones(zones) });
      },
    });
  }
}

module.exports = { ResourceLocalVoice, flatZones };
