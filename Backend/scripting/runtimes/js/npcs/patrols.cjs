'use strict';

// NPC patrols: Npc.patrol, routeState and the events patrolWaypoint and
// patrolFinished. The server steps a patrol: it copies the route when the NPC is ordered and
// gives the NPC one leg at a time as a move order (the intent field has no patrol kind), and the
// simulator's report that a leg ended (npcs.cjs intentDone) steps it on. The NPC's body, its
// pathfinding and its movement stay the simulating client's, as for any move.
//
// A leg that ends `reached` waits the leg's waitSeconds, then the next one is ordered. A leg that
// ends `blocked` or `failed` is skipped, but not before MIN_SKIP_MS after it was ordered, so a
// route nobody can walk does not spin; if all legs of one lap fail consecutively, the patrol is
// abandoned and that last leg reports `failed`. Any other order, a hold, the NPC's death or its
// removal cancels the patrol.

const { PatrolRoutes } = require('./patrol_routes.cjs');
const { runningResource } = require('../lifecycle/owned_objects.cjs');
const { PATROL_ROUTE_POINTS_MAX, ARRIVAL_RADIUS, placeOf } = require('./npc_values.cjs');

// A skipped leg is left no sooner than this long after its order (milliseconds).
const MIN_SKIP_MS = 2000;

// What a patrol reports of itself: its PatrolState.
function snapshotOf(run) {
  return {
    routeId: run.routeId,
    waypointIndex: run.index,
    lap: run.lap,
    direction: run.direction,
    phase: run.phase,
  };
}

// How many legs make a lap: a round trip of a ping-pong patrol, the whole route otherwise.
function legsPerLap(run) {
  if (run.mode === 'pingPong') {
    return Math.max(2 * (run.legs.length - 1), 1);
  }
  return run.legs.length;
}

// Where the patrol goes after the leg it is on: { index, direction, lap }, or null when it has
// been all the way round once.
function successorOf(run) {
  const { index, direction, lap } = run;
  const last = run.legs.length - 1;
  if (run.mode === 'pingPong') {
    if (last === 0) {
      return { index: 0, direction: 1, lap: lap + 1 };
    }
    if (direction === 1 && index === last) {
      return { index: last - 1, direction: -1, lap };
    }
    if (direction === -1 && index === 0) {
      return { index: 1, direction: 1, lap: lap + 1 };
    }
    return { index: index + direction, direction, lap };
  }
  if (index < last) {
    return { index: index + 1, direction, lap };
  }
  if (run.mode === 'loop') {
    return { index: 0, direction, lap: lap + 1 };
  }
  return null;
}

class NpcPatrols {
  // `npcs` is the registry, `stateKey` the symbol under which an NPC keeps its state, whose
  // `patrol` field holds the run (null when not patrolling).
  constructor(npcs, stateKey) {
    this.npcs = npcs;
    this.stateKey = stateKey;
    this.routes = new PatrolRoutes(() => runningResource(npcs.host));
  }

  // The patrol an Npc.patrol call asks for, copied: { routeId, mode, pathfinding, legs, index,
  // lap, direction, phase, ... }, each leg with the speed, radius and waitSeconds it will use
  // (the waypoint's own, else the call's, else the route's, else walk, 1.5 m and no wait). Null
  // for a route that is empty, too long, holds a point the NPC cannot use, was given a
  // waitSeconds that is not a number, or is gone or unknown. `overrides` are the call's options
  // (patrolOverridesOf); a point lacking a component keeps the one of `current`.
  runOf(source, overrides, current) {
    let defaults = {};
    let routeId = null;
    let points = null;
    if (Array.isArray(source)) {
      points = source.map((point) => ({ place: placeOf(point, current) }));
    } else {
      const definition = this.routes.lookup(source);
      if (definition === null) {
        return null;
      }
      defaults = definition;
      routeId = definition.id;
      points = definition.points.map((point) => ({ ...point, place: point.position }));
    }
    const unusable = points.length === 0 || points.length > PATROL_ROUTE_POINTS_MAX;
    if (
      unusable ||
      overrides.waitSeconds === null ||
      points.some((point) => point.place === null)
    ) {
      return null;
    }
    const legs = points.map((point) => ({
      place: { x: point.place.x, y: point.place.y, z: point.place.z },
      speed: point.speed ?? overrides.speed ?? defaults.speed ?? 'walk',
      radius: point.radius ?? overrides.radius ?? defaults.radius ?? ARRIVAL_RADIUS,
      waitSeconds: point.waitSeconds ?? overrides.waitSeconds ?? defaults.waitSeconds ?? 0,
    }));
    return {
      routeId,
      mode: overrides.mode ?? defaults.mode ?? 'loop',
      pathfinding: overrides.pathfinding ?? 'auto',
      legs,
      index: 0,
      lap: 0,
      direction: 1,
      phase: 'moving',
      // Legs that ended blocked or failed in a row, when the leg now ordered was ordered, and the
      // timer of a wait or a skip that is under way.
      failed: 0,
      ordered: 0,
      timer: null,
    };
  }

  snapshot(run) {
    return snapshotOf(run);
  }

  // The first leg is ordered by the caller (Npcs.patrol); this orders the one the run is on now.
  noteOrdered(run) {
    run.phase = 'moving';
    run.ordered = Date.now();
  }

  // Takes the NPC's patrol off it without a word, for the caller to announce once the NPC's new
  // order stands. Returns the run, or null when it was not patrolling.
  release(state) {
    const run = state.patrol;
    if (run === null) {
      return null;
    }
    clearTimeout(run.timer);
    run.timer = null;
    state.patrol = null;
    return run;
  }

  // Raises patrolFinished for a run that `release` took off its NPC.
  cancelled(npc, run) {
    if (run !== null) {
      this.finished(npc, run, 'cancelled');
    }
  }

  // Cancels the NPC's patrol, if any, and raises patrolFinished 'cancelled'.
  cancel(npc) {
    this.cancelled(npc, this.release(npc[this.stateKey]));
  }

  finished(npc, run, status) {
    this.npcs.host.emit('patrolFinished', npc, { ...snapshotOf(run), status });
  }

  // The simulator reported that the leg the NPC is on ended with `status` (reached, blocked or
  // failed). Raises patrolWaypoint and npcIntentDone, then steps the patrol on unless a handler
  // replaced it. A report that arrives while a wait or a skip is under way is dropped.
  legEnded(npc, run, status) {
    if (run.timer !== null) {
      return;
    }
    const state = npc[this.stateKey];
    if (status === 'reached') {
      run.failed = 0;
    } else {
      run.failed += 1;
    }
    // A lap of failed legs gives the patrol up, and the leg that did it reports `failed`.
    const abandoned = run.failed >= legsPerLap(run);
    const outcome = abandoned ? 'failed' : status;
    state.status = outcome;
    const sequence = state.intentSequence;
    const host = this.npcs.host;
    host.emit('patrolWaypoint', npc, { ...snapshotOf(run), status: outcome });
    host.emit('npcIntentDone', npc, outcome);
    if (state.patrol !== run || state.intentSequence !== sequence) {
      return;
    }
    if (abandoned) {
      state.patrol = null;
      this.finished(npc, run, 'failed');
    } else if (outcome === 'reached' && run.legs[run.index].waitSeconds > 0) {
      run.phase = 'waiting';
      this.later(npc, run, run.legs[run.index].waitSeconds * 1000);
    } else if (outcome === 'reached') {
      this.next(npc, run);
    } else {
      const remaining = Math.max(0, MIN_SKIP_MS - (Date.now() - run.ordered));
      this.later(npc, run, remaining);
    }
  }

  // Goes on with `run` after `delay` milliseconds, at once for none.
  later(npc, run, delay) {
    if (delay <= 0) {
      this.next(npc, run);
      return;
    }
    run.timer = setTimeout(() => {
      run.timer = null;
      if (this.npcs.live(npc) && npc[this.stateKey].patrol === run) {
        this.next(npc, run);
      }
    }, delay);
    run.timer.unref?.();
  }

  // Orders the leg after the one the run is on, or ends the run when it has been round once.
  next(npc, run) {
    const successor = successorOf(run);
    if (successor === null) {
      npc[this.stateKey].patrol = null;
      this.finished(npc, run, 'completed');
      return;
    }
    Object.assign(run, successor);
    this.npcs.giveLeg(npc, run);
  }
}

module.exports = { NpcPatrols };
