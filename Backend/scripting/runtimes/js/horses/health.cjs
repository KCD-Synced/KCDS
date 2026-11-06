'use strict';

// The health, death and knock-out of a horse a resource spawned. The horse is in one
// of three states, written to every client in the horse's `state` field (horse.hpp HorseState):
//
//   alive       0   the game's own horse, with `health` from above 0 to MAX_HEALTH
//   knockedOut  1   lying, as a lethal blow leaves a horse spawned with knockOut, or knockDown()
//   dead        2   lying with 0 health
//
// This host keeps the ledger. A resource changes it (health, kill, knockDown, wake, revive) and the
// change goes to the native server as `horseHealth`; the horse's simulator changes it too, when
// the game took health off the horse or killed it (HORSE_HEALTH): the native server checks the
// report and hands it back as horseHealth, which is applied here by the same rules the server
// uses, so a report in flight never undoes a resource's decision:
//   - a dead horse stays dead, only revive() brings it back;
//   - a knocked-out horse only dies (a report cannot wake it, wake() does);
//   - a death is already a knock-out when the horse's knockOut option is set (the server does it).
//
// Events: horseDamage(horse, attacker, amount, reason) when a report took health off or health was
// set lower, horseDeath(horse, killer, reason), horseKnockedOut(horse, attacker, reason),
// horseWake(horse) and horseRevive(horse). The simulator's report names neither attacker nor
// reason, so those read null and 'unknown'; what a resource does reads 'scripted'.

// The health a horse has at most, and starts with (horse.hpp kHorseMaxHealth).
const MAX_HEALTH = 100;
// The values of the native `state` field (horse.hpp kHorseAlive, kHorseKnockedOut, kHorseDead).
const ALIVE = 0;
const KNOCKED_OUT = 1;
const DEAD = 2;

class HorseHealth {
  constructor(host, state) {
    this.host = host;
    // The symbol under which a horse keeps { health, state }.
    this.key = state;
  }

  // The state of a freshly spawned horse: alive at full health.
  fresh() {
    return { health: MAX_HEALTH, state: ALIVE };
  }

  // A health a resource gave, as a number: above MAX_HEALTH is clamped to it. Throws for anything
  // but a finite number from 0 up (above 0 when `positive`).
  healthOf(value, what, positive = false) {
    const least = positive ? value > 0 : value >= 0;
    if (typeof value !== 'number' || !Number.isFinite(value) || !least) {
      const from = positive ? 'above 0' : 'from 0';
      throw new RangeError(`${what} must be a number ${from} to ${MAX_HEALTH}`);
    }
    return Math.min(value, MAX_HEALTH);
  }

  // Whether the horse is still here; throws when the calling resource has been stopped.
  live(horse) {
    if (this.host.horses.get(horse.id) !== horse) {
      return false;
    }
    this.host.requireLivePlayerCall(horse);
    return true;
  }

  // Tells the native server the ledger, which writes the fields every client shows.
  send(horse) {
    const { health, state } = horse[this.key];
    this.host.bridge.send({ op: 'horseHealth', id: horse.id, health, state });
  }

  // A horse that lies takes no order: the one it walked is dropped.
  stopOrder(horse) {
    if (horse[this.host.horseOrders.state].status === 'running') {
      this.host.horseOrders.hold(horse);
    }
  }

  // Horse.health: 0 kills, and writes to a dead horse do nothing. A knocked-out horse keeps lying with
  // the new health.
  setHealth(horse, value) {
    const health = this.healthOf(value, 'Horse.health');
    if (!this.live(horse)) {
      return;
    }
    const ledger = horse[this.key];
    if (ledger.state === DEAD) {
      return;
    }
    if (health === 0) {
      this.die(horse, null, 'scripted');
      return;
    }
    const taken = ledger.health - health;
    ledger.health = health;
    this.send(horse);
    if (taken > 0) {
      this.host.emit('horseDamage', horse, null, taken, 'scripted');
    }
  }

  // Horse.kill(): the horse dies where it stands. False for a dead or gone horse.
  kill(horse) {
    if (!this.live(horse) || horse[this.key].state === DEAD) {
      return false;
    }
    this.die(horse, null, 'scripted');
    return true;
  }

  // Horse.knockDown(): an alive horse is knocked out. False for a horse that lies or is gone.
  knockDown(horse) {
    if (!this.live(horse) || horse[this.key].state !== ALIVE) {
      return false;
    }
    this.fall(horse, KNOCKED_OUT, null, 'scripted');
    return true;
  }

  // Horse.wake(health?): a knocked-out horse gets up with `health`, or with the health it has,
  // full when a lethal blow left it none. False for a horse that does not lie knocked out.
  wake(horse, health) {
    const given =
      health === undefined ? undefined : this.healthOf(health, 'Horse.wake health', true);
    if (!this.live(horse) || horse[this.key].state !== KNOCKED_OUT) {
      return false;
    }
    const ledger = horse[this.key];
    ledger.state = ALIVE;
    ledger.health = given ?? (ledger.health > 0 ? ledger.health : MAX_HEALTH);
    this.send(horse);
    this.host.emit('horseWake', horse);
    return true;
  }

  // Horse.revive(health?): a dead horse comes back where it lies, at full health or at `health`.
  // False for a horse that is not dead.
  revive(horse, health = MAX_HEALTH) {
    const given = this.healthOf(health, 'Horse.revive health', true);
    if (!this.live(horse) || horse[this.key].state !== DEAD) {
      return false;
    }
    const ledger = horse[this.key];
    ledger.state = ALIVE;
    ledger.health = given;
    this.send(horse);
    this.host.emit('horseRevive', horse);
    return true;
  }

  // The horse dies (a resource's doing, so the knockOut option, which is about lethal blows, does
  // not apply).
  die(horse, killer, reason) {
    this.fall(horse, DEAD, killer, reason);
  }

  // The horse goes down to `state` (knocked out or dead) with no health left: sent, its order
  // dropped and the event raised.
  fall(horse, state, killer, reason) {
    const ledger = horse[this.key];
    ledger.state = state;
    ledger.health = 0;
    this.send(horse);
    this.stopOrder(horse);
    this.host.emit(state === DEAD ? 'horseDeath' : 'horseKnockedOut', horse, killer, reason);
  }

  // The native server's report that the horse's simulator changed its health or state (already
  // checked there, the knockOut option applied): { id, health, state }.
  reported(report) {
    const horse = this.host.horses.get(report.id);
    if (
      !horse ||
      !Number.isFinite(report.health) ||
      ![ALIVE, KNOCKED_OUT, DEAD].includes(report.state)
    ) {
      return;
    }
    const ledger = horse[this.key];
    // The same rules as the server's: nothing moves a horse that lies back up.
    if (ledger.state === DEAD || (ledger.state === KNOCKED_OUT && report.state !== DEAD)) {
      return;
    }
    const before = ledger.health;
    // A horse of other hit points than the game's 100 (Horse.hitPoints) loses health
    // scaled by 100 / hitPoints while it stays up; every body is told the health it keeps.
    const hitPoints = horse.hitPoints;
    const scaled =
      report.state === ALIVE && hitPoints && report.health < before
        ? Math.max(before - ((before - report.health) * MAX_HEALTH) / hitPoints, 0.01)
        : report.health;
    ledger.health = scaled;
    ledger.state = report.state;
    if (scaled !== report.health) {
      this.send(horse);
    }
    const taken = before - scaled;
    if (taken > 0) {
      this.host.emit('horseDamage', horse, null, taken, 'unknown');
    }
    // A damage handler may have changed the horse already; only a horse still going down is told.
    if (this.host.horses.get(horse.id) !== horse || report.state === ALIVE) {
      return;
    }
    if (horse[this.key].state !== report.state) {
      return;
    }
    this.stopOrder(horse);
    this.host.emit(
      report.state === DEAD ? 'horseDeath' : 'horseKnockedOut',
      horse,
      null,
      'unknown',
    );
  }
}

module.exports = { HorseHealth, MAX_HEALTH, ALIVE, KNOCKED_OUT, DEAD };
