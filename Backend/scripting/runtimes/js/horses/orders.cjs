'use strict';

// The orders a resource gives a horse: moveTo, patrol and hold. An order goes to the
// native server as the horse's `intent` text (horse.hpp HorseIntent), which every client
// receives: "<sequence>|<kind>|<arguments>", written as the NPC orders are (npc.hpp NpcIntent),
// the sequence one up per order so that a client takes each only once.
//
//   moveTo  <seq>|moveTo|<x>|<y>|<z>|<speed>|<radius>|<pathfinding>
//   patrol  <seq>|patrol|<speed>|<mode>|<radius>|<waitSeconds>|<pathfinding>|<points>
//           (the points "x,y,z" joined by ';')
//   hold    the intent field empties
//   follow  is the server's: four times a second, a horse further than its
//           radius from where its target will be a second on gets a moveTo to the target's side
//           of that, at a pace by the distance and the target's speed unless the script chose
//           one, so it keeps walking behind a led horse's leader.
//
// Horses take no pathfinding option: their routes are the server navigation mesh's, so the text
// always says `server`. The simulating client carries the order out with the game's own movement;
// until a client reports progress, `travelStatus` reads `running` from the moment an order
// is given and `idle` after a hold. Mounting cancels an order.

const {
  PATROL_POINTS_MAX,
  SPEEDS,
  choiceOf,
  radiusOf,
  moveOptionsOf,
  patrolOptionsOf,
  placeOf,
  decimal,
} = require('../npcs/npc_values.cjs');

// A followed target's distance kept by default, how often the follows are looked at, how far the
// target moves before the order is given again, and the distances past which a horse whose
// script chose no pace jogs and runs (metres, ms). A follow looked at once a second, aimed at
// where the target stood, walked a metre, stopped and waited behind a walking player (a player
// leading a horse on a rope): it is looked at four times a second and aims where the
// target will be FOLLOW_LEAD_SECONDS on (at most FOLLOW_LEAD_MAX metres ahead), so the walk goes
// on; a target moving faster than FOLLOW_JOG_SPEED or FOLLOW_RUN_SPEED (m/s) is jogged or run
// after whatever the distance.
const FOLLOW_RADIUS = 3;
// Where on the horse a lead rope is tied (follow rope: { at }): the bit by default, or a named
// place of the game's horse skeleton, or a joint's own name.
const ROPE_PLACES = Object.freeze({ bit: '', neck: 'Neck5', head: 'Head' });

// The joint follow's `rope` asks for: '' for the bit (true, or { at: 'bit' }). Throws for
// anything else.
function ropeAtOf(rope) {
  if (typeof rope === 'boolean') {
    return '';
  }
  const at = rope && typeof rope === 'object' ? rope.at ?? 'bit' : undefined;
  const joint = Object.hasOwn(ROPE_PLACES, at) ? ROPE_PLACES[at] : at;
  if (
    typeof joint !== 'string' ||
    joint.length > 48 ||
    joint.includes('|') ||
    !/^[\x20-\x7e]*$/.test(joint)
  ) {
    throw new TypeError(
      "follow rope must be true, false or { at: 'bit' | 'neck' | 'head' | joint }",
    );
  }
  return joint;
}
const FOLLOW_TICK_MS = 250;
const FOLLOW_MOVED = 0.5;
const FOLLOW_JOG = 8;
const FOLLOW_RUN = 20;
const FOLLOW_LEAD_SECONDS = 1;
const FOLLOW_LEAD_MAX = 6;
const FOLLOW_JOG_SPEED = 2.2;
const FOLLOW_RUN_SPEED = 4.5;
// Samples closer together than this give no speed (seconds).
const FOLLOW_MIN_SAMPLE = 0.05;

// What horseIntentDone can say of an order that ended.
const FINISHED_STATUSES = ['reached', 'blocked', 'failed'];
// The pathfinding word of every horse order.
const PATHFINDING = 'server';

// The coordinates of a place as the plain decimal texts of an intent.
function coordinatesOf(place) {
  return [decimal(place.x), decimal(place.y), decimal(place.z)];
}

class HorseOrders {
  constructor(host, state) {
    this.host = host;
    // The symbol under which a horse keeps { sequence, status }.
    this.state = state;
    // horse -> { target, radius, speed, aimed } of Horse.follow, and the one timer over them.
    this.follows = new Map();
    this.followTimer = null;
  }

  // The state of a freshly spawned horse: no order yet.
  fresh() {
    return { sequence: 0, status: 'idle' };
  }

  // The options object of an order; {} when left out. Throws for anything but an object.
  optionsOf(options, what) {
    if (options === undefined) {
      return {};
    }
    if (options === null || typeof options !== 'object') {
      throw new TypeError(`${what} options must be an object`);
    }
    return options;
  }

  // Whether the horse is still here; throws when the calling resource has been stopped.
  live(horse) {
    if (this.host.horses.get(horse.id) !== horse) {
      return false;
    }
    this.host.requireLivePlayerCall(horse);
    return true;
  }

  // Whether an order can go to the horse: it is here, alive and standing (not knocked out) and
  // nobody sits in its saddle.
  orderable(horse) {
    return (
      this.host.horses.get(horse.id) === horse &&
      horse.alive !== false &&
      !horse.knockedOut &&
      !horse.mounted
    );
  }

  // Sends "<sequence>|<kind>|<arguments>" and marks the horse as running it.
  give(horse, kind, args) {
    const state = horse[this.state];
    state.sequence += 1;
    state.status = 'running';
    const intent = [state.sequence, kind, ...args].join('|');
    this.host.bridge.send({ op: 'horseIntent', id: horse.id, intent });
    return true;
  }

  // Horse.follow(target, { radius, speed, rope }): follows a Player, an Npc or another Horse until hold,
  // another order, or the target is gone. True once it follows; false for a horse that is gone,
  // dead or ridden, a target with no position, or itself. Throws for an option it cannot use.
  follow(horse, target, options) {
    const given = this.optionsOf(options, 'follow');
    const radius = radiusOf(given.radius, FOLLOW_RADIUS, 'follow');
    const speed =
      given.speed === undefined ? null : choiceOf(given.speed, SPEEDS, 'walk', 'follow speed');
    const rope = given.rope ?? false;
    const ropeAt = ropeAtOf(rope);
    const at = target?.position;
    const placed = at && [at.x, at.y, at.z].every(Number.isFinite);
    if (!this.live(horse) || !target || target === horse || !placed) {
      return false;
    }
    if (!this.orderable(horse)) {
      return false;
    }
    this.stopFollow(horse);
    this.follows.set(horse, { target, radius, speed, rope: rope !== false, aimed: null });
    // A rope from its head to the leader's right hand on every screen (horse.hpp HorseLead).
    if (rope !== false) {
      this.host.bridge.send({ op: 'horseLead', id: horse.id, leader: target.id, at: ropeAt });
    }
    this.step(horse);
    this.armFollow();
    return true;
  }

  // Ends a follow, and takes its rope away.
  stopFollow(horse) {
    const follow = this.follows.get(horse);
    if (!follow) {
      return;
    }
    this.follows.delete(horse);
    if (follow.rope && this.host.horses.get(horse.id) === horse) {
      this.host.bridge.send({ op: 'horseLead', id: horse.id, leader: 0 });
    }
  }

  // Whether the horse follows its target on a rope.
  roped(horse) {
    return this.follows.get(horse)?.rope === true;
  }

  // The target the horse follows, or null.
  followed(horse) {
    return this.follows.get(horse)?.target ?? null;
  }

  // Whether a followed target is still in the world.
  present(target) {
    const host = this.host;
    return (
      host.players.get(target.id) === target ||
      host.horses.get(target.id) === target ||
      host.npcs?.npcs?.get(target.id) === target
    );
  }

  armFollow() {
    if (this.followTimer || this.follows.size === 0) {
      return;
    }
    this.followTimer = setInterval(() => this.stepAll(), FOLLOW_TICK_MS);
    this.followTimer.unref?.();
  }

  stepAll() {
    for (const horse of [...this.follows.keys()]) {
      this.step(horse);
    }
    if (this.follows.size === 0 && this.followTimer) {
      clearInterval(this.followTimer);
      this.followTimer = null;
    }
  }

  // The target's ground speed (m/s, x and y) from its last two looks, 0 for the first.
  targetVelocity(follow, to) {
    const now = Date.now() / 1000;
    const last = follow.sample;
    follow.sample = { x: to.x, y: to.y, at: now };
    if (!last || now - last.at < FOLLOW_MIN_SAMPLE) {
      return { x: 0, y: 0 };
    }
    const seconds = now - last.at;
    return { x: (to.x - last.x) / seconds, y: (to.y - last.y) / seconds };
  }

  // One look at a follow: a moveTo to `radius` short of where the target will be a moment on, when
  // that is further than `radius` and the target has moved since the last order; nothing while it
  // is near or the horse is ridden.
  step(horse) {
    const follow = this.follows.get(horse);
    if (!follow) {
      return;
    }
    if (this.host.horses.get(horse.id) !== horse || !this.present(follow.target)) {
      this.stopFollow(horse);
      return;
    }
    const from = horse.position;
    const to = follow.target.position;
    if (!this.orderable(horse) || !from || !to) {
      return;
    }
    const velocity = this.targetVelocity(follow, to);
    const targetSpeed = Math.hypot(velocity.x, velocity.y);
    const lead = Math.min(targetSpeed * FOLLOW_LEAD_SECONDS, FOLLOW_LEAD_MAX);
    const ahead = {
      x: to.x + (targetSpeed > 0 ? (velocity.x / targetSpeed) * lead : 0),
      y: to.y + (targetSpeed > 0 ? (velocity.y / targetSpeed) * lead : 0),
    };
    const distance = Math.hypot(ahead.x - from.x, ahead.y - from.y);
    const aimed = follow.aimed;
    const moved = aimed ? Math.hypot(to.x - aimed.x, to.y - aimed.y) : Infinity;
    if (distance <= follow.radius || moved < FOLLOW_MOVED) {
      return;
    }
    const share = (distance - follow.radius) / distance;
    const place = {
      x: from.x + (ahead.x - from.x) * share,
      y: from.y + (ahead.y - from.y) * share,
      z: to.z,
    };
    let speed = follow.speed;
    if (speed === null) {
      if (distance > FOLLOW_RUN || targetSpeed > FOLLOW_RUN_SPEED) {
        speed = 'run';
      } else if (distance > FOLLOW_JOG || targetSpeed > FOLLOW_JOG_SPEED) {
        speed = 'jog';
      } else {
        speed = 'walk';
      }
    }
    follow.aimed = { x: to.x, y: to.y };
    const args = [...coordinatesOf(place), speed, decimal(1), PATHFINDING];
    this.give(horse, 'moveTo', args);
  }

  // Horse.moveTo(position, { speed, radius }): true once sent; false if the horse is
  // missing, dead or ridden, or the destination is invalid. Throws for an option it cannot
  // use.
  moveTo(horse, position, options) {
    this.stopFollow(horse);
    const given = this.optionsOf(options, 'moveTo');
    const { speed, radius } = moveOptionsOf({ speed: given.speed, radius: given.radius }, 'moveTo');
    if (!this.live(horse)) {
      return false;
    }
    const place = placeOf(position, horse.position);
    if (place === null || !this.orderable(horse)) {
      return false;
    }
    const args = [...coordinatesOf(place), speed, decimal(radius), PATHFINDING];
    return this.give(horse, 'moveTo', args);
  }

  // Horse.patrol(points, { speed, loop, waitSeconds }): one to 64 waypoints. True when the order
  // went out; false for an empty or long route, a waypoint or waitSeconds it cannot use, or a
  // horse that is gone, dead or ridden. Throws for an option it cannot use.
  patrol(horse, points, options) {
    if (!Array.isArray(points)) {
      throw new TypeError('patrol takes an array of positions');
    }
    this.stopFollow(horse);
    const given = this.optionsOf(options, 'patrol');
    const { speed, mode, radius, waitSeconds } = patrolOptionsOf({
      speed: given.speed,
      loop: given.loop,
      waitSeconds: given.waitSeconds,
    });
    if (!this.live(horse)) {
      return false;
    }
    if (points.length === 0 || points.length > PATROL_POINTS_MAX || waitSeconds === null) {
      return false;
    }
    const places = points.map((point) => placeOf(point, horse.position));
    if (places.includes(null) || !this.orderable(horse)) {
      return false;
    }
    const route = places.map((place) => coordinatesOf(place).join(',')).join(';');
    const args = [speed, mode, decimal(radius), decimal(waitSeconds), PATHFINDING, route];
    return this.give(horse, 'patrol', args);
  }

  // Horse.hold: drops the order, which empties the intent field. A report of the earlier order is
  // stale from now on. Does nothing for a horse that is gone.
  hold(horse) {
    if (!this.host.horses.has(horse.id)) {
      return;
    }
    this.host.requireLivePlayerCall(horse);
    this.stopFollow(horse);
    const state = horse[this.state];
    state.sequence += 1;
    state.status = 'idle';
    this.host.bridge.send({ op: 'horseIntent', id: horse.id, intent: '' });
  }

  // A rider took the saddle: a running order ends there.
  mounted(horse) {
    if (horse[this.state].status === 'running') {
      // A follow waits while the horse is ridden and goes on after.
      const follow = this.follows.get(horse);
      if (follow) {
        // Its intent goes, the follow (and its rope) stay.
        follow.aimed = null;
        this.follows.delete(horse);
        this.hold(horse);
        this.follows.set(horse, follow);
      } else {
        this.hold(horse);
      }
    }
  }

  // The simulator's report that the horse's order ended: { id, status, sequence? } with a status
  // of reached, blocked or failed; a report that names an earlier order's sequence is stale and
  // dropped. Raises horseIntentDone(horse, status). The simulator sends it when a moveTo's walk
  // or a once-patrol's last leg ends (NPC_INTENT_DONE, which carries horses too).
  intentDone(report) {
    const horse = this.host.horses.get(report.id);
    if (!horse || !FINISHED_STATUSES.includes(report.status)) {
      return;
    }
    const state = horse[this.state];
    if (report.sequence !== undefined && report.sequence !== state.sequence) {
      return;
    }
    state.status = report.status;
    // A follow's steps end quietly; it goes on.
    if (this.follows.has(horse)) {
      return;
    }
    this.host.emit('horseIntentDone', horse, report.status);
  }
}

module.exports = { HorseOrders };
