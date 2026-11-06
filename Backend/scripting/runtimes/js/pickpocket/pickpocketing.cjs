'use strict';

// Robbing another player with the game's own pickpocketing. The thief's game runs the
// minigame against the victim's body; the server decides whether a theft may start
// (playerPickpocketStart, refusable) and hears how it ended (playerPickpocketCaught,
// playerPickpocketed). The native server passes the thief's reports as { op: 'pickpocket', id,
// kind: request | caught | seen | finished, victim } and takes its rules as { op:
// 'pickpocketRule', id, kind: enabled | answer | noticed, flag, player }.
//
// KCDS additions: Pickpocketing.setEnabled (whether the game offers Rob on other players at all)
// and Pickpocketing.setVictimNotice (whether a victim who caught a thief is told so; its text is
// the Locales key 'pickpocket.victimNotice', in the victim's language).

// The Locales key of the victim's notice.
const VICTIM_NOTICE = 'pickpocket.victimNotice';

const { isKeyring } = require('../inventory/catalog.cjs');

class Pickpocketing {
  constructor(host) {
    this.host = host;
    this.enabled = true;
    this.victimNotice = true;
    // Whether the game marks items as stolen at all (Inventory.setStolenMarks).
    this.stolenMarks = true;
    // thief id -> victim id, from an allowed request until the theft ended.
    this.thefts = new Map();
    // thief id -> the lines settled so far in the theft under way (or just ended).
    this.lines = new Map();
    // How long after a theft ends its last claims may still arrive (the thief's game reports what
    // it gained with its inventory, apart from the result).
    this.settleMs = 1500;
  }

  api() {
    const self = this;
    return Object.freeze({
      // Whether the game offers Rob on other players.
      get enabled() {
        return self.enabled;
      },
      setEnabled(enabled) {
        if (typeof enabled !== 'boolean') {
          throw new TypeError('setEnabled takes true or false');
        }
        if (self.enabled === enabled) return;
        self.enabled = enabled;
        if (!enabled) self.thefts.clear();
        for (const player of self.host.players.values()) self.sendEnabled(player);
      },
      // Whether a player who caught a thief at their pockets is told so (the game's notice), and
      // with which text ({thief} stands for the thief's name).
      get victimNotice() {
        return self.victimNotice;
      },
      // The notice's text (English, or the server's for 'english'); per language it is the
      // Locales key 'pickpocket.victimNotice'.
      get victimNoticeText() {
        return self.host.locales.get(VICTIM_NOTICE);
      },
      setVictimNotice(enabled, text) {
        if (typeof enabled !== 'boolean') {
          throw new TypeError('setVictimNotice takes true or false');
        }
        if (text !== undefined && (typeof text !== 'string' || !text.length)) {
          throw new TypeError('the notice text is a string');
        }
        self.victimNotice = enabled;
        if (text !== undefined) self.host.locales.set('english', { [VICTIM_NOTICE]: text });
      },
    });
  }

  send(player, kind, flag, other = 0, text = '') {
    if (!player?.ready) return;
    const rule = { op: 'pickpocketRule', id: player.id, kind, flag, player: other };
    // Only a victim's notice carries a text (at most 160 bytes on the wire).
    if (text) rule.notice = Buffer.from(text, 'utf8').subarray(0, 160).toString('utf8');
    this.host.bridge.send(rule);
  }

  // What the victim carries and does not wear, for the thief's loot screen (sent before the
  // answer that lets the thief's game start).
  sendPockets(thief, victim) {
    const inventory = this.host.playerInventories.entryOf(victim)?.inventory;
    const rows = [];
    for (const row of inventory?.rows.values() ?? []) {
      const free = row.amount - (row.equipped ?? 0);
      if (free < 1 || isKeyring(row.itemClass) || rows.length >= 64) continue;
      rows.push([
        this.host.customItems.shownClassOf(row),
        free,
        row.props.quality,
        row.props.health,
      ]);
    }
    this.host.bridge.send({ op: 'pickpocketPockets', id: thief.id, victim: victim.id, rows });
  }

  sendEnabled(player) {
    this.send(player, 'enabled', this.enabled);
  }

  // A player's world is ready: their game learns Rob is off (it offers Rob until told otherwise).
  arrived(player) {
    if (!this.enabled) this.sendEnabled(player);
    if (!this.stolenMarks) this.send(player, 'stolenMarks', false);
  }

  // Inventory.setStolenMarks: whether the game marks items as stolen (the red hand, a merchant's
  // stolen-goods price, the guards' search); off, no item is ever stolen to any player's game.
  setStolenMarks(enabled) {
    if (typeof enabled !== 'boolean') {
      throw new TypeError('setStolenMarks takes true or false');
    }
    if (this.stolenMarks === enabled) return;
    this.stolenMarks = enabled;
    for (const player of this.host.players.values()) this.send(player, 'stolenMarks', enabled);
  }

  // The victim of the theft the player has under way (or that just ended), or null.
  victimOf(thief) {
    const victim = this.thefts.get(thief.id) ?? this.ending?.get(thief.id);
    return victim === undefined ? null : this.host.players.get(victim) ?? null;
  }

  // Lines the server moved from the victim to the thief.
  settled(thief, lines) {
    const list = this.lines.get(thief.id) ?? [];
    for (const line of lines) {
      const same = list.find((l) => l.item === line.item);
      if (same) same.amount += line.amount;
      else list.push({ ...line });
    }
    this.lines.set(thief.id, list);
  }

  left(player) {
    this.thefts.delete(player.id);
    for (const [thief, victim] of this.thefts) {
      if (victim === player.id) this.thefts.delete(thief);
    }
  }

  // A report of a thief's client.
  report(thief, e) {
    if (!thief) return;
    const victim = this.host.players.get(Number(e.victim));
    if (e.kind === 'request') {
      const allowed =
        this.enabled &&
        Boolean(victim?.ready) &&
        victim !== thief &&
        this.host.emit('playerPickpocketStart', thief, victim);
      if (allowed) {
        this.thefts.set(thief.id, victim.id);
        this.lines.delete(thief.id);
        this.sendPockets(thief, victim);
      }
      this.send(thief, 'answer', allowed, Number(e.victim));
      return;
    }
    if (!victim || this.thefts.get(thief.id) !== victim.id) return;
    if (e.kind === 'caught') {
      this.thefts.delete(thief.id);
      this.host.emit('playerPickpocketCaught', thief, victim);
      if (this.victimNotice) {
        const text = this.host.locales.text(victim, VICTIM_NOTICE, {
          thief: thief.nickname ?? '',
        });
        this.send(victim, 'noticed', true, thief.id, text);
      }
    } else if (e.kind === 'finished') {
      // Claims the thief's game reports a moment later still belong to this theft.
      this.thefts.delete(thief.id);
      this.ending ??= new Map();
      this.ending.set(thief.id, victim.id);
      const done = () => {
        if (this.ending.get(thief.id) !== victim.id) return;
        this.ending.delete(thief.id);
        const items = Object.freeze((this.lines.get(thief.id) ?? []).map((l) => Object.freeze(l)));
        this.lines.delete(thief.id);
        this.host.emit('playerPickpocketed', thief, victim, items);
      };
      if (this.settleMs > 0) setTimeout(done, this.settleMs).unref?.();
      else done();
    }
  }
}

module.exports = { Pickpocketing };
