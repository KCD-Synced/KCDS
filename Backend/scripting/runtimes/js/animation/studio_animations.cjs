'use strict';

// Animations made in the Asset/Animation Studio, by name: a resource defines one
// (Animations.define, as the studio's export does) and any resource plays it with
// player.playAnimation(name), as it plays one of the game's own; stopAnimation stops it. The
// body plays it as its pose animation (Player.setPoseAnimation, animation/pose_animation.cjs).
//
// Its effect events ({ time, kind: 'effect', name, joint?, scale? }) play on the
// player's body at their times while it plays, for the players who can see it (Vfx burstOnBody),
// again each loop. Sound events are the studio's own (the server plays no sounds) and are kept
// but not played here.

const { poseAnimationOf, MAX_BYTES, MAX_DEFINED_BYTES } = require('./pose_animation.cjs');
const { effectOf, jointOf } = require('../vfx/vfx_values.cjs');

const NAME = /^[A-Za-z0-9_]{1,48}$/;
const MOST_EVENTS = 32;
const SOUND_NAME = /^[A-Za-z0-9_]{1,96}$/;

// The animation's events, checked: effects by the Vfx rules (a TypeError or RangeError).
function eventsOf(given) {
  if (given === undefined) {
    return [];
  }
  if (!Array.isArray(given) || given.length > MOST_EVENTS) {
    throw new TypeError(`Animations.define: events is a list of at most ${MOST_EVENTS}`);
  }
  return given
    .map((event, index) => {
      const time = event?.time;
      if (typeof time !== 'number' || !Number.isFinite(time) || time < 0 || time > 60) {
        throw new RangeError(`Animations.define: event ${index} needs a time of 0 to 60 s`);
      }
      if (event.kind === 'sound') {
        if (typeof event.name !== 'string' || !SOUND_NAME.test(event.name)) {
          throw new TypeError(`Animations.define: event ${index} names a sound trigger`);
        }
        return { time, kind: 'sound', name: event.name };
      }
      if (event.kind !== 'effect') {
        throw new TypeError(`Animations.define: event ${index}'s kind is 'effect' or 'sound'`);
      }
      const scale = event.scale ?? 1;
      if (typeof scale !== 'number' || !(scale > 0) || scale > 32) {
        throw new RangeError(`Animations.define: event ${index}'s scale is above 0, up to 32`);
      }
      return {
        time,
        kind: 'effect',
        name: effectOf(event.name),
        joint: jointOf(event.joint),
        scale,
      };
    })
    .sort((a, b) => a.time - b.time);
}

class StudioAnimations {
  constructor() {
    // name -> the checked animation ({ keys, blend, loop }) and its events.
    this.byName = new Map();
    // The players playing one by name (so stopAnimation knows to stop it): player -> name.
    this.playing = new WeakMap();
    // The players held where they stand while theirs plays (playAnimation's lockMovement).
    this.held = new WeakSet();
    // The timers of the events a player's animation plays: player -> [timer].
    this.timers = new Map();
  }

  // A defined animation longer than the field takes (a baked game clip) goes to every
  // client once by its name (onLarge(name, field), '' when it goes) and plays by its name.
  define(name, animation) {
    if (typeof name !== 'string' || !NAME.test(name)) {
      throw new TypeError('Animations.define: the name is 1 to 48 letters, digits or _');
    }
    const { animation: checked, field } = poseAnimationOf(
      animation,
      'Animations.define',
      MAX_DEFINED_BYTES,
    );
    const events = eventsOf(animation.events);
    const large = Buffer.byteLength(field) > MAX_BYTES;
    const before = this.byName.get(name);
    this.byName.set(name, { animation: checked, events, field, large });
    if (large) {
      this.onLarge?.(name, field);
    } else if (before?.large) {
      this.onLarge?.(name, '');
    }
  }

  undefine(name) {
    const before = this.byName.get(name);
    if (before?.large) {
      this.onLarge?.(name, '');
    }
    return this.byName.delete(name);
  }

  // Whether it plays by its name (longer than the field takes).
  large(name) {
    return this.byName.get(name)?.large === true;
  }

  // The large ones' names and field texts, for a client whose resources start.
  largeOnes() {
    return [...this.byName.entries()]
      .filter(([, entry]) => entry.large)
      .map(([name, entry]) => ({ name, field: entry.field }));
  }

  has(name) {
    return this.byName.has(name);
  }

  // The animation, with loop as playAnimation's options give it (else as defined).
  animationFor(name, options) {
    const animation = structuredClone(this.byName.get(name).animation);
    if (typeof options?.loop === 'boolean') {
      animation.loop = options.loop;
    }
    return animation;
  }

  // Plays the animation's effects on the player at their times (again each loop) while the
  // player plays it; `alive()` says whether they still do.
  startEvents(host, player, name, loop, alive) {
    this.stopEvents(player);
    const { animation, events } = this.byName.get(name);
    const effects = events.filter((event) => event.kind === 'effect');
    if (effects.length === 0) {
      return;
    }
    const length = animation.keys.at(-1).time;
    const timers = [];
    const round = (start) => {
      for (const event of effects) {
        const timer = setTimeout(() => {
          if (alive()) {
            host.vfx.burstOnBody(event.name, player, event.joint, event.scale);
          }
        }, start + event.time * 1000);
        timer.unref?.();
        timers.push(timer);
      }
      if (loop && length > 0) {
        const next = setTimeout(() => {
          if (alive()) {
            timers.length = 0;
            round(0);
          }
        }, start + length * 1000);
        next.unref?.();
        timers.push(next);
      }
    };
    this.timers.set(player, timers);
    round(0);
  }

  stopEvents(player) {
    for (const timer of this.timers.get(player) ?? []) {
      clearTimeout(timer);
    }
    this.timers.delete(player);
  }

  api() {
    return Object.freeze({
      define: (name, animation) => this.define(name, animation),
      undefine: (name) => this.undefine(name),
      defined: () => [...this.byName.keys()].sort(),
    });
  }
}

module.exports = { StudioAnimations };
