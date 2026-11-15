'use strict';

// Players sit on the seats and lie on the beds of the SmartObjects scripts placed (SmartObject.spawn,
// World Architect's exports).
//
// The game shows its own "Sit" and "Lie down" on a placed bench or bed, but its sitting never
// starts there: the player's request runs the seat's own behaviour, which the game gives only the
// level's seats (live: noninteractive for about 50 ms, then nothing). So a player's press of the
// seat's key (use_bed_sit, use_bed_sleep) comes here through the built-in event 'kcds:seat'
// (Client/resources/placed_seats.cjs), and the player sits the way Npc.useObject seats an NPC,
// from the same game data (npcs/object_use.cjs): put where the game's sit-down (or lie-down)
// starts on the side they stand, the sit-down onto the seat, the seat's idle until they stand up
// (the "Stand up" hint), then the stand-up. Everything is Player.playAnimation, so every player sees
// it. A seat holds one body at a time, NPCs included.

const EVENT = 'kcds:seat';
// How near the player the seat must be (across the ground, from the seat), as the game offers
// its own hint from about there.
const REACH = 2.2;
// The game's seat keys a press of which may seat the player.
const USE_ACTIONS = new Set(['use_bed_sit', 'use_bed_sleep']);

class PlayerSeats {
  // `host`: the resource host (its placed smart objects, objectUses and the players' animations).
  constructor(host, { helpers, motion, downStart, multiply, rotate, quaternion }) {
    this.host = host;
    this.helpers = helpers;
    this.motion = motion;
    this.downStart = downStart;
    this.multiply = multiply;
    this.rotate = rotate;
    this.quaternion = quaternion;
    // Player -> { object, place, key, approach, idle, leave, phase }
    this.using = new Map();
    host.on('playerAnimationEnd', (player, fragment, reason) => this.animated(player, reason));
  }

  // A player's request from their game; false for an event that is not this module's.
  receive(player, name, payload) {
    if (name !== EVENT) {
      return false;
    }
    if (payload?.op === 'use' && USE_ACTIONS.has(payload.action)) {
      this.use(player);
    } else if (payload?.op === 'stand') {
      this.stand(player);
    }
    return true;
  }

  // The nearest free seat or bed of a placed SmartObject within REACH of the player, in their
  // dimension: { object, seat }, or null.
  nearest(player) {
    const at = player.position;
    if (!at) {
      return null;
    }
    let best = null;
    for (const object of this.host.smartObjects.placed.values()) {
      if (object.dimension !== player.dimension) {
        continue;
      }
      let seats = [];
      try {
        seats = this.host.objectUses.seatsOf(object).seats;
      } catch {
        continue;
      }
      for (const seat of seats) {
        if (this.host.objectUses.taken.has(seat.key)) {
          continue;
        }
        const far = Math.hypot(seat.seatAt.x - at.x, seat.seatAt.y - at.y);
        if (far <= REACH && (best === null || far < best.far)) {
          best = { object, seat, far };
        }
      }
    }
    return best;
  }

  // The player sits on (or lies on) the nearest placed seat. False when none is near and free, or
  // they sit already.
  use(player) {
    if (this.using.has(player) || player.knockedOut) {
      return false;
    }
    const found = this.nearest(player);
    if (!found) {
      return false;
    }
    const { object, seat } = found;
    const helper = this.helpers[seat.helperClass];
    // The side the player stands on: the approach helper nearest them.
    const at = player.position;
    let approach = helper.approaches[0];
    let stand = null;
    for (const candidate of helper.approaches) {
      const offset = this.rotate(seat.seatTurn, candidate.at);
      const point = {
        x: seat.seatAt.x + offset.x,
        y: seat.seatAt.y + offset.y,
        z: seat.seatAt.z + offset.z,
      };
      if (
        stand === null ||
        Math.hypot(point.x - at.x, point.y - at.y) < Math.hypot(stand.x - at.x, stand.y - at.y)
      ) {
        approach = candidate;
        stand = point;
      }
    }
    const facing = this.multiply(seat.seatTurn, this.quaternion(approach.turn));
    const start = this.downStart(approach.down, seat.seatAt, facing, stand) ?? {
      at: seat.seatAt,
      facing,
    };
    const leave = {
      x: seat.seatAt.x + (stand.x - seat.seatAt.x) * 0.45,
      y: seat.seatAt.y + (stand.y - seat.seatAt.y) * 0.45,
      z: seat.seatAt.z + (stand.z - seat.seatAt.z) * 0.45,
    };
    const idle = helper.idle[approach.name] ?? helper.idle.none ?? Object.values(helper.idle)[0];
    const [fragment, tags] = approach.down;
    player.position = start.at;
    player.rotation = start.facing;
    if (!player.playAnimation(fragment, { tags, lockMovement: true })) {
      return false;
    }
    this.host.objectUses.taken.set(seat.key, player);
    this.using.set(player, {
      object,
      place: seat.place,
      key: seat.key,
      approach,
      idle,
      leave,
      phase: 'sitting',
    });
    return true;
  }

  // The player stands up from their seat. False when they sit on none of these.
  stand(player) {
    const state = this.using.get(player);
    if (!state || state.phase === 'standing') {
      return false;
    }
    if (state.phase === 'sitting') {
      // Still sitting down: they stand where they are.
      this.end(player);
      player.stopAnimation({ standUp: true });
      return true;
    }
    state.phase = 'standing';
    const [fragment, tags] = state.approach.up;
    if (!player.playAnimation(fragment, { tags, lockMovement: true })) {
      this.end(player);
      player.stopAnimation({ standUp: true });
    }
    return true;
  }

  animated(player, reason) {
    const state = this.using.get(player);
    if (!state || reason === 'replaced') {
      return;
    }
    if (state.phase === 'sitting') {
      if (reason !== 'finished') {
        this.end(player);
        return;
      }
      state.phase = 'using';
      if (state.idle) {
        const [fragment, tags] = state.idle;
        player.playAnimation(fragment, { tags, loop: true, lockMovement: true });
      }
      this.host.emitToPlayerClient(player.id, EVENT, { op: 'seated' });
      return;
    }
    if (state.phase === 'using') {
      // The idle ended by something else (a script's animation, a hit): they are up.
      this.end(player);
      return;
    }
    if (state.phase === 'standing') {
      // The stand-up carried them out of the seat; one whose motion is not known is put out of it.
      if (!this.motion[state.approach.up.join('|')]) {
        player.position = state.leave;
      }
      this.end(player);
      player.stopAnimation();
    }
  }

  end(player) {
    const state = this.using.get(player);
    if (!state) {
      return;
    }
    this.using.delete(player);
    if (this.host.objectUses.taken.get(state.key) === player) {
      this.host.objectUses.taken.delete(state.key);
    }
    this.host.emitToPlayerClient(player.id, EVENT, { op: 'stood' });
  }

  // A placed object that goes stands its players up where they are.
  objectGone(object) {
    for (const [player, state] of [...this.using]) {
      if (state.object === object) {
        this.end(player);
        player.stopAnimation({ standUp: true });
      }
    }
  }

  // The player left: their seat is free.
  playerLeft(player) {
    const state = this.using.get(player);
    if (state) {
      this.using.delete(player);
      if (this.host.objectUses.taken.get(state.key) === player) {
        this.host.objectUses.taken.delete(state.key);
      }
    }
  }
}

module.exports = { PlayerSeats, SEAT_EVENT: EVENT };
