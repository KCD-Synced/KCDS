'use strict';

// The sound of web views on each player's game (Player.webVolume, Player.webMuted): the server's
// share, 0 to 100 and muted or not, which the game applies on top of the player's own Browser
// volume setting (the game's Settings > Sound page). Told to that player's game with the reserved
// event kcds:webAudio { volume, muted }, never raised to scripts, when it changes; a player who
// joins starts at 100 and not muted.

const WEB_AUDIO_EVENT = 'kcds:webAudio';
const FULL = Object.freeze({ volume: 100, muted: false });

class WebAudio {
  constructor(host) {
    this.host = host;
    // player id -> { volume, muted } set by a script
    this.levels = new Map();
  }

  of(id) {
    return this.levels.get(id) ?? FULL;
  }

  setVolume(id, value) {
    if (typeof value !== 'number' || !Number.isFinite(value) || value < 0 || value > 100) {
      throw new RangeError('webVolume must be a number from 0 to 100');
    }
    this.update(id, { volume: value });
  }

  setMuted(id, value) {
    if (typeof value !== 'boolean') {
      throw new TypeError('webMuted must be true or false');
    }
    this.update(id, { muted: value });
  }

  // Only a change goes to the game.
  update(id, change) {
    const before = this.of(id);
    const next = { ...before, ...change };
    if (next.volume === before.volume && next.muted === before.muted) {
      return;
    }
    this.levels.set(id, next);
    this.host.services.emitToPlayer(id, WEB_AUDIO_EVENT, next);
  }

  left(id) {
    this.levels.delete(id);
  }
}

module.exports = { WebAudio, WEB_AUDIO_EVENT };
