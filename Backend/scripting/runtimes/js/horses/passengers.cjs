'use strict';

// A passenger behind a horse's rider (a KCDS addition). The game has one rider per horse; KCDS
// seats one more player on its back, behind the saddle, with the game's own clips.
//
// A player on foot who aims at a horse someone rides gets the game's own hint, Ride behind, on the
// use key. They climb on with the game's mount clip from the horse's left side (Mount
// player+sideLeft) and sit with the rider's own pose (MotionIdle horse+player); getting off plays
// its dismount clip (Dismount player+sideLeft). The body hangs on the horse on every game
// (player_attach carry: the horse's network id, kAttachToBody), placed from where each game shows
// the horse, and the clips' way between the ground and the seat is the order's moving offset, so
// the climb follows the horse even when it shifts. The seat is where the rider sits, moved back
// along the horse. Men and women alike: a woman player's body plays the male clips
// (appearance/female_skeleton.hpp).
//
// The passenger gets off with the use key aimed at their rider (Get off), when the rider gets off
// or leaves, and at once when the horse goes. Horse.seatPassenger puts a player on at once, without
// the climb; horsePassengerBoarding may refuse a player's own climb on.

// Ride behind shows within this reach of the horse (metres), and a player this far away or farther
// is not taken on.
const BOARD_REACH = 2.5;
const BOARD_LIMIT = 3.5;
// Get off shows on the rider's body within this reach of the passenger.
const GET_OFF_REACH = 2.0;
const HINTS = Object.freeze({ board: 'Ride behind', getOff: 'Get off' });

// Behind the rider: how far back along the horse, and how much higher, in metres. The rider's
// pose sits in the saddle's seat; behind the saddle the horse's back is that much higher (seen
// live: at the rider's own height the passenger's hips sank into it, 15 cm higher they sat on air).
// Closer than this, the passenger's own camera came inside the rider's back as the rider leant
// with the gait, and the rider flickered on their screen (seen live at 0.32, and still at 0.42 in
// a sprint, braking and in turns).
const BEHIND_RIDER = 0.55;
const ABOVE_RIDER = 0.06;
// Where a rider sits on a horse when the rider's own place says nothing better: right, forward
// and up of the horse's root, metres. A rider's reported place outside these bounds is not taken.
const DEFAULT_RIDER_SEAT = Object.freeze({ right: 0, forward: -0.05, up: 1.5 });
const RIDER_SEAT_BOUNDS = Object.freeze({
  right: 0.5,
  forwardMin: -1,
  forwardMax: 1,
  upMin: 1,
  upMax: 2.2,
});

// The game's clips of getting on a horse and off it from its left side, and how each moves the body
// (Libs/Tables/action/actor_anim_action.xml, relative_movement): metres right, ahead and up of where
// the clip starts, how far it turns the body to the left (degrees) and how long it takes (ms).
const MOUNT = Object.freeze({
  fragment: 'Mount',
  tags: 'player+sideLeft',
  right: 0.089065,
  ahead: 0.877656,
  up: 1.514537,
  turn: (2 * Math.atan2(0.575339, 0.817915) * 180) / Math.PI,
  ms: 2600,
});
const DISMOUNT = Object.freeze({
  fragment: 'Dismount',
  tags: 'player+sideLeft',
  right: -0.793705,
  ahead: -0.244066,
  up: -1.542184,
  turn: (2 * Math.atan2(-0.580513, 0.814251) * 180) / Math.PI,
  ms: 3133,
});
// The rider's own sitting pose on a horse.
const SEATED = Object.freeze({ fragment: 'MotionIdle', tags: 'horse+player' });
// A clip that never reports its end still ends this much later (ms).
const CLIP_SLACK_MS = 1500;
// After a new look, the player's game has their new body by then (ms).
const NEW_BODY_MS = 1500;

const DEGREES = Math.PI / 180;

// The heading of a rotation about the up axis (radians, to the left).
function headingOf(rotation) {
  if (!rotation) {
    return 0;
  }
  const { x = 0, y = 0, z = 0, w = 1 } = rotation;
  return Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));
}

// A world position seen from a horse: metres right, forward and up of its root.
function horseLocal(horse, position) {
  const at = horse.position;
  const heading = headingOf(horse.rotation);
  const dx = position.x - at.x;
  const dy = position.y - at.y;
  const c = Math.cos(heading);
  const s = Math.sin(heading);
  return { right: dx * c + dy * s, forward: -dx * s + dy * c, up: position.z - at.z };
}

// The world position of a place on a horse.
function horseWorld(horse, place) {
  const at = horse.position;
  const heading = headingOf(horse.rotation);
  const c = Math.cos(heading);
  const s = Math.sin(heading);
  return {
    x: at.x + place.right * c - place.forward * s,
    y: at.y + place.right * s + place.forward * c,
    z: at.z + place.up,
  };
}

// Where the rider sits on the horse, from the rider's reported place when it is a seat's.
function riderSeat(horse, rider) {
  if (!rider?.position || !horse.position) {
    return { ...DEFAULT_RIDER_SEAT };
  }
  const local = horseLocal(horse, rider.position);
  const bounds = RIDER_SEAT_BOUNDS;
  const sits =
    Math.abs(local.right) <= bounds.right &&
    local.forward >= bounds.forwardMin &&
    local.forward <= bounds.forwardMax &&
    local.up >= bounds.upMin &&
    local.up <= bounds.upMax;
  return sits ? local : { ...DEFAULT_RIDER_SEAT };
}

// The passenger's seat on the horse: behind its rider.
function passengerSeat(horse, rider) {
  const seat = riderSeat(horse, rider);
  return {
    right: seat.right,
    forward: seat.forward - BEHIND_RIDER,
    up: seat.up + ABOVE_RIDER,
    yaw: 0,
  };
}

// The place a clip moves a body to from `start` ({ right, forward, up, yaw } on the horse).
function afterClip(start, clip) {
  const heading = start.yaw * DEGREES;
  const c = Math.cos(heading);
  const s = Math.sin(heading);
  return {
    right: start.right + clip.right * c - clip.ahead * s,
    forward: start.forward + clip.right * s + clip.ahead * c,
    up: start.up + clip.up,
    yaw: start.yaw + clip.turn,
  };
}

// The place a clip starts at to end at `end`.
function beforeClip(end, clip) {
  const yaw = end.yaw - clip.turn;
  const heading = yaw * DEGREES;
  const c = Math.cos(heading);
  const s = Math.sin(heading);
  return {
    right: end.right - (clip.right * c - clip.ahead * s),
    forward: end.forward - (clip.right * s + clip.ahead * c),
    up: end.up - clip.up,
    yaw,
  };
}

// A place beside the horse on the ground it stands on (its root's height): the clips start and end
// there, whatever height the seat has on this horse.
function onGround(place) {
  return { ...place, up: 0 };
}

class HorsePassengers {
  constructor(host) {
    this.host = host;
    // horse -> { player, seat, state: 'climbing' | 'seated' | 'leaving', request, getOff }
    this.seats = new Map();
    // player id -> horse
    this.byPlayer = new Map();
    // horse -> the Ride behind interaction
    this.boarding = new Map();
    // horses whose passengers their scripts forbid (passengerAllowed = false)
    this.closed = new WeakSet();
  }

  passengerOf(horse) {
    return this.seats.get(horse)?.player ?? null;
  }

  horseOf(player) {
    return this.byPlayer.get(player?.id) ?? null;
  }

  allowed(horse) {
    return !this.closed.has(horse);
  }

  setAllowed(horse, allowed) {
    if (typeof allowed !== 'boolean') {
      throw new TypeError('Horse.passengerAllowed takes a boolean');
    }
    if (allowed) {
      this.closed.delete(horse);
    } else {
      this.closed.add(horse);
    }
    this.updateHints(horse);
  }

  riderOf(horse) {
    return horse.riderId ? this.host.players.get(horse.riderId) ?? null : null;
  }

  // Whether the player rides a horse themselves.
  riding(player) {
    for (const horse of this.host.horses.values()) {
      if (horse.riderId === player.id) {
        return true;
      }
    }
    return false;
  }

  // Whether the player can be put on the horse now: there, free, not riding or carried already.
  canTake(horse, player) {
    if (!this.host.horses.has(horse.id) || this.seats.has(horse)) {
      return false;
    }
    if (this.host.players.get(player?.id) !== player || !player.ready || !player.position) {
      return false;
    }
    if (horse.riderId === player.id || this.byPlayer.has(player.id) || this.riding(player)) {
      return false;
    }
    if (this.host.playerAttachments.attached.has(player)) {
      return false;
    }
    return Boolean(horse.position);
  }

  // A player's use of Ride behind.
  boardRequested(horse, player) {
    if (!this.riderOf(horse) || !this.allowed(horse) || !this.canTake(horse, player)) {
      return false;
    }
    if (horse.alive === false) {
      return false;
    }
    const at = player.position;
    const horseAt = horse.position;
    if (Math.hypot(at.x - horseAt.x, at.y - horseAt.y, at.z - horseAt.z) > BOARD_LIMIT) {
      return false;
    }
    if (!this.host.emit('horsePassengerBoarding', horse, player)) {
      return false;
    }
    return this.seat(horse, player, true);
  }

  // Puts the player on the horse behind its rider: with the game's mount clip (`climb`), or at once.
  seat(horse, player, climb) {
    if (!this.canTake(horse, player)) {
      return false;
    }
    const seat = passengerSeat(horse, this.riderOf(horse));
    const entry = {
      player,
      seat,
      state: climb ? 'climbing' : 'seated',
      request: null,
      getOff: null,
    };
    this.seats.set(horse, entry);
    this.byPlayer.set(player.id, horse);
    const animations = this.host.playerAnimations;
    const climbing =
      climb &&
      player.playAnimation(MOUNT.fragment, { tags: MOUNT.tags, loop: false, lockMovement: true });
    if (climbing) {
      const start = onGround(beforeClip(seat, MOUNT));
      this.host.playerAttachments.carry(player, horse.id, seat, { ...start, ms: MOUNT.ms });
      const request = animations.stateOf(player).current;
      entry.request = request;
      const settled = () => {
        if (
          this.seats.get(horse) === entry &&
          entry.state === 'climbing' &&
          entry.request === request
        ) {
          this.settle(horse, entry);
        }
      };
      if (request) {
        request.onEnd = settled;
      }
      setTimeout(settled, MOUNT.ms + CLIP_SLACK_MS).unref?.();
    } else {
      this.host.playerAttachments.carry(player, horse.id, seat, null);
      this.settle(horse, entry);
    }
    this.updateHints(horse);
    return true;
  }

  // On the seat: the rider's sitting pose, and Get off on the rider.
  settle(horse, entry) {
    entry.state = 'seated';
    const player = entry.player;
    if (
      player.playAnimation(SEATED.fragment, { tags: SEATED.tags, loop: true, lockMovement: true })
    ) {
      entry.request = this.host.playerAnimations.stateOf(player).current;
    } else {
      entry.request = null;
    }
    this.updateHints(horse);
    this.host.emit('horsePassengerBoarded', horse, player);
  }

  // The passenger gets off: with the game's dismount clip (`climb`), or at once beside the horse.
  unseat(horse, climb) {
    const entry = this.seats.get(horse);
    if (!entry || entry.state === 'leaving') {
      return false;
    }
    const player = entry.player;
    const connected = this.host.players.get(player.id) === player;
    const climbing =
      climb &&
      connected &&
      this.host.horses.has(horse.id) &&
      player.playAnimation(DISMOUNT.fragment, {
        tags: DISMOUNT.tags,
        loop: false,
        lockMovement: true,
      });
    if (!climbing) {
      this.release(horse, entry, connected);
      return true;
    }
    entry.state = 'leaving';
    const end = onGround(afterClip(entry.seat, DISMOUNT));
    this.host.playerAttachments.carry(player, horse.id, end, { ...entry.seat, ms: DISMOUNT.ms });
    const request = this.host.playerAnimations.stateOf(player).current;
    entry.request = request;
    const down = () => {
      if (this.seats.get(horse) === entry && entry.request === request) {
        this.release(horse, entry, this.host.players.get(player.id) === player);
      }
    };
    if (request) {
      request.onEnd = down;
    }
    setTimeout(down, DISMOUNT.ms + CLIP_SLACK_MS).unref?.();
    this.updateHints(horse);
    return true;
  }

  // The passenger is off: their body their own again, where the clip left it or beside the horse.
  release(horse, entry, connected) {
    const player = entry.player;
    this.seats.delete(horse);
    this.byPlayer.delete(player.id);
    entry.getOff?.remove();
    entry.getOff = null;
    if (connected) {
      // A body the dismount clip carried down, or one a resource took elsewhere, stays where it is.
      const placed = entry.state === 'leaving' || entry.state === 'dropped';
      if (this.host.playerAttachments.bodyOf(player) === horse.id) {
        this.host.playerAttachments.detach(player);
      }
      const animations = this.host.playerAnimations;
      if (entry.request && animations.stateOf(player).current === entry.request) {
        player.stopAnimation();
      }
      // Off at once: on the ground where the dismount clip would have left them.
      if (!placed && horse.position) {
        player.teleport(horseWorld(horse, onGround(afterClip(entry.seat, DISMOUNT))));
      }
    }
    this.updateHints(horse);
    this.host.emit('horsePassengerLeft', horse, player);
  }

  // The hints as the horse stands now: Ride behind while someone rides it and its seat is free, to
  // everyone but its rider; Get off on the rider for the seated passenger.
  updateHints(horse) {
    const rider = this.riderOf(horse);
    const entry = this.seats.get(horse);
    let board = this.boarding.get(horse);
    const offered =
      Boolean(rider) &&
      !entry &&
      this.allowed(horse) &&
      this.host.horses.has(horse.id) &&
      horse.alive !== false;
    if (offered && !board) {
      board = this.host.worldInteractions.add(
        { entity: horse, radius: BOARD_REACH, text: HINTS.board },
        (player) => {
          this.boardRequested(horse, player);
        },
      );
      this.boarding.set(horse, board);
    }
    if (board) {
      board.setEnabled(offered);
      this.host.worldInteractions.hideFrom(board, rider ? [rider.id] : []);
    }
    if (!entry) {
      return;
    }
    const seated = entry.state === 'seated' && rider;
    if (entry.getOff && (!seated || entry.getOffRider !== rider)) {
      entry.getOff.remove();
      entry.getOff = null;
    }
    if (seated && !entry.getOff) {
      entry.getOff = this.host.worldInteractions.add(
        { entity: rider, radius: GET_OFF_REACH, text: HINTS.getOff },
        (player) => {
          if (player === entry.player) {
            this.unseat(horse, true);
          }
        },
      );
      entry.getOff.setPlayers([entry.player]);
      entry.getOffRider = rider;
    }
  }

  // The horse's rider changed: a passenger whose rider is gone gets off.
  riderChanged(horse) {
    if (this.seats.has(horse) && !this.riderOf(horse)) {
      this.unseat(horse, true);
      return;
    }
    this.updateHints(horse);
  }

  // The horse is gone: its passenger is off at once, its hints with it.
  destroyed(horse) {
    if (this.seats.has(horse)) {
      this.unseat(horse, false);
    }
    this.boarding.get(horse)?.remove();
    this.boarding.delete(horse);
  }

  // A player left: off the horse they sat on, and their rider's horse keeps its hints right.
  left(player) {
    const horse = this.byPlayer.get(player.id);
    if (horse) {
      const entry = this.seats.get(horse);
      if (entry) {
        this.release(horse, entry, false);
      }
    }
    // A rider who left takes the passenger's ride with them.
    for (const [riding, entry] of [...this.seats]) {
      if (riding.riderId === player.id || entry.getOffRider === player) {
        this.unseat(riding, true);
      }
    }
  }

  // The player's look changed: their game made their body anew, without the sitting pose, so it
  // sits again once the new body is there.
  appearanceChanged(player) {
    const horse = this.byPlayer.get(player.id);
    const entry = horse ? this.seats.get(horse) : null;
    if (!entry || entry.state !== 'seated') {
      return;
    }
    const again = () => {
      if (this.seats.get(horse) !== entry || entry.state !== 'seated') {
        return;
      }
      if (this.host.players.get(player.id) !== player) {
        return;
      }
      const seated = player.playAnimation(SEATED.fragment, {
        tags: SEATED.tags,
        loop: true,
        lockMovement: true,
      });
      entry.request = seated ? this.host.playerAnimations.stateOf(player).current : null;
    };
    setTimeout(again, NEW_BODY_MS).unref?.();
  }

  // The player's carry ended without us (a resource attached them elsewhere or detached them):
  // they are no passenger any more.
  dropped(player) {
    const horse = this.byPlayer.get(player.id);
    const entry = horse ? this.seats.get(horse) : null;
    if (entry) {
      entry.state = 'dropped';
      this.release(horse, entry, true);
    }
  }
}

module.exports = {
  HorsePassengers,
  passengerSeat,
  afterClip,
  beforeClip,
  horseLocal,
  horseWorld,
  MOUNT,
  DISMOUNT,
  SEATED,
  BEHIND_RIDER,
  ABOVE_RIDER,
};
