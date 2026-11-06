'use strict';

// The level's beehives are the server's: every player sees the same honey, and what one
// takes is gone for the others. Left to the game, each player's game fills a hive by itself
// (InventoryPreset inventory_beeHive, about ten items), so each could take all of it.
//
// A hive becomes a server container (Stash.adopt) when a player first comes near it, so a server pays
// only for the hives players reach. Its honey grows back to the full stock on a timer.

const fs = require('node:fs');
const path = require('node:path');

const LEVEL_CHESTS = path.join(__dirname, 'level_chests.json');
const HIVE_MODEL = /\/beehive_[a-z]+\.cgf$/i;
const HONEY = 'honey';
const FULL_STOCK = 10;
// In metres: a hive is adopted before a player can reach it.
const ADOPT_REACH = 60;
const CHECK_EVERY_MS = 2000;
// The honey grows back this long after a hive was last topped up.
const REGROW_MS = 60 * 60 * 1000;

function hivesOf(level, rows) {
  return rows
    .filter(([own, , , , , model]) => own === level && HIVE_MODEL.test(model))
    .map(([, guid, x, y]) => ({ guid, x, y, adopted: false, toppedUpAt: 0 }));
}

function honeyIn(stash) {
  const items = stash.getInventory()?.items ?? [];
  let amount = 0;
  for (const row of items) {
    if (row.itemClass === HONEY || row.name === HONEY) {
      amount += row.amount ?? 1;
    }
  }
  return amount;
}

class Beehives {
  // api: the Stash class; players: () => the players to look around; level: () => the level's name.
  constructor({ Stash, players, level, now = Date.now, rows = null }) {
    this.Stash = Stash;
    this.players = players;
    this.level = level;
    this.now = now;
    this.rows = rows;
    this.hives = null;
    this.timer = null;
  }

  start() {
    if (this.timer) {
      return;
    }
    this.timer = setInterval(() => this.tick(), CHECK_EVERY_MS);
    this.timer.unref?.();
  }

  stop() {
    clearInterval(this.timer);
    this.timer = null;
  }

  tick() {
    if (!this.hives) {
      this.rows ??= JSON.parse(fs.readFileSync(LEVEL_CHESTS, 'utf8'));
      this.hives = hivesOf(this.level(), this.rows);
    }
    const players = this.players();
    for (const hive of this.hives) {
      if (!hive.adopted) {
        const near = players.some((player) => {
          const at = player.position;
          return at && Math.hypot(at.x - hive.x, at.y - hive.y) <= ADOPT_REACH;
        });
        if (near) {
          this.adopt(hive);
        }
      } else if (this.now() - hive.toppedUpAt >= REGROW_MS) {
        this.topUp(hive);
      }
    }
  }

  adopt(hive) {
    hive.adopted = true;
    this.topUp(hive);
  }

  // A script may have taken the hive over or given it back: its stock is then left alone.
  topUp(hive) {
    hive.toppedUpAt = this.now();
    const stash = this.Stash.adopt(hive.guid);
    if (!stash) {
      return;
    }
    const missing = FULL_STOCK - honeyIn(stash);
    if (missing > 0) {
      stash.addItem({ item: HONEY, amount: missing });
    }
  }
}

module.exports = { Beehives, FULL_STOCK, REGROW_MS };
