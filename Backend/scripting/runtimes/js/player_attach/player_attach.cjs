'use strict';

// One player attached to another (a KCDS addition): Player.attachTo, as a rider to a
// horse. Every game gets { op: 'playerAttach', id, target, right, forward, up, yaw } (and gets it
// again when a player's world gets ready): the player's own game keeps their body at that offset
// from the target's, and every game shows the body there, from where it shows the target. Ends
// with detach() and when either player leaves.
//
// The body always faces the way the target faces, turned by `rotation`. look 'free' (the default):
// the player's view and head still turn with the mouse. look 'follow': the view goes with the body.
//
// `animation` plays on the player and `otherAnimation` on the target, both as playAnimation's
// { fragment, ...its options }, a fragment-less one with `clip`. The target's is upper-body unless it
// says otherwise, so they can still walk. What attachTo started is stopped when the attachment ends.
//
// The server's own features also attach a player to a body that is no player (carry: a horse's
// passenger, horses/passengers.cjs): the order names that body's network id with body: true, and
// may carry the body from another offset in a time ({ from }), as a mount clip carries a rider.

// How far the offset may reach on each axis (m).
const MAX_OFFSET = 5;

// What attachTo plays on a body: [fragment, playAnimation's options], or null.
function animationOf(given, name, defaults) {
  if (given === undefined || given === null) {
    return null;
  }
  if (typeof given !== 'object') {
    throw new TypeError(`Player.attachTo ${name} takes { fragment, ...options }`);
  }
  const { fragment = '', ...options } = given;
  return [fragment, { ...defaults, ...options }];
}

function offsetAxis(value, name) {
  if (value === undefined) {
    return 0;
  }
  if (typeof value !== 'number' || !Number.isFinite(value) || Math.abs(value) > MAX_OFFSET) {
    throw new TypeError(`Player.attachTo offset.${name} takes a number up to ${MAX_OFFSET} m`);
  }
  return value;
}

class PlayerAttachments {
  constructor(host) {
    this.host = host;
    // player -> { target, body, right, forward, up, yaw, from }: target a Player, or body the
    // network id of another body (carry).
    this.attached = new WeakMap();
  }

  targetOf(player) {
    return this.attached.get(player)?.target ?? null;
  }

  // The network id of the body the player is carried on (carry), or 0.
  bodyOf(player) {
    return this.attached.get(player)?.body ?? 0;
  }

  // Carries the player on a body that is no player, by its network id: their game keeps their body
  // at `place` ({ right, forward, up, yaw }) from that body's. `from` ({ right, forward, up, yaw,
  // ms }) or null: the body starts there and moves to `place` in that time. Called again it moves
  // the body on from where it is. False for a player who left.
  carry(player, body, place, from = null) {
    if (this.host.players.get(player.id) !== player) {
      return false;
    }
    const known = this.attached.get(player);
    if (known && !known.body) {
      this.stopAnimations(player);
    }
    const order = {
      target: null,
      body,
      follow: false,
      right: place.right,
      forward: place.forward,
      up: place.up,
      yaw: place.yaw,
      playing: [],
    };
    this.attached.set(player, order);
    this.apply(player, from);
    return true;
  }

  // player.attachTo(other, { offset: { x right, y forward, z up }, rotation degrees }). False for a
  // player or target who left, the player themselves, and a target attached to this player.
  attach(player, other, options = {}) {
    const offset = options.offset ?? {};
    const rotation = options.rotation ?? 0;
    if (typeof rotation !== 'number' || !Number.isFinite(rotation)) {
      throw new TypeError('Player.attachTo rotation takes degrees');
    }
    const look = options.look ?? 'free';
    if (look !== 'free' && look !== 'follow') {
      throw new TypeError("Player.attachTo look takes 'free' or 'follow'");
    }
    const own = animationOf(options.animation, 'animation', { loop: true });
    const theirs = animationOf(options.otherAnimation, 'otherAnimation', {
      loop: true,
      upperBody: true,
    });
    const place = {
      follow: look === 'follow',
      right: offsetAxis(offset.x, 'x'),
      forward: offsetAxis(offset.y, 'y'),
      up: offsetAxis(offset.z, 'z'),
      yaw: rotation,
    };
    if (!other || typeof other.id !== 'number' || this.host.players.get(other.id) !== other) {
      return false;
    }
    if (this.host.players.get(player.id) !== player || other === player) {
      return false;
    }
    this.host.requireLivePlayerCall(player);
    for (let next = other; next; next = this.targetOf(next)) {
      if (next === player) {
        return false;
      }
    }
    this.stopAnimations(player);
    const carried = this.bodyOf(player);
    const order = { target: other, ...place, playing: [] };
    this.attached.set(player, order);
    if (carried) {
      this.host.horsePassengers?.dropped(player);
    }
    this.apply(player);
    if (own && player.playAnimation(own[0], own[1])) {
      order.playing.push(player);
    }
    if (theirs && other.playAnimation(theirs[0], theirs[1])) {
      order.playing.push(other);
    }
    return true;
  }

  // player.detach(): false when they were not attached.
  detach(player) {
    if (!this.attached.has(player)) {
      return false;
    }
    this.stopAnimations(player);
    const carried = this.bodyOf(player);
    this.attached.delete(player);
    this.apply(player);
    if (carried) {
      this.host.horsePassengers?.dropped(player);
    }
    return true;
  }

  // What the player's attachment started goes with it; a body that left has nothing to stop.
  stopAnimations(player) {
    const order = this.attached.get(player);
    for (const body of order?.playing ?? []) {
      if (this.host.players.get(body.id) === body) {
        body.stopAnimation();
      }
    }
  }

  // Tells every game; one still loading hears it when its world is ready (arrived). `from`: the
  // way a carried body moves now; an order told again later (arrived) is where the body ends.
  apply(player, from = null) {
    const order = this.attached.get(player);
    let target = 0;
    if (order) {
      target = order.target ? order.target.id : order.body;
    }
    const message = {
      op: 'playerAttach',
      id: player.id,
      target,
      follow: order ? order.follow : false,
      right: order ? order.right : 0,
      forward: order ? order.forward : 0,
      up: order ? order.up : 0,
      yaw: order ? order.yaw : 0,
    };
    if (order?.body) {
      message.body = true;
    }
    if (order && from) {
      message.from = { ...from };
    }
    this.host.bridge.send(message);
  }

  // A player's world is ready (again): their game starts without orders, so it hears them all.
  arrived() {
    for (const player of this.host.players.values()) {
      if (this.attached.has(player)) {
        this.apply(player);
      }
    }
  }

  // A player left: whoever was attached to them is free.
  left(leaving) {
    if (this.attached.has(leaving)) {
      this.detach(leaving);
    }
    for (const player of this.host.players.values()) {
      if (this.targetOf(player) === leaving) {
        this.detach(player);
      }
    }
  }
}

module.exports = { PlayerAttachments, MAX_OFFSET };
