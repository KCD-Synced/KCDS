'use strict';

// Conversations the server drives on a player (Dialogue), shown in the
// game's own dialogue list (the HUD's interactive dialogue) with an optional line above it (the
// HUD's subtitles).
//
//   Dialogue.open(player, page, options?)  -> session id, or 0 for an unusable page or a player
//                                             who is gone or still loading
//   Dialogue.update(session, page)         -> false once the session ended or for an unusable page
//   Dialogue.close(session)                -> false once the session ended
//   Dialogue.activeSession(player)             -> the player's session, or 0
//
//   events: dialogueChoice(session, player, optionId)
//           dialogueClosed(session, player, reason): 0 completed (Dialogue.close), 1 the player
//           left, 2 a new conversation took its place, 3 something cut it short.
//
// A page is { line?, onRight?, options: [{ id, text, enabled? }] } with one to eight rows and
// unique ids. The wire carries a row's index in the page it was shown in, with the page's
// generation; the pick comes back as the row's id, and a pick against a page that was replaced
// meanwhile, a second pick of one page or a pick of a row drawn disabled is dropped. `enabled`
// draws the row greyed; it is a hint, a handler checks what a row costs itself.
//
// Each player can have one conversation; starting a new one ends the old one with reason 2. The
// session ends with reason 3 when the player disconnects, is knocked out (playerDied) or their
// world loads again, when the resource that opened it stops, and when the player's game ends it
// (its own conversation began, or the list could not be shown). Options, a KCDS addition, tie it
// to more: `npc` (a server Npc handle or its id) names the speaker of the line and ends the session
// when that NPC dies or is removed; `npc` or `position` with `radius` (metres, 4 by default) end it
// when the player walks out of reach; `timeout` (seconds, 0 none) ends it when a page goes that
// long without a pick.
//
//   { op: 'dialoguePage', id, session, generation, onRight, line, speaker, options: [{ text,
//     enabled }] }
//   { op: 'dialogueClose', id, session }
//   from the client: { op: 'dialogueChoice', id, session, generation, index }
//                    { op: 'dialogueCancel', id, session, reason: 1 | 3 }

const MAX_OPTIONS = 8;
const MAX_OPTION_TEXT = 512;
const MAX_LINE = 1024;
const MAX_SPEAKER = 64;
const MAX_ID = 128;
const DEFAULT_RADIUS = 4;
const MAX_TIMEOUT_SECONDS = 3600;
// How often the players' reach is checked while a conversation is tied to a place.
const REACH_CHECK_MS = 500;

const CLOSE_COMPLETED = 0;
const CLOSE_CANCELLED = 1;
const CLOSE_REPLACED = 2;
const CLOSE_INTERRUPTED = 3;

// The UTF-8 length of a string, as the wire counts it.
function byteLength(text) {
  return Buffer.byteLength(text, 'utf8');
}

// The page as the session keeps it, or null for an unusable one.
function pageOf(page) {
  if (!page || typeof page !== 'object' || !Array.isArray(page.options)) {
    return null;
  }
  const { line, onRight, options } = page;
  if (line !== undefined && (typeof line !== 'string' || byteLength(line) > MAX_LINE)) {
    return null;
  }
  if (onRight !== undefined && typeof onRight !== 'boolean') {
    return null;
  }
  if (options.length < 1 || options.length > MAX_OPTIONS) {
    return null;
  }
  const ids = new Set();
  const rows = [];
  for (const option of options) {
    if (!option || typeof option !== 'object') {
      return null;
    }
    const { id, text, enabled } = option;
    if (typeof id !== 'string' || id === '' || id.length > MAX_ID || ids.has(id)) {
      return null;
    }
    if (typeof text !== 'string' || text === '' || byteLength(text) > MAX_OPTION_TEXT) {
      return null;
    }
    if (enabled !== undefined && typeof enabled !== 'boolean') {
      return null;
    }
    ids.add(id);
    rows.push({ id, text, enabled: enabled !== false });
  }
  return { line: line ?? '', onRight: onRight === true, options: rows };
}

function finitePoint(value) {
  return (
    value !== null &&
    typeof value === 'object' &&
    Number.isFinite(value.x) &&
    Number.isFinite(value.y) &&
    Number.isFinite(value.z)
  );
}

function distance(a, b) {
  return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

// A speaker name cut to what the wire carries, on a character boundary.
function speakerOf(name) {
  let speaker = typeof name === 'string' ? name : '';
  while (byteLength(speaker) > MAX_SPEAKER) {
    speaker = speaker.slice(0, -1);
  }
  return speaker;
}

class Dialogues {
  constructor(host) {
    this.host = host;
    this.sessions = new Map(); // session id -> session
    this.byPlayer = new Map(); // player id -> session id
    this.nextSession = 1;
    this.reachTimer = null;
  }

  playerOf(value) {
    const id = typeof value === 'number' ? value : value?.id;
    if (!Number.isInteger(id)) {
      return null;
    }
    const player = this.host.players.get(id);
    return player && (typeof value === 'number' || player === value) ? player : null;
  }

  // The options of open, or null for unusable ones (throws on a wrong type).
  tiesOf(options) {
    if (options === undefined) {
      return { npc: null, position: null, radius: 0, timeout: 0 };
    }
    if (options === null || typeof options !== 'object') {
      throw new TypeError('Dialogue.open options must be an object');
    }
    let npc = null;
    if (options.npc !== undefined) {
      const id = typeof options.npc === 'number' ? options.npc : options.npc?.id;
      npc = this.host.npcs.npcs.get(id) ?? null;
      if (!npc) {
        throw new TypeError('Dialogue.open options.npc must be a server NPC');
      }
    }
    let position = null;
    if (options.position !== undefined) {
      if (!finitePoint(options.position)) {
        throw new TypeError('Dialogue.open options.position must be { x, y, z }');
      }
      position = { x: options.position.x, y: options.position.y, z: options.position.z };
    }
    const tied = npc !== null || position !== null;
    const radius = options.radius ?? (tied ? DEFAULT_RADIUS : 0);
    if (!Number.isFinite(radius) || radius < 0 || (radius > 0 && !tied)) {
      throw new RangeError('Dialogue.open options.radius needs npc or position and is >= 0');
    }
    const timeout = options.timeout ?? 0;
    if (!Number.isFinite(timeout) || timeout < 0 || timeout > MAX_TIMEOUT_SECONDS) {
      throw new RangeError(`Dialogue.open options.timeout is 0 to ${MAX_TIMEOUT_SECONDS} seconds`);
    }
    return { npc, position, radius, timeout };
  }

  open(playerValue, page, options) {
    const ties = this.tiesOf(options);
    const player = this.playerOf(playerValue);
    const shown = pageOf(page);
    if (!player || !player.ready || !shown) {
      return 0;
    }
    if (ties.npc && (!this.host.npcs.live(ties.npc) || !ties.npc.alive)) {
      return 0;
    }
    const previous = this.byPlayer.get(player.id);
    if (previous !== undefined) {
      this.end(this.sessions.get(previous), CLOSE_REPLACED, true);
    }
    // A dialogueClosed handler may have opened another one already: this one replaces it too.
    const again = this.byPlayer.get(player.id);
    if (again !== undefined) {
      this.end(this.sessions.get(again), CLOSE_REPLACED, true);
    }
    if (!player.ready || this.host.players.get(player.id) !== player) {
      return 0;
    }
    const session = {
      id: this.nextSession++,
      player,
      owner: this.host.context.getStore()?.name ?? null,
      generation: 0,
      page: null,
      answered: false,
      timer: null,
      ...ties,
      speaker: ties.npc ? speakerOf(ties.npc.name) : '',
    };
    if (this.nextSession > 0xffffffff) {
      this.nextSession = 1;
    }
    this.sessions.set(session.id, session);
    this.byPlayer.set(player.id, session.id);
    this.show(session, shown);
    this.watchReach();
    return session.id;
  }

  update(sessionId, page) {
    const session = this.sessions.get(sessionId);
    const shown = pageOf(page);
    if (!session || !shown) {
      return false;
    }
    this.show(session, shown);
    return true;
  }

  close(sessionId) {
    const session = this.sessions.get(sessionId);
    if (!session) {
      return false;
    }
    this.end(session, CLOSE_COMPLETED, true);
    return true;
  }

  activeSession(playerValue) {
    const player = this.playerOf(playerValue);
    return (player && this.byPlayer.get(player.id)) || 0;
  }

  // Sends the page under a new generation and starts its timeout.
  show(session, page) {
    session.generation = session.generation >= 0xffffffff ? 1 : session.generation + 1;
    session.page = page;
    session.answered = false;
    this.host.bridge.send({
      op: 'dialoguePage',
      id: session.player.id,
      session: session.id,
      generation: session.generation,
      onRight: page.onRight,
      line: page.line,
      speaker: session.speaker,
      options: page.options.map((option) => ({ text: option.text, enabled: option.enabled })),
    });
    clearTimeout(session.timer);
    session.timer = null;
    if (session.timeout > 0) {
      session.timer = setTimeout(() => {
        if (this.sessions.get(session.id) === session) {
          this.end(session, CLOSE_INTERRUPTED, true);
        }
      }, session.timeout * 1000);
      session.timer.unref?.();
    }
  }

  end(session, reason, tellClient) {
    if (!session || this.sessions.get(session.id) !== session) {
      return;
    }
    this.sessions.delete(session.id);
    if (this.byPlayer.get(session.player.id) === session.id) {
      this.byPlayer.delete(session.player.id);
    }
    clearTimeout(session.timer);
    session.timer = null;
    const player = session.player;
    if (tellClient && player.ready && this.host.players.get(player.id) === player) {
      this.host.bridge.send({ op: 'dialogueClose', id: player.id, session: session.id });
    }
    this.watchReach();
    this.host.emit('dialogueClosed', session.id, player, reason);
  }

  // dialogueChoice from the player's game: row `index` of the page `generation`.
  choice(player, sessionId, generation, index) {
    const session = this.sessions.get(sessionId);
    if (!player || !session || session.player !== player) {
      return false;
    }
    const option = session.page.options[index];
    if (generation !== session.generation || session.answered || !option || !option.enabled) {
      return false;
    }
    session.answered = true;
    clearTimeout(session.timer);
    session.timer = null;
    this.host.emit('dialogueChoice', session.id, player, option.id);
    return true;
  }

  // dialogueCancel from the player's game: it took the list off itself.
  cancelled(player, sessionId, reason) {
    const session = this.sessions.get(sessionId);
    if (!player || !session || session.player !== player) {
      return false;
    }
    const why = reason === CLOSE_CANCELLED ? CLOSE_CANCELLED : CLOSE_INTERRUPTED;
    this.end(session, why, false);
    return true;
  }

  sessionsOfPlayer(player) {
    const id = this.byPlayer.get(player.id);
    return id === undefined ? null : this.sessions.get(id);
  }

  // The player disconnects, is knocked out or loads a world again.
  left(player) {
    this.end(this.sessionsOfPlayer(player), CLOSE_INTERRUPTED, false);
  }

  knockedOut(player) {
    this.end(this.sessionsOfPlayer(player), CLOSE_INTERRUPTED, true);
  }

  arrived(player) {
    // The new world's game shows nothing of the old one.
    this.end(this.sessionsOfPlayer(player), CLOSE_INTERRUPTED, false);
  }

  // The NPC a conversation is tied to died or is being removed.
  npcGone(npc) {
    for (const session of [...this.sessions.values()]) {
      if (session.npc === npc) {
        this.end(session, CLOSE_INTERRUPTED, true);
      }
    }
  }

  resourceStopped(name) {
    for (const session of [...this.sessions.values()]) {
      if (session.owner === name) {
        this.end(session, CLOSE_INTERRUPTED, true);
      }
    }
  }

  // Runs the reach check while a conversation is tied to a place.
  watchReach() {
    const needed = [...this.sessions.values()].some((session) => session.radius > 0);
    if (needed && !this.reachTimer) {
      this.reachTimer = setInterval(() => this.checkReach(), REACH_CHECK_MS);
      this.reachTimer.unref?.();
    } else if (!needed && this.reachTimer) {
      clearInterval(this.reachTimer);
      this.reachTimer = null;
    }
  }

  checkReach() {
    for (const session of [...this.sessions.values()]) {
      if (session.radius <= 0 || this.sessions.get(session.id) !== session) {
        continue;
      }
      const anchor = session.npc ? session.npc.position : session.position;
      const at = session.player.position;
      if (finitePoint(anchor) && finitePoint(at) && distance(anchor, at) > session.radius) {
        this.end(session, CLOSE_INTERRUPTED, true);
      }
    }
  }

  api() {
    const self = this;
    return Object.freeze({
      open: (player, page, options) => self.open(player, page, options),
      update: (session, page) => self.update(session, page),
      close: (session) => self.close(session),
      activeSession: (player) => self.activeSession(player),
    });
  }
}

module.exports = {
  Dialogues,
  CLOSE_COMPLETED,
  CLOSE_CANCELLED,
  CLOSE_REPLACED,
  CLOSE_INTERRUPTED,
};
