'use strict';

// Inventory.beforeCommit: what a player's own action does to inventories (taking up a ground
// item, dropping, eating, a shop basket, a container's rows, gear on a horse) waits for the
// resources' verdict before it changes anything. Each guard gets a transaction, a plain object that
// says what would move, and a verdict callback: returning true or false decides at once, a promise
// of true or false decides when it settles, and otherwise the guard calls verdict(true) or
// verdict(false, reason) later (from Lua or C# too). Every guard must allow it. A player's actions
// wait in a line and commit one after the other, each against the inventories as they are then,
// so a refusal for a change that no longer fits comes from the action itself. Without any guard an
// action commits at once, as it always did.

const { runningResource } = require('../lifecycle/owned_objects.cjs');
const { metadataOf } = require('./inventories.cjs');

// A transaction's line for `amount` units of row `id` of an inventory: { item, name, amount,
// metadata, row, from, to }, or null when the inventory has no such row or not that many units.
function rowLine(inventory, id, amount, from, to) {
  const row = inventory?.rows?.get(id);
  if (!row || !Number.isInteger(amount) || amount < 1 || amount > row.amount) {
    return null;
  }
  return {
    item: row.itemClass.id,
    name: row.itemClass.name,
    amount,
    metadata: metadataOf(row.itemClass, row.props),
    row: id,
    from,
    to,
  };
}

// How long the guards have for one transaction (a player's game waits 10 s for the verdict on a
// use), and how many of one player's actions may wait.
const VERDICT_MS = 5000;
const MAX_WAITING = 8;
const MAX_REASON_LENGTH = 128;

class CommitGuards {
  constructor(host, { verdictMs = VERDICT_MS } = {}) {
    this.host = host;
    this.verdictMs = verdictMs;
    this.guards = new Map();
    this.nextGuard = 1;
    this.nextTransaction = 1;
    // Player id -> { running, waiting: [action], keys: Set }.
    this.lines = new Map();
  }

  api() {
    return {
      beforeCommit: (handler) => this.add(handler),
      stopBeforeCommit: (id) => this.remove(id),
    };
  }

  add(handler) {
    if (typeof handler !== 'function') {
      throw new TypeError('handler must be a function');
    }
    const id = this.nextGuard++;
    this.guards.set(id, {
      fn: handler,
      owner: this.host.context.getStore(),
      resource: runningResource(this.host),
    });
    return id;
  }

  remove(id) {
    return this.guards.delete(id);
  }

  // A stopping resource's guards go with it.
  resourceStopped(name) {
    for (const [id, guard] of this.guards) {
      if (guard.resource === name) {
        this.guards.delete(id);
      }
    }
  }

  // Runs a player's action. `describe()` returns what it would move ({ kind, lines, ... }), or null
  // for an action that cannot happen as asked (it then commits at once and refuses itself).
  // `commit()` does it, `refuse(code)` tells the player's game it did not happen. `key` names the
  // action: one whose key is still waiting is the same action again, and is left out.
  run(player, key, describe, commit, refuse) {
    if (this.guards.size === 0 || !player) {
      commit();
      return;
    }
    let line = this.lines.get(player.id);
    if (!line) {
      line = { running: false, waiting: [], keys: new Set() };
      this.lines.set(player.id, line);
    }
    if (key && line.keys.has(key)) {
      return;
    }
    if (line.waiting.length >= MAX_WAITING) {
      refuse('busy');
      return;
    }
    if (key) {
      line.keys.add(key);
    }
    line.waiting.push({ key, describe, commit, refuse });
    if (!line.running) {
      this.next(player, line);
    }
  }

  next(player, line) {
    const action = line.waiting.shift();
    if (!action) {
      line.running = false;
      this.lines.delete(player.id);
      return;
    }
    line.running = true;
    const done = () => {
      if (action.key) {
        line.keys.delete(action.key);
      }
      this.next(player, line);
    };
    let transaction = null;
    try {
      transaction = action.describe();
    } catch (error) {
      this.host.bridge.log('host', error.stack ?? String(error));
    }
    if (!transaction || this.guards.size === 0) {
      this.settle(action, { allowed: true });
      done();
      return;
    }
    const entry = this.host.playerInventories.entryOf(player);
    const described = Object.freeze({
      id: `${player.id}:${this.nextTransaction++}`,
      player,
      revision: entry ? entry.inventory.revision : 0,
      money: 0,
      ...transaction,
      lines: Object.freeze((transaction.lines ?? []).map((one) => Object.freeze({ ...one }))),
    });
    this.decide(described).then((verdict) => {
      this.settle(action, verdict);
      done();
    });
  }

  settle(action, verdict) {
    try {
      if (verdict.allowed) {
        action.commit();
      } else {
        action.refuse(verdict.code);
      }
    } catch (error) {
      this.host.bridge.log('host', error.stack ?? String(error));
    }
  }

  // Every guard's verdict on a transaction: { allowed, code, reason }. The first refusal decides;
  // a guard that throws refuses, and one that has not decided in time refuses with 'timeout'.
  decide(transaction) {
    const guards = [...this.guards.values()];
    if (guards.length === 0) {
      return Promise.resolve({ allowed: true });
    }
    return new Promise((resolve) => {
      let open = guards.length;
      let finished = false;
      const finish = (verdict) => {
        if (finished) {
          return;
        }
        finished = true;
        clearTimeout(timer);
        resolve(verdict);
      };
      const timer = setTimeout(() => {
        finish({ allowed: false, code: 'timeout', reason: '' });
      }, this.verdictMs);
      timer.unref?.();
      for (const guard of guards) {
        let decided = false;
        const verdict = (allowed, reason) => {
          if (decided) {
            return;
          }
          decided = true;
          if (allowed !== true) {
            const text = typeof reason === 'string' ? reason.slice(0, MAX_REASON_LENGTH) : '';
            finish({ allowed: false, code: 'refused', reason: text });
            return;
          }
          open -= 1;
          if (open === 0) {
            finish({ allowed: true });
          }
        };
        const failed = (error) => {
          this.host.bridge.log(guard.resource ?? 'host', error?.stack ?? String(error));
          verdict(false, 'error');
        };
        try {
          const result = this.host.context.run(guard.owner, () => guard.fn(transaction, verdict));
          if (result === true || result === false) {
            verdict(result);
          } else if (result && typeof result.then === 'function') {
            result.then((value) => {
              if (value === true || value === false) {
                verdict(value);
              }
            }, failed);
          }
        } catch (error) {
          failed(error);
        }
      }
    });
  }
}

module.exports = { CommitGuards, VERDICT_MS, rowLine };
