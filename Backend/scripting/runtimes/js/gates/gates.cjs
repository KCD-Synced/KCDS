'use strict';

// The levels' gates, the same for every player (the server API's Gate). The server
// owns each gate's state; every player's game moves the gate into it smoothly
// (Client/runtime/gates/):
//   - the animated gates of kutnohorsko, the Malesov drawbridge and the Suchdol portcullis
//     (states closed and open), with their own animated model over the clip's length;
//   - the static gate series of trosecko and kutnohorsko (catalog.json, generated from the levels
//     by Dev/tools/gates/build_catalog.py; states as the level names them: closed, open,
//     barricaded, ...), whose leaves travel, slide or fade from one state into the next in 4 s.
//
// A series with a bar across its closed leaves (the Trosky and Suchdol palisade gates) lets a
// resource take the bar out and put it back while the gate stays shut (removeBar, placeBar), so a
// roleplay server can have a player lift the bar off (and, say, carry it in their inventory)
// before the gate can be opened. Opening a barred gate takes the bar out first; closing puts it
// back unless asked not to.
//
// Server -> client op, for one ready player (`id`), sent when a resource sets a gate's state or
// its bar and when a player's world is ready (again), for each gate a resource has set:
//   { op: 'gateState', id: <player id>, gate, state, elapsedMs, barOut, barElapsedMs }
// `gate` is the gate's number (GATES order: the client's kGates, then its kGateSeries), `state`
// the index of its state, `elapsedMs` how long ago the move began, so a late joiner sees it part
// way or finished; `barOut` whether the bar is out in the bar state and `barElapsedMs` how long
// ago it began to move. A gate no resource set stays as the level shows it. No timers.
//
// Events: gateChanged (gate, state) when a resource starts a gate moving into a state;
// gateBarChanged (gate, barred) when a resource takes a gate's bar out or puts it back.

const SERIES = require('./catalog.json');

// The animated gates: key, level, states and how long moving into each takes (the clips').
const ANIMATED = [
  {
    key: 'malesovDrawbridge',
    level: 'kutnohorsko',
    states: ['closed', 'open'],
    seconds: [7.733, 7.733],
    guid: '495c74b33d651072',
    position: { x: 469.485, y: 371.745, z: 111.96 },
    initial: 'open',
    bar: null,
  },
  {
    key: 'suchdolPortcullis',
    level: 'kutnohorsko',
    states: ['closed', 'open'],
    seconds: [6.967, 17.033],
    guid: '4b5d68eb986fda15',
    position: { x: 751.45, y: 3354.923, z: 142.02 },
    initial: 'open',
    bar: null,
  },
];
// A series' move from one state to the next (the client's kSeriesSeconds), and its bar's own
// move out or in (kBarSeconds).
const SERIES_SECONDS = 4;
const BAR_SECONDS = 1.8;
const GATES = [
  ...ANIMATED,
  ...SERIES.map((series) => ({
    key: series.key,
    level: series.map,
    states: series.states,
    seconds: series.states.map(() => SERIES_SECONDS),
    guid: series.guid,
    position: { x: series.center[0], y: series.center[1], z: series.center[2] },
    initial: null,
    bar: series.bar ?? null,
  })),
];
// The longest move the op carries; any older move is over.
const MAX_ELAPSED_MS = 600000;

// The state open() and close() pick: the one so named, or else the first whose name says so.
function stateLike(states, exact, word) {
  if (states.includes(exact)) return exact;
  return states.find((name) => name.toLowerCase().includes(word)) ?? null;
}

function isOpenState(name) {
  const lower = String(name ?? '').toLowerCase();
  return lower.includes('open') && !lower.includes('close');
}

class Gates {
  // `now` is the real time in milliseconds, from a clock that only goes forward.
  constructor(host, now = () => performance.now()) {
    this.host = host;
    this.now = now;
    // index -> { state (index), began, barOut, barBegan } of a gate a resource set
    this.states = new Map();
    // index -> the one handle of the gate
    this.handles = new Map();
  }

  // The Gate class of the server API; every resource gets the same one.
  define() {
    this.Gate ??= this.makeClass();
    return this.Gate;
  }

  makeClass() {
    const registry = this;
    class Gate {
      constructor(index) {
        if (!Number.isInteger(index) || index < 0 || index >= GATES.length) {
          throw new Error('Unknown gate');
        }
        const known = registry.handles.get(index);
        if (known) return known;
        Object.defineProperty(this, 'index', { value: index, enumerable: true });
        registry.handles.set(index, this);
      }
      get key() {
        return GATES[this.index].key;
      }
      get level() {
        return GATES[this.index].level;
      }
      // The level's EntityGuid of the gate, or of what it shows; '' when the level gives it none.
      get guid() {
        return GATES[this.index].guid;
      }
      // Where the level has it (a series: the middle of what it shows).
      get position() {
        return { ...GATES[this.index].position };
      }
      // A number that names it for as long as the server runs.
      get id() {
        return this.index;
      }
      // What it is: drawbridge, portcullis, or gate for leaves moved between their states.
      get kind() {
        const { key } = GATES[this.index];
        if (/drawbridge/i.test(key)) {
          return 'drawbridge';
        }
        return /portcullis/i.test(key) ? 'portcullis' : 'gate';
      }
      // Current phase: closed, opening, open or closing. Only open lets anyone through.
      get cycle() {
        const open = this.isOpen;
        if (registry.moving(this.index)) {
          return open ? 'opening' : 'closing';
        }
        return open ? 'open' : 'closed';
      }
      // How long opening takes, in seconds; 0 for a gate that has no open state.
      get openingTime() {
        const state = stateLike(GATES[this.index].states, 'open', 'open');
        return state === null ? 0 : this.secondsTo(state);
      }
      // How long closing takes, in seconds; 0 for a gate that has no closed state.
      get closingTime() {
        const state = stateLike(GATES[this.index].states, 'closed', 'clos');
        return state === null ? 0 : this.secondsTo(state);
      }
      // Its states' names.
      get states() {
        return [...GATES[this.index].states];
      }
      // Its state's name: the one it stands in or moves into; null while it stands as the level
      // shows it (a series no resource set).
      get state() {
        return registry.stateName(this.index);
      }
      // Whether its state is an open one (open, opened, openFull, ...).
      get isOpen() {
        return isOpenState(this.state);
      }
      // How far open it is, 0 shut to 1 open, as it moves: the share of the way into
      // an open state, or what is left of the way into a shut one.
      get openness() {
        return registry.openness(this.index);
      }
      // Whether it is still on its way into its state, or its bar on its way out or in.
      get moving() {
        return registry.moving(this.index) || registry.barMoving(this.index);
      }
      // Whether it has a bar across its leaves in one of its states (barState).
      get hasBar() {
        return GATES[this.index].bar !== null;
      }
      // The state in which the bar sits across the leaves; null without a bar.
      get barState() {
        return GATES[this.index].bar;
      }
      // Whether the bar is in its brackets: true or false in the bar state, false in any other;
      // null without a bar or while the gate stands as the level shows it.
      get barred() {
        return registry.barred(this.index);
      }
      // How long moving into that state takes, in seconds.
      secondsTo(state) {
        const at = GATES[this.index].states.indexOf(state);
        return at < 0 ? null : GATES[this.index].seconds[at];
      }
      // Starts the gate moving into that state for every player; false for a state it does not
      // have, the state it is in, or while it moves. Into its bar state the bar is put back at
      // the end unless `options.bar` is false.
      setState(state, options = {}) {
        return registry.move(this, GATES[this.index].states.indexOf(state), options.bar !== false);
      }
      // Starts it opening (its open state); false when it has none, is open, or moves. A barred
      // gate's bar is taken out first.
      open() {
        const state = stateLike(GATES[this.index].states, 'open', 'open');
        return state !== null && this.setState(state);
      }
      // Starts it closing (its closed state); false when it has none, is closed, or moves. Its
      // bar is put back at the end unless `options.bar` is false.
      close(options = {}) {
        const state = stateLike(GATES[this.index].states, 'closed', 'clos');
        return state !== null && this.setState(state, options);
      }
      // Opens a shut gate or shuts an open one (open() or close()); false as they are.
      toggle(options = {}) {
        return this.isOpen ? this.close(options) : this.open();
      }
      // Takes the bar out of its brackets (the gate stays shut) for every player; false without a
      // bar, outside the bar state, when it is out already, or while the gate moves.
      removeBar() {
        return registry.setBar(this, false);
      }
      // Puts the bar back across the shut gate; false without a bar, outside the bar state, when
      // it is in already, or while the gate moves.
      placeBar() {
        return registry.setBar(this, true);
      }
      toString() {
        return `Gate(${this.key}, ${this.state ?? 'as the level has it'}, ${
          this.moving ? 'moving' : 'still'
        })`;
      }
      static all(level) {
        return GATES.map((gate, index) => new Gate(index)).filter(
          (gate) => level === undefined || gate.level === level,
        );
      }
      // The gate with this key or level GUID (with or without 0x), or null.
      static find(key) {
        const text = String(key).toLowerCase().replace(/^0x/, '');
        const index = GATES.findIndex(
          (gate) => gate.key === key || (gate.guid && gate.guid === text),
        );
        return index < 0 ? null : new Gate(index);
      }
      static getById(id) {
        return Number.isInteger(id) && id >= 0 && id < GATES.length ? new Gate(id) : null;
      }
      // The gate nearest a point within `radius` metres, on the level this server runs; null for none.
      static nearest(position, radius) {
        const usable = (value) => typeof value === 'number' && Number.isFinite(value);
        if (!position || !usable(position.x) || !usable(position.y) || !usable(position.z)) {
          throw new TypeError('Gate.nearest: position needs finite x, y and z');
        }
        if (!usable(radius) || radius < 0) {
          throw new TypeError('Gate.nearest: radius is a number of metres, 0 or more');
        }
        const level = registry.host.areas?.level;
        let best = null;
        let bestDistance = radius;
        GATES.forEach((gate, index) => {
          if (level && gate.level !== level) {
            return;
          }
          const at = gate.position;
          const distance = Math.hypot(at.x - position.x, at.y - position.y, at.z - position.z);
          if (distance <= bestDistance) {
            best = index;
            bestDistance = distance;
          }
        });
        return best === null ? null : new Gate(best);
      }
    }
    return Gate;
  }

  stateName(index) {
    const set = this.states.get(index);
    return set ? GATES[index].states[set.state] : GATES[index].initial;
  }

  barred(index) {
    const bar = GATES[index].bar;
    const set = this.states.get(index);
    if (bar === null || !set) return null;
    return GATES[index].states[set.state] === bar && !set.barOut;
  }

  elapsed(index) {
    const set = this.states.get(index);
    return set ? Math.min(MAX_ELAPSED_MS, Math.max(0, this.now() - set.began)) : MAX_ELAPSED_MS;
  }

  barElapsed(index) {
    const set = this.states.get(index);
    if (!set || set.barBegan === null) return MAX_ELAPSED_MS;
    return Math.min(MAX_ELAPSED_MS, Math.max(0, this.now() - set.barBegan));
  }

  moving(index) {
    const set = this.states.get(index);
    return set !== undefined && this.elapsed(index) < GATES[index].seconds[set.state] * 1000;
  }

  openness(index) {
    const name = this.stateName(index);
    const set = this.states.get(index);
    if (!set) {
      return isOpenState(name) ? 1 : 0;
    }
    const seconds = GATES[index].seconds[set.state];
    const done = seconds > 0 ? Math.min(1, this.elapsed(index) / (seconds * 1000)) : 1;
    return isOpenState(name) ? done : 1 - done;
  }

  barMoving(index) {
    return this.barElapsed(index) < BAR_SECONDS * 1000;
  }

  move(gate, state, withBar) {
    const index = gate.index;
    const name = GATES[index].states[state];
    if (name === undefined || this.moving(index) || this.barMoving(index)) {
      return false;
    }
    if (this.stateName(index) === name) {
      return false;
    }
    const barOut = name === GATES[index].bar && !withBar;
    this.states.set(index, { state, began: this.now(), barOut, barBegan: null });
    this.tellAll(index);
    this.host.emit('gateChanged', gate, name);
    return true;
  }

  setBar(gate, barred) {
    const index = gate.index;
    const set = this.states.get(index);
    const bar = GATES[index].bar;
    if (bar === null || !set || GATES[index].states[set.state] !== bar) {
      return false;
    }
    if (set.barOut === !barred || this.moving(index) || this.barMoving(index)) {
      return false;
    }
    set.barOut = !barred;
    set.barBegan = this.now();
    this.tellAll(index);
    this.host.emit('gateBarChanged', gate, barred);
    return true;
  }

  tellAll(index) {
    for (const player of this.host.players.values()) {
      if (player.ready) {
        this.send(player, index);
      }
    }
  }

  send(player, index) {
    const set = this.states.get(index);
    this.host.bridge.send({
      op: 'gateState',
      id: player.id,
      gate: index,
      state: set.state,
      elapsedMs: Math.round(this.elapsed(index)),
      barOut: set.barOut,
      barElapsedMs: Math.round(this.barElapsed(index)),
    });
  }

  // A player's world is ready: their game shows the level's gates, so it hears each set one.
  arrived(player) {
    for (const index of this.states.keys()) {
      this.send(player, index);
    }
  }
}

module.exports = { Gates, GATES };
