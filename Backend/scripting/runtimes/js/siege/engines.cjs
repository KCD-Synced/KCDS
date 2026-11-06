'use strict';

// Siege engines the server puts in the world: the server API's SiegeEngine, a
// trebuchet or a cannon. The server owns the cycle (idle, drawing, loading, ready, firing), the
// operator, the ammunition, the health and the outcome of every shot; the clients only draw it and
// report where a stone came down.
//
// Everything is sent only when something changes. Server to client, broadcast (no `id`); a client
// keeps an engine only for players in its `dimension`:
//   { op: 'siegeSpawn', id, kind, position, rotation, dimension, ...state }
//   { op: 'siegeUpdate', id, cycle, seconds, speed, rotation, aimGuide, target, wrecked, usable,
//     operator }       state: `seconds` is how long the cycle step just entered takes, `operator`
//                      is a player id or 0, `target` the point the guide points at ({0,0,0} when
//                      none) and `rotation` a quaternion { x, y, z, w }
//   { op: 'siegeShot', id, shot, from, target, flight, apex, reporter, dimension }
//   { op: 'siegeDestroy', id }
// Client to server:
//   { op: 'siegeImpactReport', id (player), engine, shot, position }   from `reporter` only
//   { op: 'siegeUse', id (player), engine, action, look? }   action: operate, leave, load, fire
//
// Timers: one per engine for the step it is in (load phases, the release of a shot), and one per
// shot in the air that only fires when its reporter stays silent.

const {
  KINDS,
  arcOf,
  facingOf,
  horizontal,
  onArc,
  rangeProblem,
  shareOf,
} = require('./ballistics.cjs');
const { OWNER, runningResource, removeOwned } = require('../lifecycle/owned_objects.cjs');
const { Quaternion, Vector3, rotation, vector } = require('../math.cjs');

const GLOBAL_WORLD = 0xffffffff;
const WORLD_LIMIT = 1e6;
const SPEED_MIN = 0.1;
const SPEED_MAX = 20;
const RADIUS_MAX = 100;
const DEFAULT_RADIUS = 10;
// A player this close to an empty engine may take its place.
const USE_DISTANCE = 3;
// A stone is reported by the attacker within this many metres of the target, else by the nearest
// player within VIEW_DISTANCE of it.
const REPORT_DISTANCE = 200;
const VIEW_DISTANCE = 300;
// A reporter that stays silent this long after the stone should have landed is given up on.
const REPORT_GRACE_MS = 3000;
const ACTIONS = new Set(['operate', 'leave', 'load', 'fire']);

function validWorld(world) {
  return Number.isSafeInteger(world) && world >= 0 && world <= GLOBAL_WORLD;
}

function isPoint(point) {
  return [point.x, point.y, point.z].every((n) => Number.isFinite(n) && Math.abs(n) <= WORLD_LIMIT);
}

function distance3(a, b) {
  return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

function refused(reason) {
  return { accepted: false, reason, seconds: 0 };
}

function accepted(seconds) {
  return { accepted: true, reason: '', seconds };
}

class SiegeEngines {
  // `host`: the resource host (bridge, emit, allocateEntityId, players, npcs).
  constructor(host) {
    this.host = host;
    // id -> SiegeEngine
    this.engines = new Map();
    // shot number -> the shot in the air
    this.shots = new Map();
    this.nextShot = 1;
  }

  // The SiegeEngine class of the server API, an Entity.
  define(Entity) {
    const registry = this;
    class SiegeEngine {
      constructor(id) {
        const engine = registry.engines.get(id);
        if (!engine) {
          throw new Error('Unknown siege engine');
        }
        return engine;
      }
      static spawn(kind, position, orientation, dimension) {
        return registry.spawn(SiegeEngine, { kind, position, orientation, dimension });
      }
      static all(dimension) {
        const engines = [...registry.engines.values()];
        if (dimension === undefined) {
          return engines;
        }
        return engines.filter((engine) => engine.dimension === dimension);
      }
      static getById(id) {
        return registry.engines.get(id) ?? null;
      }
      static destroyAll(dimension) {
        if (dimension !== undefined && !validWorld(dimension)) {
          throw new TypeError('Invalid dimension');
        }
        let removed = 0;
        for (const engine of [...registry.engines.values()]) {
          if (dimension === undefined || engine.dimension === dimension) {
            registry.destroy(engine);
            removed += 1;
          }
        }
        return removed;
      }
      get position() {
        return new Vector3(this.at.x, this.at.y, this.at.z);
      }
      get rotation() {
        const f = this.s.facing;
        return new Quaternion(f.w, f.x, f.y, f.z);
      }
      get cycle() {
        return this.s.cycle;
      }
      get loaded() {
        return this.s.cycle === 'ready';
      }
      get speed() {
        return this.s.speed;
      }
      set speed(value) {
        registry.setSpeed(this, value);
      }
      get damage() {
        return this.s.damage;
      }
      set damage(value) {
        this.s.damage = registry.numberOf(value, 0, Infinity, 'Siege engine damage');
      }
      get blastRadius() {
        return this.s.blastRadius;
      }
      set blastRadius(value) {
        this.s.blastRadius = registry.numberOf(value, 0, RADIUS_MAX, 'Siege engine blastRadius');
      }
      get usable() {
        return this.s.usable;
      }
      set usable(value) {
        registry.setUsable(this, value);
      }
      get range() {
        return this.s.range;
      }
      set range(value) {
        this.s.range = registry.clampRange(this, registry.numberOf(value, 0, Infinity, 'range'));
      }
      get minRange() {
        return this.spec.minRange;
      }
      get maxRange() {
        return this.spec.maxRange;
      }
      get reloadTime() {
        return this.spec.load / this.s.speed;
      }
      get maxHealth() {
        return this.s.maxHealth;
      }
      set maxHealth(value) {
        registry.setMaxHealth(this, value);
      }
      get health() {
        return this.s.health;
      }
      set health(value) {
        registry.setHealth(this, value);
      }
      get wrecked() {
        return this.s.wrecked;
      }
      get ammo() {
        return this.s.ammo;
      }
      set ammo(value) {
        this.s.ammo = registry.numberOf(value, -Infinity, Infinity, 'Siege engine ammo');
      }
      get crewMinimum() {
        return this.s.crewMinimum;
      }
      set crewMinimum(value) {
        const places = this.spec.crewSeats;
        this.s.crewMinimum = Math.trunc(registry.numberOf(value, 0, places, 'crewMinimum'));
      }
      get crewSeats() {
        return this.spec.crewSeats;
      }
      get operator() {
        return this.s.operator;
      }
      // The winches are not manned yet: one empty place per winch.
      get crew() {
        return new Array(this.spec.crewSeats).fill(null);
      }
      load() {
        return registry.load(this);
      }
      fire(target, attacker) {
        return registry.fire(this, target, attacker);
      }
      aim(target) {
        return registry.aim(this, target);
      }
      inReach(target) {
        return registry.inReach(this, registry.targetOf(target));
      }
      showAimLine(enabled) {
        registry.showAimLine(this, enabled);
      }
      aimLine() {
        return this.s.aimGuide;
      }
      repair() {
        registry.repair(this);
      }
      assignGunner(player) {
        return registry.assignGunner(this, player);
      }
      destroy() {
        registry.destroy(this);
      }
      toString() {
        return `SiegeEngine(${this.id}, ${this.kind}, ${this.s.cycle})`;
      }
    }
    Object.setPrototypeOf(SiegeEngine.prototype, Entity.prototype);
    return SiegeEngine;
  }

  numberOf(value, min, max, what) {
    if (typeof value !== 'number' || Number.isNaN(value)) {
      throw new TypeError(`${what} must be a number`);
    }
    return Math.min(max, Math.max(min, value));
  }

  clampRange(engine, range) {
    return Math.min(engine.spec.maxRange, Math.max(engine.spec.minRange, range));
  }

  targetOf(target) {
    const point = vector(target ?? {});
    if (!isPoint(point)) {
      throw new RangeError('Invalid siege target');
    }
    return { x: point.x, y: point.y, z: point.z };
  }

  attackerOf(given) {
    if (given === null || given === undefined) {
      return null;
    }
    const player = this.host.players.get(typeof given === 'number' ? given : given?.id);
    return player ?? null;
  }

  spawn(SiegeEngine, { kind, position = {}, orientation = {}, dimension }) {
    const owner = this.host.context.getStore();
    if (
      owner &&
      (this.host.resources.get(owner.name) !== owner || !owner.active || owner.failing)
    ) {
      throw new Error('Resource is stopped');
    }
    const spec = KINDS[kind];
    if (!Object.hasOwn(KINDS, kind)) {
      throw new RangeError('Siege engine kind must be trebuchet or cannon');
    }
    const world = dimension ?? GLOBAL_WORLD;
    if (!validWorld(world)) {
      throw new TypeError('Invalid dimension');
    }
    const at = this.targetOf(position);
    const facing = rotation(orientation);
    const engine = Object.create(SiegeEngine.prototype);
    Object.defineProperty(engine, 'id', { value: this.host.allocateEntityId(), enumerable: true });
    Object.defineProperty(engine, 'kind', { value: kind, enumerable: true });
    Object.defineProperty(engine, 'dimension', { value: world, enumerable: true });
    Object.defineProperty(engine, 'spec', { value: spec });
    Object.defineProperty(engine, 'at', { value: at });
    // The changeable state; `step` is the timed cycle step in progress, if any.
    Object.defineProperty(engine, 's', {
      value: {
        cycle: 'idle',
        speed: 1,
        damage: 0,
        blastRadius: DEFAULT_RADIUS,
        usable: false,
        range: spec.defaultRange,
        maxHealth: 0,
        health: 0,
        wrecked: false,
        ammo: -1,
        crewMinimum: 0,
        operator: null,
        aimGuide: false,
        target: null,
        facing: { x: facing.x, y: facing.y, z: facing.z, w: facing.w },
        step: null,
      },
    });
    engine[OWNER] = runningResource(this.host);
    this.engines.set(engine.id, engine);
    this.host.bridge.send(this.spawnMessage(engine));
    this.host.emit('siegeEngineSpawn', engine);
    return engine;
  }

  // The op that makes an engine appear, with the state it has now.
  spawnMessage(engine) {
    return {
      op: 'siegeSpawn',
      id: engine.id,
      kind: engine.kind,
      position: { ...engine.at },
      rotation: { ...engine.s.facing },
      dimension: engine.dimension,
      ...this.stateOf(engine),
    };
  }

  // A player's world became ready: their game lost the engines, so it is told of each again. The
  // op names the player, so nobody else hears it.
  arrived(player) {
    for (const engine of this.engines.values()) {
      this.host.bridge.send({ ...this.spawnMessage(engine), to: player.id });
    }
  }

  // The part of an engine's state the clients follow.
  stateOf(engine, seconds = 0) {
    const s = engine.s;
    return {
      cycle: s.cycle,
      seconds,
      speed: s.speed,
      rotation: { ...s.facing },
      aimGuide: s.aimGuide,
      target: s.target ? { ...s.target } : { x: 0, y: 0, z: 0 },
      wrecked: s.wrecked,
      usable: s.usable,
      operator: s.operator ? s.operator.id : 0,
    };
  }

  sendState(engine, seconds = 0) {
    if (this.engines.get(engine.id) !== engine) {
      return;
    }
    this.host.bridge.send({ op: 'siegeUpdate', id: engine.id, ...this.stateOf(engine, seconds) });
  }

  // --- timed steps -------------------------------------------------------------------------

  // Runs `then` after `seconds` of the engine's own pace; one step at a time per engine.
  startStep(engine, seconds, then) {
    this.cancelStep(engine);
    const ms = Math.max(0, seconds * 1000);
    const step = { due: Date.now() + ms, speed: engine.s.speed, then, timer: null };
    step.timer = this.timerFor(engine, step, ms);
    engine.s.step = step;
  }

  timerFor(engine, step, ms) {
    const timer = setTimeout(() => {
      if (engine.s.step !== step) {
        return;
      }
      engine.s.step = null;
      step.then();
    }, ms);
    timer.unref?.();
    return timer;
  }

  cancelStep(engine) {
    const step = engine.s.step;
    if (step) {
      clearTimeout(step.timer);
      engine.s.step = null;
    }
  }

  // A new speed carries the step in progress on from where it has got to.
  setSpeed(engine, value) {
    const speed = this.numberOf(value, SPEED_MIN, SPEED_MAX, 'Siege engine speed');
    if (speed === engine.s.speed) {
      return;
    }
    const step = engine.s.step;
    const previous = engine.s.speed;
    engine.s.speed = speed;
    let seconds = 0;
    if (step) {
      clearTimeout(step.timer);
      const left = Math.max(0, step.due - Date.now()) * (previous / speed);
      step.due = Date.now() + left;
      step.speed = speed;
      step.timer = this.timerFor(engine, step, left);
      seconds = left / 1000;
    }
    this.sendState(engine, seconds);
  }

  // --- the cycle ---------------------------------------------------------------------------

  load(engine) {
    const s = engine.s;
    if (s.wrecked) return refused('is wrecked');
    if (s.cycle === 'ready') return refused('is already loaded');
    if (s.cycle !== 'idle') return refused('is busy');
    // A positive count is spent now; a negative one never runs out.
    if (s.ammo === 0) return refused('is out of ammunition');
    if (s.ammo > 0) s.ammo -= 1;
    const total = engine.reloadTime;
    const draw = total * engine.spec.draw;
    if (draw > 0) {
      s.cycle = 'drawing';
      this.sendState(engine, draw);
      this.startStep(engine, draw, () => this.beginLoading(engine, total - draw));
    } else {
      this.beginLoading(engine, total);
    }
    return accepted(total);
  }

  beginLoading(engine, seconds) {
    engine.s.cycle = 'loading';
    this.sendState(engine, seconds);
    this.startStep(engine, seconds, () => {
      engine.s.cycle = 'ready';
      this.sendState(engine);
      this.host.emit('siegeEngineReady', engine);
    });
  }

  inReach(engine, target) {
    if (engine.s.wrecked) return 'is wrecked';
    return rangeProblem(engine.spec, horizontal(engine.at, target));
  }

  aim(engine, targetGiven) {
    const target = this.targetOf(targetGiven);
    const problem = this.inReach(engine, target);
    if (problem) return refused(problem);
    this.turnTo(engine, target);
    this.sendState(engine);
    return accepted(0);
  }

  turnTo(engine, target) {
    engine.s.facing = facingOf(target.x - engine.at.x, target.y - engine.at.y);
    engine.s.target = target;
  }

  fire(engine, targetGiven, attackerGiven) {
    const target = this.targetOf(targetGiven);
    const attacker = this.attackerOf(attackerGiven);
    const s = engine.s;
    if (s.wrecked) return refused('is wrecked');
    if (s.cycle !== 'ready') return refused('is not loaded');
    const problem = this.inReach(engine, target);
    if (problem) return refused(problem);
    this.turnTo(engine, target);
    s.cycle = 'firing';
    const seconds = engine.spec.release / s.speed;
    this.sendState(engine, seconds);
    this.startStep(engine, seconds, () => this.release(engine, target, attacker));
    return accepted(seconds);
  }

  // The stone is let go: every client draws it along the arc, and one player is asked where it
  // came down.
  release(engine, target, attacker) {
    const s = engine.s;
    s.cycle = 'idle';
    this.sendState(engine);
    this.host.emit('siegeFire', engine, attacker, new Vector3(target.x, target.y, target.z));
    const from = { x: engine.at.x, y: engine.at.y, z: engine.at.z + engine.spec.launch };
    const arc = arcOf(engine.spec, from, target);
    const reporter = this.reporterFor(engine.dimension, attacker, target);
    const shot = {
      number: this.nextShot++,
      engine,
      world: engine.dimension,
      from,
      target,
      attacker,
      reporter,
      damage: s.damage,
      radius: s.blastRadius,
      timer: null,
    };
    this.shots.set(shot.number, shot);
    if (!reporter) {
      this.land(shot, target);
      return;
    }
    this.host.bridge.send({
      op: 'siegeShot',
      id: engine.id,
      shot: shot.number,
      from,
      target,
      flight: arc.flight,
      apex: arc.apex,
      reporter: reporter.id,
      dimension: shot.world,
    });
    shot.timer = setTimeout(() => this.land(shot, target), arc.flight * 1000 + REPORT_GRACE_MS);
    shot.timer.unref?.();
  }

  // Who is asked to report a landing: the attacker when near enough to the target, else the ready
  // player nearest it, else nobody.
  reporterFor(world, attacker, target) {
    const near = (player, limit) =>
      player.ready && player.dimension === world && distance3(player.position, target) <= limit;
    if (
      attacker &&
      this.host.players.get(attacker.id) === attacker &&
      near(attacker, REPORT_DISTANCE)
    ) {
      return attacker;
    }
    let best = null;
    let bestDistance = VIEW_DISTANCE;
    for (const player of this.host.players.values()) {
      if (!near(player, VIEW_DISTANCE)) continue;
      const d = distance3(player.position, target);
      if (d <= bestDistance) {
        best = player;
        bestDistance = d;
      }
    }
    return best;
  }

  // The client's report of where a shot came down; kept to the reporter and to the arc, else the
  // target stands.
  impactReport(player, report) {
    const shot = this.shots.get(report?.shot);
    if (!player || !shot || shot.reporter !== player || report.engine !== shot.engine.id) {
      return;
    }
    const point = vector(report.position ?? {});
    const position = { x: point.x, y: point.y, z: point.z };
    const trusted = isPoint(position) && onArc(shot.from, shot.target, position);
    this.land(shot, trusted ? position : shot.target);
  }

  // --- impacts -----------------------------------------------------------------------------

  // A shot came down at `position`: everything within the radius takes its share, then
  // siegeImpact. A shot lands once.
  land(shot, position) {
    if (this.shots.get(shot.number) !== shot) {
      return;
    }
    clearTimeout(shot.timer);
    this.shots.delete(shot.number);
    this.blast(shot, position);
    const engine = this.engines.get(shot.engine.id) === shot.engine ? shot.engine : null;
    this.host.emit(
      'siegeImpact',
      engine,
      new Vector3(position.x, position.y, position.z),
      shot.attacker,
    );
  }

  blast(shot, position) {
    if (!(shot.damage > 0)) {
      return;
    }
    const share = (point) => shareOf(shot.damage, shot.radius, distance3(point, position));
    for (const engine of [...this.engines.values()]) {
      const amount = engine.dimension === shot.world ? share(engine.at) : 0;
      if (amount > 0) this.takeDamage(engine, amount, shot.attacker);
    }
    for (const player of [...this.host.players.values()]) {
      const amount = player.dimension === shot.world ? share(player.position) : 0;
      if (amount > 0) this.host.addPlayerStat(player, 'health', -amount);
    }
    for (const npc of [...this.host.npcs.npcs.values()]) {
      const amount = npc.dimension === shot.world ? share(npc.position) : 0;
      if (amount > 0) this.hurtNpc(npc, amount, shot.attacker);
    }
  }

  hurtNpc(npc, amount, attacker) {
    try {
      this.host.npcs.scriptedDamage(npc, amount, attacker);
    } catch (error) {
      // An NPC removed by an earlier handler of this very blast.
    }
  }

  // --- health ------------------------------------------------------------------------------

  takeDamage(engine, amount, attacker) {
    const s = engine.s;
    if (s.maxHealth === 0 || s.wrecked) {
      return;
    }
    s.health = Math.max(0, s.health - amount);
    this.host.emit('siegeEngineDamage', engine, amount, attacker);
    if (s.health === 0) {
      this.wreck(engine, attacker);
    }
  }

  wreck(engine, attacker) {
    const s = engine.s;
    s.wrecked = true;
    s.cycle = 'idle';
    s.operator = null;
    this.cancelStep(engine);
    this.sendState(engine);
    this.host.emit('siegeEngineWreck', engine, attacker);
  }

  setMaxHealth(engine, value) {
    const max = this.numberOf(value, 0, Infinity, 'Siege engine maxHealth');
    engine.s.maxHealth = max;
    engine.s.health = max;
    if (engine.s.wrecked && max > 0) {
      this.repair(engine);
    }
  }

  setHealth(engine, value) {
    const s = engine.s;
    const health = this.numberOf(value, 0, Infinity, 'Siege engine health');
    if (s.maxHealth === 0) {
      return;
    }
    s.health = Math.min(s.maxHealth, health);
    if (s.health === 0 && !s.wrecked) {
      this.wreck(engine, null);
    } else if (s.health > 0 && s.wrecked) {
      this.revive(engine);
    }
  }

  repair(engine) {
    engine.s.health = engine.s.maxHealth;
    if (engine.s.wrecked) {
      this.revive(engine);
    }
  }

  revive(engine) {
    engine.s.wrecked = false;
    engine.s.cycle = 'idle';
    this.sendState(engine);
  }

  // --- players -----------------------------------------------------------------------------

  setUsable(engine, value) {
    const usable = Boolean(value);
    if (usable === engine.s.usable) {
      return;
    }
    engine.s.usable = usable;
    this.sendState(engine);
  }

  showAimLine(engine, enabled) {
    const guide = Boolean(enabled);
    if (guide === engine.s.aimGuide) {
      return;
    }
    engine.s.aimGuide = guide;
    this.sendState(engine);
  }

  // Puts a player in the engine's place, or empties it. False for a player who is gone.
  assignGunner(engine, player) {
    if (player !== null && this.host.players.get(player?.id) !== player) {
      return false;
    }
    if (player) {
      this.leaveEngines(player);
    }
    if (engine.s.operator !== player) {
      engine.s.operator = player;
      this.sendState(engine);
    }
    return true;
  }

  // One operator place per player: they first leave any other engine.
  leaveEngines(player) {
    for (const engine of this.engines.values()) {
      if (engine.s.operator === player) {
        engine.s.operator = null;
        this.sendState(engine);
      }
    }
  }

  // siegeUse from a client: the player asks to take the place, leave it, load or fire. Each ask
  // raises siegeEngineUse first, which a handler may refuse by returning false.
  use(player, report) {
    const engine = this.engines.get(report?.engine);
    const action = report?.action;
    if (!player || !engine || !ACTIONS.has(action) || engine.dimension !== player.dimension) {
      return;
    }
    const s = engine.s;
    const here = s.operator === player;
    if (action === 'operate') {
      const close = distance3(player.position, engine.at) <= USE_DISTANCE;
      if (!s.usable || s.wrecked || s.operator || !close) return;
    } else if (!here || s.wrecked) {
      return;
    }
    if (this.host.emit('siegeEngineUse', player, engine, action) === false) {
      return;
    }
    if (action === 'operate') {
      this.assignGunner(engine, player);
    } else if (action === 'leave') {
      this.assignGunner(engine, null);
    } else if (action === 'load') {
      this.load(engine);
    } else {
      this.fire(engine, this.laidTarget(engine, report.look), player);
    }
  }

  // Where a shot goes when the operator fires without laying the engine: `range` metres along
  // where they look (the engine's own facing without a look direction).
  laidTarget(engine, look) {
    const f = engine.s.facing;
    let dx = -2 * f.z * f.w;
    let dy = 1 - 2 * f.z * f.z;
    if (look && Number.isFinite(look.x) && Number.isFinite(look.y) && (look.x || look.y)) {
      dx = look.x;
      dy = look.y;
    }
    const length = Math.hypot(dx, dy) || 1;
    const range = engine.s.range;
    return {
      x: engine.at.x + (dx / length) * range,
      y: engine.at.y + (dy / length) * range,
      z: engine.at.z,
    };
  }

  // A player left: they let go of the engine they worked.
  left(player) {
    this.leaveEngines(player);
  }

  // The siege engines a stopping resource made go with it.
  resourceStopped(name) {
    removeOwned(this.engines.values(), name, (engine) => this.destroy(engine));
  }

  destroy(engine) {
    if (this.engines.get(engine.id) !== engine) {
      return;
    }
    this.host.emit('siegeEngineDestroy', engine);
    this.cancelStep(engine);
    this.engines.delete(engine.id);
    this.host.bridge.send({ op: 'siegeDestroy', id: engine.id });
  }
}

module.exports = { SiegeEngines };
