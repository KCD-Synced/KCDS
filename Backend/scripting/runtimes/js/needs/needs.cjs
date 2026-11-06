'use strict';

// Whether the game's sleep (exhaust) and food (hunger) needs run for the players (a KCDS addition).
// They are off unless a resource turns them on: a multiplayer server
// does not want the single-player hunger, its lines ("I'm hungry") and penalties by default. The server tells every player's client ({ op: 'needs', id, enabled }) when it
// changes and, while the needs are off, when their world is ready; the client stops or restarts
// its game's own need timers, and lets them run again whenever it leaves a world, so a newcomer
// only hears it while they are off.
//
// Switching the needs off also fills both to their maximum, through the same order as
// Player.fillStat (the game's own state setter on the owner's client), so nobody stays tired or
// hungry, with the game's penalties, while the values cannot change; a newcomer is filled on
// arrival, before a resource's playerSpawned and playerReady handlers can set stored values.
// Switching them on again lets both run on from where they are.

const { MAX_STAT_VALUE, statRequest } = require('../stats/player_stats.cjs');

// The needs the switch covers, as the writable stats name them.
const NEED_STATS = ['exhaust', 'hunger'];

class Needs {
  constructor(host, isDown) {
    this.host = host;
    this.isDown = isDown;
    this.enabled = false;
  }

  send(player) {
    if (!player?.ready) {
      return;
    }
    this.host.bridge.send({ op: 'needs', id: player.id, enabled: this.enabled });
    if (!this.enabled && !this.isDown(player)) {
      this.fill(player);
    }
  }

  fill(player) {
    for (const stat of NEED_STATS) {
      this.host.bridge.send({ op: 'setStat', id: player.id, ...statRequest(stat, MAX_STAT_VALUE) });
    }
  }

  arrived(player) {
    if (!this.enabled) {
      this.send(player);
    }
  }

  api() {
    const self = this;
    return Object.freeze({
      // Whether the players' sleep and food needs run (false by default).
      get enabled() {
        return self.enabled;
      },
      setEnabled(enabled) {
        if (typeof enabled !== 'boolean') {
          throw new TypeError('Needs.setEnabled takes true or false');
        }
        if (self.enabled === enabled) {
          return;
        }
        self.enabled = enabled;
        for (const player of self.host.players.values()) {
          self.send(player);
        }
      },
    });
  }
}

module.exports = { Needs, NEED_STATS };
