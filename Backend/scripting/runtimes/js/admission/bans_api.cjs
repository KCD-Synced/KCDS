'use strict';

// Bans for server scripts: the Bans global and player.ban. They are this server's own bans
// (bans.json, the list KCDS-Admin's Ban keeps; admission/server_bans.cjs), checked when a player
// connects: a banned player is turned away with "You are banned from this server: <reason>".
//
// A script's ban has a reason and no end. A server that bans for a time keeps its own times and
// lifts the ban with Bans.remove when the time is up; KCDS keeps no clock for it.
//
// A ban names identifiers as "kind:id" (steam, epic, xbox, gog, discord, hardware, ip).
// player.ban takes every identifier of the player but the address: behind a shared NAT the address
// would ban strangers too.

const { IDENTIFIER } = require('./server_bans.cjs');

// Who a script's ban is by, in the list KCDS-Admin shows.
const BY = 'script';
// The most bytes a session end carries to the player (kKickReasonBytes).
const NOTICE_BYTES = 200;
const MAX_REASON_BYTES = 300;

// The text cut to whole characters within the bytes a session end carries.
function noticeOf(text) {
  let cut = '';
  for (const character of text) {
    if (Buffer.byteLength(cut + character) > NOTICE_BYTES) {
      break;
    }
    cut += character;
  }
  return cut;
}

class BansApi {
  constructor(host) {
    this.host = host;
  }

  get bans() {
    return this.host.serverBans;
  }

  // A ban as a script reads it: { id, identifiers, reason, by, at } (at in milliseconds), and
  // `until` for a ban KCDS-Admin made for a time (0 for none).
  view(ban) {
    return {
      id: ban.id,
      identifiers: [...ban.identifiers],
      reason: ban.reason,
      by: ban.by,
      at: ban.at,
      until: ban.until,
    };
  }

  reasonOf(reason, method) {
    if (reason === undefined) {
      return '';
    }
    if (typeof reason !== 'string' || Buffer.byteLength(reason) > MAX_REASON_BYTES) {
      throw new TypeError(`${method} takes a reason of at most ${MAX_REASON_BYTES} bytes`);
    }
    return reason;
  }

  // The connected players any of these identifiers names leave with the ban's text.
  sendAway(ban) {
    const keys = new Set(ban.identifiers);
    for (const player of [...this.host.players.values()]) {
      const named = player.identifiers.some(({ kind, id }) => keys.has(`${kind}:${id}`));
      if (named) {
        this.host.disconnectPlayer(player, noticeOf(this.bans.refusal(ban)), undefined, true);
      }
    }
  }

  // Bans.add(identifiers, reason): a ban of these identifiers ("kind:id", 1 to 8); a connected
  // player any of them names leaves at once. Returns the ban.
  add(identifiers, reason) {
    const list = typeof identifiers === 'string' ? [identifiers] : identifiers;
    if (
      !Array.isArray(list) ||
      list.length === 0 ||
      list.length > 8 ||
      !list.every((identifier) => typeof identifier === 'string' && IDENTIFIER.test(identifier))
    ) {
      throw new TypeError('Bans.add takes 1 to 8 identifiers such as "steam:76561198000000001"');
    }
    const ban = this.bans.add({
      identifiers: list,
      reason: this.reasonOf(reason, 'Bans.add'),
      by: BY,
      minutes: 0,
    });
    this.sendAway(ban);
    return this.view(ban);
  }

  // player.ban(reason): a ban of the player's identifiers but the address; they leave at once with
  // its text. Returns the ban, or null when the player is gone or has no identifier to ban.
  banPlayer(player, reason) {
    const text = this.reasonOf(reason, 'Player.ban');
    if (this.host.players.get(player.id) !== player) {
      return null;
    }
    const identifiers = player.identifiers
      .filter(({ kind }) => kind !== 'ip')
      .map(({ kind, id }) => `${kind}:${id}`)
      .slice(0, 8);
    if (identifiers.length === 0) {
      return null;
    }
    const ban = this.bans.add({ identifiers, reason: text, by: BY, minutes: 0 });
    this.sendAway(ban);
    return this.view(ban);
  }

  // Bans.remove(idOrIdentifier): lifts a ban by its id, or every ban naming an identifier
  // ("kind:id"). True when one was lifted.
  remove(key) {
    if (typeof key !== 'string' || key === '') {
      throw new TypeError('Bans.remove takes a ban id or an identifier such as "steam:7656..."');
    }
    if (this.bans.remove(key)) {
      return true;
    }
    if (!IDENTIFIER.test(key)) {
      return false;
    }
    const named = this.bans.list().filter((ban) => ban.identifiers.includes(key));
    for (const ban of named) {
      this.bans.remove(ban.id);
    }
    return named.length > 0;
  }

  // Bans.find(identifierOrPlayer): the running ban naming the identifier ("kind:id") or any of the
  // player's identifiers, or null.
  find(target) {
    let identifiers;
    if (typeof target === 'string') {
      if (!IDENTIFIER.test(target)) {
        throw new TypeError('Bans.find takes an identifier such as "steam:7656..." or a player');
      }
      const colon = target.indexOf(':');
      identifiers = [{ kind: target.slice(0, colon), id: target.slice(colon + 1) }];
    } else if (target && Array.isArray(target.identifiers)) {
      identifiers = target.identifiers;
    } else {
      throw new TypeError('Bans.find takes an identifier such as "steam:7656..." or a player');
    }
    const ban = this.bans.match(identifiers);
    return ban ? this.view(ban) : null;
  }

  // Bans.list(): every ban, the running ones first.
  list() {
    return this.bans.list().map((ban) => this.view(ban));
  }

  api() {
    const self = this;
    return Object.freeze({
      add: (identifiers, reason = undefined) => self.add(identifiers, reason),
      remove: (idOrIdentifier) => self.remove(idOrIdentifier),
      find: (identifierOrPlayer) => self.find(identifierOrPlayer),
      list: () => self.list(),
    });
  }
}

module.exports = { BansApi };
