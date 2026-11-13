'use strict';

// The corner of the screen the players' games draw the KCDS watermark in (a KCDS addition). The
// watermark cannot be turned off; a server only chooses its corner: 'bottomLeft' (where it stands
// by default), 'bottomRight', 'topRight' or 'topLeft'. The voice indicator always stands beside
// it toward the middle of the screen. The server tells every player's client
// ({ op: 'watermarkCorner', id, corner }) when it changes and, while it is not the bottom left one,
// when their world is ready; a client goes back to the bottom left corner when it connects. One
// player can have their own corner (player.setWatermarkCorner), null back to the server's.

const CORNERS = Object.freeze(['bottomLeft', 'bottomRight', 'topRight', 'topLeft']);

function validCorner(corner) {
  return CORNERS.includes(corner);
}

class Watermark {
  constructor(host) {
    this.host = host;
    this.corner = 'bottomLeft';
    // player -> the player's own corner.
    this.own = new WeakMap();
  }

  // The corner this player's game draws the watermark in.
  cornerFor(player) {
    return this.own.get(player) ?? this.corner;
  }

  send(player) {
    if (!player?.ready) {
      return;
    }
    this.host.bridge.send({
      op: 'watermarkCorner',
      id: player.id,
      corner: CORNERS.indexOf(this.cornerFor(player)),
    });
  }

  arrived(player) {
    if (this.cornerFor(player) !== 'bottomLeft') {
      this.send(player);
    }
  }

  // player.setWatermarkCorner: a corner for this player, null for the server's.
  setFor(player, corner) {
    if (corner !== null && !validCorner(corner)) {
      throw new TypeError(
        `Player.setWatermarkCorner takes ${CORNERS.map((c) => `'${c}'`).join(', ')} or null`,
      );
    }
    const before = this.cornerFor(player);
    if (corner === null) {
      this.own.delete(player);
    } else {
      this.own.set(player, corner);
    }
    if (this.cornerFor(player) !== before) {
      this.send(player);
    }
  }

  api() {
    const self = this;
    return Object.freeze({
      // The corner every player's watermark stands in ('bottomLeft' by default).
      get corner() {
        return self.corner;
      },
      setCorner(corner) {
        if (!validCorner(corner)) {
          throw new TypeError(
            `Watermark.setCorner takes ${CORNERS.map((c) => `'${c}'`).join(', ')}`,
          );
        }
        if (self.corner === corner) {
          return;
        }
        self.corner = corner;
        for (const player of self.host.players.values()) {
          if (!self.own.has(player)) {
            self.send(player);
          }
        }
      },
    });
  }
}

module.exports = { Watermark, WATERMARK_CORNERS: CORNERS };
