'use strict';

// Voice chat's server side for scripts. The native relay forwards the frames; this
// holds the rules scripts set and tells it of them.
//
//   { op: 'voiceEnabled', enabled }                  the whole server, its clients' devices too
//   { op: 'voiceIndicator', shown }                  the players' games show their indicator
//   { op: 'voiceRange', tier, meters }               how far a tier reaches
//   { op: 'voiceTier', id, tier }                    the tier a player talks in
//   { op: 'voiceMuted', id, muted }                  nobody hears this player
//   { op: 'voiceDeaf', id, deaf }                    this player hears nobody
//   { op: 'voiceMuffle', id, amount }                how muffled this player sounds, 0 to 1
//   { op: 'voiceListenerMute', id, talker, muted }   this listener alone does not hear that talker
//   { op: 'voiceTalking', id, talking }              from the relay
//   { op: 'voiceTierRequest', id, tier }             from the player's game

const TIERS = Object.freeze(['whisper', 'normal', 'shout']);
const MAX_RANGE = 500;

function tierIndex(tier, name) {
  const index = TIERS.indexOf(tier);
  if (index < 0) {
    throw new TypeError(`${name}: tier is 'whisper', 'normal' or 'shout'`);
  }
  return index;
}

class VoiceChat {
  constructor(host) {
    this.host = host;
    this.enabled = true;
    // Whether the players' games show their own voice indicator.
    this.indicator = true;
    this.ranges = [2, 10, 30];
    // Player id -> { tier, muted, deaf, listenerMutes }, made when a script or the player sets one.
    this.players = new Map();
    this.talking = new Set();
  }

  player(player, name) {
    const target = player && this.host.players.get(player.id);
    if (!target || target !== player) {
      throw new TypeError(`${name} takes a connected player`);
    }
    return target;
  }

  state(id) {
    let state = this.players.get(id);
    if (!state) {
      state = { tier: 1, muted: false, deaf: false, listenerMutes: new Set() };
      this.players.set(id, state);
    }
    return state;
  }

  setEnabled(enabled) {
    if (typeof enabled !== 'boolean') {
      throw new TypeError('Voice.setEnabled takes true or false');
    }
    if (enabled === this.enabled) {
      return;
    }
    this.enabled = enabled;
    this.host.bridge.send({ op: 'voiceEnabled', enabled });
    if (!enabled) {
      for (const id of [...this.talking]) {
        this.talkingChanged(id, false);
      }
    }
  }

  setIndicator(shown) {
    if (typeof shown !== 'boolean') {
      throw new TypeError('Voice.setIndicator takes true or false');
    }
    if (shown === this.indicator) {
      return;
    }
    this.indicator = shown;
    this.host.bridge.send({ op: 'voiceIndicator', shown });
  }

  setRange(tier, meters) {
    const index = tierIndex(tier, 'Voice.setRange');
    if (!Number.isFinite(meters) || meters < 0 || meters > MAX_RANGE) {
      throw new RangeError(`Voice.setRange: meters is 0 to ${MAX_RANGE}`);
    }
    this.ranges[index] = meters;
    this.host.bridge.send({ op: 'voiceRange', tier: index, meters });
  }

  setPlayerTier(player, tier) {
    const target = this.player(player, 'Voice.setPlayerTier');
    const index = tierIndex(tier, 'Voice.setPlayerTier');
    this.state(target.id).tier = index;
    this.host.bridge.send({ op: 'voiceTier', id: target.id, tier: index });
  }

  setPlayerMuted(player, muted) {
    const target = this.player(player, 'Voice.setPlayerMuted');
    this.state(target.id).muted = muted === true;
    this.host.bridge.send({ op: 'voiceMuted', id: target.id, muted: muted === true });
  }

  setPlayerMuffled(player, amount) {
    const target = this.player(player, 'Voice.setPlayerMuffled');
    const value = amount === true ? 1 : amount === false ? 0 : amount;
    if (typeof value !== 'number' || !Number.isFinite(value) || value < 0 || value > 1) {
      throw new RangeError('Voice.setPlayerMuffled takes true, false or a number from 0 to 1');
    }
    this.state(target.id).muffled = value;
    this.host.bridge.send({ op: 'voiceMuffle', id: target.id, amount: value });
  }

  setPlayerDeaf(player, deaf) {
    const target = this.player(player, 'Voice.setPlayerDeaf');
    this.state(target.id).deaf = deaf === true;
    this.host.bridge.send({ op: 'voiceDeaf', id: target.id, deaf: deaf === true });
  }

  setListenerMuted(listener, talker, muted) {
    const hearing = this.player(listener, 'Voice.setListenerMuted');
    const speaking = this.player(talker, 'Voice.setListenerMuted');
    const mutes = this.state(hearing.id).listenerMutes;
    if (muted === true) {
      mutes.add(speaking.id);
    } else {
      mutes.delete(speaking.id);
    }
    this.host.bridge.send({
      op: 'voiceListenerMute',
      id: hearing.id,
      talker: speaking.id,
      muted: muted === true,
    });
  }

  talkingChanged(id, talking) {
    const player = this.host.players.get(id);
    if (talking === this.talking.has(id)) {
      return;
    }
    if (talking) {
      this.talking.add(id);
    } else {
      this.talking.delete(id);
    }
    if (player) {
      this.host.emit('voiceTalkingChanged', player, talking);
    }
  }

  // The player asked for another tier: a script's handler returning false refuses it.
  tierRequested(id, index) {
    const player = this.host.players.get(id);
    const tier = TIERS[index];
    if (!player || !tier) {
      return;
    }
    if (this.host.emit('voiceTierRequest', player, tier) === false) {
      return;
    }
    this.setPlayerTier(player, tier);
  }

  left(id) {
    this.talkingChanged(id, false);
    this.players.delete(id);
    for (const state of this.players.values()) {
      state.listenerMutes.delete(id);
    }
  }

  api() {
    const self = this;
    return Object.freeze({
      setEnabled: (enabled) => self.setEnabled(enabled),
      isEnabled: () => self.enabled,
      setIndicator: (shown) => self.setIndicator(shown),
      isIndicatorShown: () => self.indicator,
      setRange: (tier, meters) => self.setRange(tier, meters),
      getRange: (tier) => self.ranges[tierIndex(tier, 'Voice.getRange')],
      setPlayerTier: (player, tier) => self.setPlayerTier(player, tier),
      getPlayerTier: (player) => {
        const target = self.player(player, 'Voice.getPlayerTier');
        return TIERS[self.players.get(target.id)?.tier ?? 1];
      },
      setPlayerMuted: (player, muted) => self.setPlayerMuted(player, muted),
      isPlayerMuted: (player) => {
        const target = self.player(player, 'Voice.isPlayerMuted');
        return self.players.get(target.id)?.muted === true;
      },
      setPlayerMuffled: (player, amount) => self.setPlayerMuffled(player, amount),
      getPlayerMuffled: (player) => {
        const target = self.player(player, 'Voice.getPlayerMuffled');
        return self.players.get(target.id)?.muffled ?? 0;
      },
      setPlayerDeaf: (player, deaf) => self.setPlayerDeaf(player, deaf),
      isPlayerDeaf: (player) => {
        const target = self.player(player, 'Voice.isPlayerDeaf');
        return self.players.get(target.id)?.deaf === true;
      },
      setListenerMuted: (listener, talker, muted) => self.setListenerMuted(listener, talker, muted),
      isListenerMuted: (listener, talker) => {
        const hearing = self.player(listener, 'Voice.isListenerMuted');
        const speaking = self.player(talker, 'Voice.isListenerMuted');
        return self.players.get(hearing.id)?.listenerMutes.has(speaking.id) === true;
      },
      isTalking: (player) => {
        const target = self.player(player, 'Voice.isTalking');
        return self.talking.has(target.id);
      },
    });
  }
}

module.exports = { VoiceChat, TIERS };
