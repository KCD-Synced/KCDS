'use strict';

// Factions and hostility: the server API's Faction, Npc.aggroRange, Npc.hostile, Npc.hostileTo
// and Player.faction.
//
// KCDS NPCs fight only whom the server names (Npc.attack): their game's AI takes no side on its
// faction's account. This module is that side, decided on the server. A faction is a number a
// resource gives to NPCs (Npc.faction) and players (Player.faction); two factions are hostile,
// friendly or neutral to each other (Faction.setRelation), and members of one faction are
// friends. An NPC with an aggro range attacks the nearest player or NPC it is hostile to within
// that range: one of a hostile faction, any player while it is `hostile`, or one it was made
// hostile to (hostileTo). The resources' npcAggro event sees each such attack first and may
// refuse it. An NPC struck in a fight is helped by the NPCs of its own and friendly factions whose
// aggro range reaches it.
//
// The server looks around every SCAN_MS, and only for the NPCs with an aggro range: with none the
// timer does not run. Each look goes through the players and the NPCs that have a faction or are
// someone's chosen foe, so it costs the watchers times the bodies, each a distance.

const { OWNER, runningResource } = require('../lifecycle/owned_objects.cjs');

// How often the NPCs with an aggro range look around (milliseconds).
const SCAN_MS = 500;
// The widest aggro range (metres).
const AGGRO_RANGE_MAX = 50;
// The largest faction number.
const FACTION_MAX = 65535;
// How long an attack the npcAggro event refused is not offered again (milliseconds).
const REFUSED_MS = 5000;
const RELATIONS = ['hostile', 'neutral', 'friendly'];

function factionOf(value, what) {
  if (!Number.isInteger(value) || value < 0 || value > FACTION_MAX) {
    throw new RangeError(`${what} must be a whole number from 0 to ${FACTION_MAX}`);
  }
  return value;
}

// The key of a pair of factions, the same either way round.
function pairOf(a, b) {
  return a < b ? `${a}:${b}` : `${b}:${a}`;
}

class Factions {
  // `npcs` is the registry, `stateKey` the symbol under which an NPC keeps its state.
  constructor(npcs, stateKey) {
    this.npcs = npcs;
    this.host = npcs.host;
    this.stateKey = stateKey;
    // pair key -> { a, b, relation, [OWNER] }: only hostile and friendly pairs are kept.
    this.relations = new Map();
    // player id -> faction (0, none, is not kept).
    this.players = new Map();
    // The NPCs with an aggro range.
    this.watchers = new Set();
    this.timer = null;
  }

  // The Faction namespace.
  define() {
    const factions = this;
    return Object.freeze({
      // Sets how two factions stand to each other, both ways: 'hostile' (an NPC of one with an
      // aggro range attacks a member of the other), 'friendly' (its NPCs help each other in a
      // fight) or 'neutral' (neither; the default). Lasts until changed or the resource that set
      // it stops. Throws a RangeError for a faction that is not a whole number from 1 to 65535 or
      // for an unknown relation, and an Error for one faction with itself.
      setRelation(a, b, relation) {
        factions.setRelation(a, b, relation);
      },
      // How faction `a` stands to faction `b`: 'hostile', 'friendly' or 'neutral'. A faction is
      // friendly to itself, and 0 (no faction) is neutral to every faction.
      relation(a, b) {
        return factions.relation(
          factionOf(a, 'Faction.relation faction'),
          factionOf(b, 'Faction.relation faction'),
        );
      },
      // The pairs of factions that are not neutral: [{ a, b, relation }], a below b.
      relations() {
        return [...factions.relations.values()].map(({ a, b, relation }) => ({ a, b, relation }));
      },
    });
  }

  setRelation(a, b, relation) {
    const first = factionOf(a, 'Faction.setRelation faction');
    const second = factionOf(b, 'Faction.setRelation faction');
    if (first === 0 || second === 0) {
      throw new RangeError('Faction.setRelation takes factions from 1 to 65535');
    }
    if (first === second) {
      throw new Error('Faction.setRelation: a faction is always friendly to itself');
    }
    if (!RELATIONS.includes(relation)) {
      throw new RangeError(`Faction.setRelation relation is one of ${RELATIONS.join(', ')}`);
    }
    const key = pairOf(first, second);
    if (relation === 'neutral') {
      this.relations.delete(key);
      return;
    }
    this.relations.set(key, {
      a: Math.min(first, second),
      b: Math.max(first, second),
      relation,
      [OWNER]: runningResource(this.host),
    });
  }

  relation(a, b) {
    if (a === 0 || b === 0) {
      return 'neutral';
    }
    if (a === b) {
      return 'friendly';
    }
    return this.relations.get(pairOf(a, b))?.relation ?? 'neutral';
  }

  playerFaction(id) {
    return this.players.get(id) ?? 0;
  }

  setPlayerFaction(id, value) {
    const faction = factionOf(value, 'Player.faction');
    if (faction === 0) {
      this.players.delete(id);
    } else {
      this.players.set(id, faction);
    }
  }

  factionOf(body) {
    const state = body[this.stateKey];
    return state ? state.faction : this.playerFaction(body.id);
  }

  setAggroRange(npc, value) {
    if (typeof value !== 'number' || !(value >= 0) || value > AGGRO_RANGE_MAX) {
      throw new RangeError(`Npc.aggroRange is a number of metres from 0 to ${AGGRO_RANGE_MAX}`);
    }
    npc[this.stateKey].aggroRange = value;
    if (value > 0) {
      this.watchers.add(npc);
    } else {
      this.watchers.delete(npc);
    }
    this.schedule();
  }

  // Npc.hostileTo: makes the NPC hostile to one Player or Npc whatever their factions, or not
  // (false). False when the target is gone or the NPC itself.
  setHostileTo(npc, target, hostile) {
    const state = npc[this.stateKey];
    const body = this.npcs.targetOf(npc, target, 'Npc.hostileTo target');
    if (body === null) {
      return false;
    }
    if (!state.foes) {
      state.foes = new Set();
    }
    if (hostile) {
      state.foes.add(body);
    } else {
      state.foes.delete(body);
    }
    return true;
  }

  // Whether `npc` is hostile to `body` (a Player or an Npc).
  hostile(npc, body) {
    const state = npc[this.stateKey];
    if (body === npc) {
      return false;
    }
    if (state.foes?.has(body)) {
      return true;
    }
    const player = !body[this.stateKey];
    if (player && state.hostile) {
      return true;
    }
    return this.relation(state.faction, this.factionOf(body)) === 'hostile';
  }

  // Whether `npc` sides with `friend` in a fight: the same or a friendly faction.
  sidesWith(npc, friend) {
    const faction = npc[this.stateKey].faction;
    return faction !== 0 && this.relation(faction, friend[this.stateKey].faction) === 'friendly';
  }

  // Starts the look-around while an NPC has an aggro range, stops it when none has. The timer
  // belongs to no resource: it runs on whatever resource set the first range.
  schedule() {
    if (this.watchers.size > 0 && this.timer === null) {
      this.timer = this.host.context.exit(() => setInterval(() => this.scan(), SCAN_MS));
      this.timer.unref?.();
    } else if (this.watchers.size === 0 && this.timer !== null) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }

  // Whether the NPC is free to start a fight of its own: alive, with its brain, not fighting.
  ready(npc) {
    const state = npc[this.stateKey];
    if (!state.alive || !state.brain) {
      return false;
    }
    return !(state.intent === 'attack' && state.status === 'running');
  }

  // The bodies an NPC might be hostile to: the players, and the NPCs with a faction or chosen as
  // someone's foe.
  candidates() {
    const chosen = new Set();
    for (const npc of this.watchers) {
      for (const foe of npc[this.stateKey].foes ?? []) {
        chosen.add(foe);
      }
    }
    // Only players with a body in the world.
    const bodies = [...this.host.players.values()].filter((player) => player.ready);
    for (const npc of this.npcs.npcs.values()) {
      if (npc[this.stateKey].faction !== 0 || chosen.has(npc)) {
        bodies.push(npc);
      }
    }
    return bodies;
  }

  scan() {
    for (const npc of [...this.watchers]) {
      if (!this.npcs.live(npc)) {
        this.watchers.delete(npc);
      }
    }
    this.schedule();
    if (this.watchers.size === 0) {
      return;
    }
    const bodies = this.candidates();
    const now = Date.now();
    for (const npc of this.watchers) {
      if (!this.ready(npc)) {
        continue;
      }
      const target = this.nearestFoe(npc, bodies, now);
      if (target !== null) {
        this.aggro(npc, target, now);
      }
    }
  }

  // The nearest body within the NPC's aggro range it is hostile to and may attack, or null.
  nearestFoe(npc, bodies, now) {
    const state = npc[this.stateKey];
    let best = null;
    let bestDistance = Infinity;
    for (const body of bodies) {
      if (body === npc || body.knockedOut === true || body.alive === false) {
        continue;
      }
      if (state.refused?.get(body) > now) {
        continue;
      }
      if (!this.npcs.reaches(npc, body, state.aggroRange) || !this.hostile(npc, body)) {
        continue;
      }
      const there = body.position;
      const distance = Math.hypot(
        there.x - state.position.x,
        there.y - state.position.y,
        there.z - state.position.z,
      );
      if (distance < bestDistance) {
        best = body;
        bestDistance = distance;
      }
    }
    return best;
  }

  // npcAggro(npc, target): false refuses the attack, which is then not offered for REFUSED_MS.
  aggro(npc, target, now) {
    const state = npc[this.stateKey];
    if (!this.host.emit('npcAggro', npc, target)) {
      if (!state.refused) {
        state.refused = new Map();
      }
      state.refused.set(target, now + REFUSED_MS);
      return false;
    }
    if (!this.npcs.live(npc) || !this.ready(npc)) {
      return false;
    }
    return this.host.context.exit(() => this.npcs.attack(npc, target));
  }

  // An NPC was struck in a fight by `attacker`: the NPCs that side with it and whose aggro range
  // reaches it attack the striker, unless they fight already or the striker is a friend of theirs.
  struck(npc, attacker) {
    if (attacker === null || attacker === npc || npc[this.stateKey].faction === 0) {
      return;
    }
    const now = Date.now();
    for (const helper of this.watchers) {
      if (helper === npc || helper === attacker || !this.ready(helper)) {
        continue;
      }
      if (!this.sidesWith(helper, npc)) {
        continue;
      }
      if (attacker[this.stateKey] && this.sidesWith(helper, attacker)) {
        continue;
      }
      if (!this.npcs.reaches(helper, npc, helper[this.stateKey].aggroRange)) {
        continue;
      }
      this.aggro(helper, attacker, now);
    }
  }

  // The NPC is removed: it watches no more, and is nobody's chosen foe.
  forget(npc) {
    this.watchers.delete(npc);
    for (const other of this.npcs.npcs.values()) {
      other[this.stateKey].foes?.delete(npc);
    }
    this.schedule();
  }

  // A player left: their faction is forgotten, and so are they as anyone's chosen foe.
  playerLeft(id) {
    this.players.delete(id);
    for (const npc of this.npcs.npcs.values()) {
      const foes = npc[this.stateKey].foes;
      for (const foe of foes ?? []) {
        if (!foe[this.stateKey] && foe.id === id) {
          foes.delete(foe);
        }
      }
    }
  }

  // The resource stopped: the relations it set end.
  resourceStopped(name) {
    for (const [key, entry] of [...this.relations]) {
      if (entry[OWNER] === name) {
        this.relations.delete(key);
      }
    }
  }
}

module.exports = { Factions, AGGRO_RANGE_MAX, SCAN_MS };
