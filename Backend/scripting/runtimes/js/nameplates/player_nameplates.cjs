'use strict';

// What the server says about each player's nameplate: Player.setNameplateVisible,
// setNameplateHealthVisible, setNameplateNameVisible, setNameplateMode, setNameplateText,
// setNameplateColor and setNameplateTitle, and their readers.
//
// The whole tag goes down as { op: 'playerNameplate', id, field } and lives in the player's
// NetworkPlayer field "nameplate" (kcd2s/integration/nameplates.hpp): "<visible><health>;<AARRGGBB>;
// <text>", a third flag "<visible><health><name>" when the name is hidden, and a line break,
// "<AARRGGBB>;" and the title when there is one; "" for the default tag. So every client draws the
// same tag, late joiners too. A player's game draws the tags (Client/runtime/nameplates/); each
// player can still hide them all for themselves with the client's Nameplates.

const MAX_TEXT_BYTES = 64;
// Player.showOverheadText: the reserved client event that carries it, and its limits
// (the client's Nameplates label: 256 bytes, an hour).
const OVERHEAD_EVENT = 'kcds:overheadText';
const MAX_OVERHEAD_BYTES = 256;
const DEFAULT_OVERHEAD_MS = 6000;
const MAX_OVERHEAD_MS = 3600000;
const MAX_OVERHEAD_RANGE = 500;
const WHITE = 0xffffffff;
// A title's size in pixels at 1080 high (0: the plate's own).
const MIN_TITLE_SIZE = 8;
const MAX_TITLE_SIZE = 48;
// What a shown tag carries (Player.setNameplateMode): the name and the health bar, one of them, or
// neither.
const MODES = {
  both: { name: true, health: true },
  name: { name: true, health: false },
  health: { name: false, health: true },
  none: { name: false, health: false },
};

function defaults() {
  return {
    visible: true,
    health: true,
    name: true,
    color: WHITE,
    text: '',
    title: '',
    titleColor: 0,
    titleSize: 0,
  };
}

function hex8(color) {
  return color.toString(16).toUpperCase().padStart(8, '0');
}

function modeOf(tag) {
  return Object.keys(MODES).find(
    (mode) => MODES[mode].name === tag.name && MODES[mode].health === tag.health,
  );
}

// The text as the tag draws it: no control characters, at most MAX_TEXT_BYTES UTF-8 bytes, cut
// at a character.
function cleanText(text) {
  let clean = '';
  let bytes = 0;
  for (const character of String(text).replace(/[\u0000-\u001f\u007f]/g, '')) {
    const size = Buffer.byteLength(character, 'utf8');
    if (bytes + size > MAX_TEXT_BYTES) {
      break;
    }
    clean += character;
    bytes += size;
  }
  return clean;
}

function fieldOf(tag) {
  const plain =
    tag.visible &&
    tag.health &&
    tag.name &&
    tag.color === WHITE &&
    tag.text === '' &&
    tag.title === '';
  if (plain) {
    return '';
  }
  const name = tag.name ? '' : '0';
  const title = tag.title === '' ? '' : `\n${hex8(tag.titleColor)};${tag.titleSize};${tag.title}`;
  return `${tag.visible ? 1 : 0}${tag.health ? 1 : 0}${name};${hex8(tag.color)};${
    tag.text
  }${title}`;
}

class PlayerNameplates {
  constructor(host) {
    this.host = host;
    // player id -> their tag, while it is not the default one
    this.tags = new Map();
  }

  // Player.showOverheadText (a KCDS addition): a line above the player's tag on the
  // screens of the players of their dimension within `range` metres, themselves included, for
  // `durationMs`; '' takes it down. Each client draws it as a Nameplates label (the reserved event
  // OVERHEAD_EVENT, never raised to scripts). Players who come near later do not get it: it is a
  // moment's line, as /me and /do. Returns how many players it went to.
  showOverheadText(player, text, durationMs = DEFAULT_OVERHEAD_MS, color = 0, range = 20) {
    if (typeof text !== 'string') {
      throw new TypeError('showOverheadText takes a text');
    }
    if (Buffer.byteLength(text, 'utf8') > MAX_OVERHEAD_BYTES) {
      throw new RangeError(`an overhead text is at most ${MAX_OVERHEAD_BYTES} UTF-8 bytes`);
    }
    if (!Number.isFinite(durationMs) || durationMs < 1 || durationMs > MAX_OVERHEAD_MS) {
      throw new RangeError(`durationMs is 1 to ${MAX_OVERHEAD_MS} milliseconds`);
    }
    if (!Number.isInteger(color) || color < 0 || color > 0xffffffff) {
      throw new TypeError('a colour is packed 0xAARRGGBB');
    }
    if (!Number.isFinite(range) || range <= 0 || range > MAX_OVERHEAD_RANGE) {
      throw new RangeError(`range is more than 0 and at most ${MAX_OVERHEAD_RANGE} metres`);
    }
    const at = player.position;
    if (!at) {
      return 0;
    }
    const line = {
      entity: player.id,
      text,
      durationMs: Math.round(durationMs),
      color: color >>> 0,
    };
    let reached = 0;
    for (const other of this.host.players.values()) {
      const there = other.position;
      if (!there || other.dimension !== player.dimension) {
        continue;
      }
      const distance = Math.hypot(there.x - at.x, there.y - at.y, there.z - at.z);
      if (distance > range || !this.host.clientResourcesRunning.has(other.id)) {
        continue;
      }
      this.host.services.emitToPlayer(other.id, OVERHEAD_EVENT, line);
      reached += 1;
    }
    return reached;
  }

  of(player) {
    return this.tags.get(player.id) ?? defaults();
  }

  change(player, key, value) {
    this.changeAll(player, { [key]: value });
  }

  changeAll(player, values) {
    const tag = { ...this.of(player), ...values };
    const field = fieldOf(tag);
    if (field === '') {
      this.tags.delete(player.id);
    } else {
      this.tags.set(player.id, tag);
    }
    if (this.host.players.get(player.id) === player) {
      this.host.bridge.send({ op: 'playerNameplate', id: player.id, field });
    }
    // Every client's Player reads the tag from its directory (isNameplateVisible, ...).
    this.host.nameplateChanged?.(player.id, field === '' ? null : { ...tag });
  }

  setVisible(player, visible) {
    if (typeof visible !== 'boolean') {
      throw new TypeError('Player.setNameplateVisible takes a boolean');
    }
    this.change(player, 'visible', visible);
  }

  setHealthVisible(player, visible) {
    if (typeof visible !== 'boolean') {
      throw new TypeError('Player.setNameplateHealthVisible takes a boolean');
    }
    this.change(player, 'health', visible);
  }

  setNameVisible(player, visible) {
    if (typeof visible !== 'boolean') {
      throw new TypeError('Player.setNameplateNameVisible takes a boolean');
    }
    this.change(player, 'name', visible);
  }

  // 'both' (the default), 'name', 'health' or 'none': what a shown tag carries.
  setMode(player, mode) {
    if (!Object.hasOwn(MODES, mode)) {
      throw new TypeError(`Player.setNameplateMode takes one of ${Object.keys(MODES).join(', ')}`);
    }
    this.changeAll(player, MODES[mode]);
  }

  modeOf(player) {
    return modeOf(this.of(player));
  }

  setText(player, text) {
    if (text !== undefined && text !== null && typeof text !== 'string') {
      throw new TypeError('Player.setNameplateText takes a text');
    }
    this.change(player, 'text', cleanText(text ?? ''));
  }

  // A title above the name (a role such as "Town guard" or "Admin") in its own colour (packed
  // 0xAARRGGBB; none or 0 for the plate's gold) and size (pixels at 1080 high, 8 to 48; none or 0
  // for the plate's own); '' or none takes it away. A larger title only grows upward.
  setTitle(player, title, color = 0, size = 0) {
    if (title !== undefined && title !== null && typeof title !== 'string') {
      throw new TypeError('Player.setNameplateTitle takes a text');
    }
    if (!Number.isInteger(color) || color < 0 || color > WHITE) {
      throw new TypeError('Player.setNameplateTitle takes a packed 0xAARRGGBB colour');
    }
    const sized = size === 0 || (size >= MIN_TITLE_SIZE && size <= MAX_TITLE_SIZE);
    if (!Number.isInteger(size) || !sized) {
      throw new TypeError(
        `Player.setNameplateTitle takes a size of ${MIN_TITLE_SIZE} to ${MAX_TITLE_SIZE} pixels`,
      );
    }
    const clean = cleanText(title ?? '');
    this.changeAll(player, {
      title: clean,
      titleColor: clean === '' ? 0 : color >>> 0,
      titleSize: clean === '' ? 0 : size,
    });
  }

  setColor(player, color) {
    if (!Number.isInteger(color) || color < 0 || color > WHITE) {
      throw new TypeError('Player.setNameplateColor takes a packed 0xAARRGGBB colour');
    }
    this.change(player, 'color', color >>> 0);
  }

  // A player whose game got ready: their tag again, in case it was set before their body was in
  // the world.
  arrived(player) {
    const tag = this.tags.get(player.id);
    if (tag) {
      this.host.bridge.send({ op: 'playerNameplate', id: player.id, field: fieldOf(tag) });
    }
  }

  left(player) {
    this.tags.delete(player.id);
    this.host.nameplateChanged?.(player.id, null);
  }
}

module.exports = { PlayerNameplates, fieldOf, cleanText, MAX_TEXT_BYTES, MODES };
