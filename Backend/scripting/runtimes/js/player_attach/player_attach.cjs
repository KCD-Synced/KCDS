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
    // player -> { target, right, forward, up, yaw }
    this.attached = new WeakMap();
  }

  targetOf(player) {
    return this.attached.get(player)?.target ?? null;
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
    const order = { target: other, ...place, playing: [] };
    this.attached.set(player, order);
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
    this.attached.delete(player);
    this.apply(player);
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

  // Tells every game; one still loading hears it when its world is ready (arrived).
  apply(player) {
    const order = this.attached.get(player);
    this.host.bridge.send({
      op: 'playerAttach',
      id: player.id,
      target: order ? order.target.id : 0,
      follow: order ? order.follow : false,
      right: order ? order.right : 0,
      forward: order ? order.forward : 0,
      up: order ? order.up : 0,
      yaw: order ? order.yaw : 0,
    });
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
