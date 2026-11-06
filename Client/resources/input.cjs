'use strict';

// The client-only Key and Controls globals.
//
// Keys are named by their place on a US keyboard layout. This table is the only list of
// them: the runtime sends it to the game client once ({ op: 'keyTable' }), which polls exactly
// these keys and reports edges ({ op: 'keyEdges' }) only while binds may fire (in a world, game
// window in front, no chat or focused view capturing typed input). Letters and digits are
// polled by scan code, so they follow the physical key whatever the layout prints; their labels
// come from the player's layout ({ op: 'keyLabels' }). The other keys use their virtual key and
// an English label.
//
// Controls holds are counted per resource; the totals go to the game client
// ({ op: 'controls', disabled, cursor, actions }), which takes gameplay input, shows the pointer or
// keeps the named game actions from doing anything (Controls.blockAction). A
// stopping resource gives back exactly what it held.

// A game action's name as the action maps write it (moveforward, use, opponent_lock, ...).
const ACTION_NAME = /^[A-Za-z0-9_]{1,64}$/;

function actionName(action, where) {
  if (typeof action !== 'string' || !ACTION_NAME.test(action)) {
    throw new TypeError(`${where} takes a game action's name, such as 'use' or 'jump'`);
  }
  return action;
}

// [name, kind, code, label]: kind 'scan' (set 1 scan code), 'vk' (virtual key) or 'pad'.
const KEYS = [];
const ALIASES = {
  control: 'ctrl',
  return: 'enter',
  esc: 'escape',
};

const LETTER_SCANS = {
  q: 0x10,
  w: 0x11,
  e: 0x12,
  r: 0x13,
  t: 0x14,
  y: 0x15,
  u: 0x16,
  i: 0x17,
  o: 0x18,
  p: 0x19,
  a: 0x1e,
  s: 0x1f,
  d: 0x20,
  f: 0x21,
  g: 0x22,
  h: 0x23,
  j: 0x24,
  k: 0x25,
  l: 0x26,
  z: 0x2c,
  x: 0x2d,
  c: 0x2e,
  v: 0x2f,
  b: 0x30,
  n: 0x31,
  m: 0x32,
};
for (const [letter, scan] of Object.entries(LETTER_SCANS)) {
  KEYS.push([letter, 'scan', scan, letter.toUpperCase()]);
}
const DIGIT_SCANS = [0x0b, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a];
DIGIT_SCANS.forEach((scan, digit) => KEYS.push([String(digit), 'scan', scan, String(digit)]));
// The punctuation keys by their place on a US keyboard, by scan code like the letters.
for (const [name, scan, label] of [
  ['grave', 0x29, '`'],
  ['minus', 0x0c, '-'],
  ['equals', 0x0d, '='],
  ['lbracket', 0x1a, '['],
  ['rbracket', 0x1b, ']'],
  ['backslash', 0x2b, '\\'],
  ['semicolon', 0x27, ';'],
  ['apostrophe', 0x28, "'"],
  ['comma', 0x33, ','],
  ['period', 0x34, '.'],
  ['slash', 0x35, '/'],
]) {
  KEYS.push([name, 'scan', scan, label]);
}
for (let n = 1; n <= 12; n++) {
  KEYS.push([`f${n}`, 'vk', 0x6f + n, `F${n}`]);
}
for (let n = 0; n <= 9; n++) {
  KEYS.push([`numpad${n}`, 'vk', 0x60 + n, `Numpad ${n}`]);
  ALIASES[`num${n}`] = `numpad${n}`;
}
for (const [name, vk, label] of [
  ['up', 0x26, 'Up'],
  ['down', 0x28, 'Down'],
  ['left', 0x25, 'Left'],
  ['right', 0x27, 'Right'],
  ['shift', 0x10, 'Shift'],
  ['lshift', 0xa0, 'Left Shift'],
  ['rshift', 0xa1, 'Right Shift'],
  ['ctrl', 0x11, 'Ctrl'],
  ['lctrl', 0xa2, 'Left Ctrl'],
  ['rctrl', 0xa3, 'Right Ctrl'],
  ['alt', 0x12, 'Alt'],
  ['lalt', 0xa4, 'Left Alt'],
  ['ralt', 0xa5, 'Right Alt'],
  ['space', 0x20, 'Space'],
  ['enter', 0x0d, 'Enter'],
  ['escape', 0x1b, 'Escape'],
  ['tab', 0x09, 'Tab'],
  ['backspace', 0x08, 'Backspace'],
  ['insert', 0x2d, 'Insert'],
  ['delete', 0x2e, 'Delete'],
  ['home', 0x24, 'Home'],
  ['end', 0x23, 'End'],
  ['pageup', 0x21, 'Page Up'],
  ['pagedown', 0x22, 'Page Down'],
  ['capslock', 0x14, 'Caps Lock'],
  ['mouse1', 0x01, 'Mouse 1'],
  ['mouse2', 0x02, 'Mouse 2'],
  ['mouse3', 0x04, 'Mouse 3'],
  ['mouse4', 0x05, 'Mouse 4'],
  ['mouse5', 0x06, 'Mouse 5'],
]) {
  KEYS.push([name, 'vk', vk, label]);
}
// Controller buttons by the engine's names: kind 'pad', code an XInput button bit,
// the triggers 0x10000 (left) and 0x20000 (right).
for (const [name, bit, label] of [
  ['xi_dpad_up', 0x0001, 'D-pad Up'],
  ['xi_dpad_down', 0x0002, 'D-pad Down'],
  ['xi_dpad_left', 0x0004, 'D-pad Left'],
  ['xi_dpad_right', 0x0008, 'D-pad Right'],
  ['xi_start', 0x0010, 'Menu'],
  ['xi_back', 0x0020, 'View'],
  ['xi_thumbl', 0x0040, 'Left Stick'],
  ['xi_thumbr', 0x0080, 'Right Stick'],
  ['xi_shoulderl', 0x0100, 'LB'],
  ['xi_shoulderr', 0x0200, 'RB'],
  ['xi_a', 0x1000, 'A'],
  ['xi_b', 0x2000, 'B'],
  ['xi_x', 0x4000, 'X'],
  ['xi_y', 0x8000, 'Y'],
  ['xi_triggerl_btn', 0x10000, 'LT'],
  ['xi_triggerr_btn', 0x20000, 'RT'],
]) {
  KEYS.push([name, 'pad', bit, label]);
}
const KNOWN = new Map(KEYS.map((entry) => [entry[0], entry]));
const STATES = new Set(['down', 'up', 'both']);

// The canonical key name; throws for anything that is not a key.
function keyName(key) {
  if (typeof key !== 'string') {
    throw new TypeError('Key name must be a string');
  }
  const lower = key.toLowerCase();
  const name = ALIASES[lower] ?? lower;
  if (!KNOWN.has(name)) {
    throw new Error(`Unknown key '${key}'`);
  }
  return name;
}

class ResourceInput {
  constructor({ send = () => {}, log = () => {}, emit = () => {} } = {}) {
    this.send = send;
    this.log = log;
    this.emit = emit;
    // Whether the pointer showed when the scripts last heard (cursorVisibilityChanged).
    this.cursorShown = false;
    this.binds = []; // { owner, name, state, handler }
    this.down = new Set(); // keys down while binds may fire
    this.labels = new Map(); // layout labels of the scan keys
    this.captured = false; // chat, a focused view or an overlay holds typed input
    this.pointer = false; // the game client shows the pointer (a focused view)
    this.disableHolds = new Map(); // owner -> count
    this.cursorHolds = new Map(); // owner -> count
    this.cursorAt = null; // where the pointer is while a resource shows it ({ x, y }, -1 .. 1)
    this.actionHolds = new Map(); // owner -> Map(action -> count)
    this.sent = { disabled: false, cursor: false, actions: '' };
    this.tableSent = false;
  }

  // The game client polls keys only once a resource binds or reads one.
  ensureTable() {
    if (this.tableSent) {
      return;
    }
    this.tableSent = true;
    this.send({
      op: 'keyTable',
      keys: KEYS.map(([name, kind, code]) => ({ name, kind, code })),
    });
  }

  total(holds) {
    let count = 0;
    for (const value of holds.values()) {
      count += value;
    }
    return count;
  }

  // The game actions some resource holds disabled, sorted.
  disabledActions() {
    const actions = new Set();
    for (const held of this.actionHolds.values()) {
      for (const action of held.keys()) {
        actions.add(action);
      }
    }
    return [...actions].sort();
  }

  publishControls() {
    const disabled = this.total(this.disableHolds) > 0;
    const cursor = this.total(this.cursorHolds) > 0;
    const actions = this.disabledActions();
    const key = actions.join(',');
    if (
      disabled === this.sent.disabled &&
      cursor === this.sent.cursor &&
      key === this.sent.actions
    ) {
      return;
    }
    this.sent = { disabled, cursor, actions: key };
    this.send({ op: 'controls', disabled, cursor, actions });
    this.tellCursor();
  }

  // The pointer shows when a resource holds it or the game shows it (a focused view, the chat):
  // each change is raised once as cursorVisibilityChanged(visible).
  tellCursor() {
    const visible = this.total(this.cursorHolds) > 0 || this.pointer;
    if (visible === this.cursorShown) {
      return;
    }
    this.cursorShown = visible;
    this.emit('cursorVisibilityChanged', [visible]);
  }

  holdAction(owner, action, delta) {
    const held = this.actionHolds.get(owner) ?? new Map();
    const count = (held.get(action) ?? 0) + delta;
    if (count < 0) {
      return; // a release without a hold of this resource does nothing
    }
    if (count === 0) {
      held.delete(action);
    } else {
      held.set(action, count);
    }
    if (held.size === 0) {
      this.actionHolds.delete(owner);
    } else {
      this.actionHolds.set(owner, held);
    }
    this.publishControls();
  }

  hold(holds, owner, delta) {
    const count = (holds.get(owner) ?? 0) + delta;
    if (count < 0) {
      return; // a release without a hold of this resource does nothing
    }
    if (count === 0) {
      holds.delete(owner);
    } else {
      holds.set(owner, count);
    }
    this.publishControls();
  }

  keyFacade(owner) {
    const input = this;
    return {
      getLabel(key) {
        const name = keyName(key);
        return input.labels.get(name) ?? KNOWN.get(name)[3];
      },

      bind(key, stateOrHandler, handler) {
        const name = keyName(key);
        let state = 'down';
        let callback = stateOrHandler;
        if (typeof stateOrHandler === 'string') {
          if (!STATES.has(stateOrHandler)) {
            throw new Error(`Unknown key state '${stateOrHandler}'`);
          }
          state = stateOrHandler;
          callback = handler;
        }
        if (typeof callback !== 'function') {
          throw new TypeError('Key handler must be a function');
        }
        input.ensureTable();
        input.binds.push({ owner, name, state, handler: callback });
        return true;
      },

      unbind(key, state, handler) {
        const name = keyName(key);
        if (state !== undefined && !STATES.has(state)) {
          throw new Error(`Unknown key state '${state}'`);
        }
        const before = input.binds.length;
        input.binds = input.binds.filter(
          (bind) =>
            !(
              bind.owner === owner &&
              bind.name === name &&
              (state === undefined || bind.state === state) &&
              (handler === undefined || bind.handler === handler)
            ),
        );
        return input.binds.length !== before;
      },

      isDown(key) {
        let name;
        try {
          name = keyName(key);
        } catch {
          return false;
        }
        input.ensureTable();
        return input.down.has(name);
      },
    };
  }

  controlsFacade(owner) {
    const input = this;
    return {
      disable() {
        input.hold(input.disableHolds, owner, 1);
      },
      enable() {
        input.hold(input.disableHolds, owner, -1);
      },
      isEnabled() {
        return !input.captured && input.total(input.disableHolds) === 0;
      },
      showCursor() {
        input.hold(input.cursorHolds, owner, 1);
      },
      hideCursor() {
        input.hold(input.cursorHolds, owner, -1);
      },
      isCursorVisible() {
        return input.total(input.cursorHolds) > 0 || input.pointer;
      },
      // Where the pointer is while a resource shows it (showCursor): { x, y }, -1 .. 1 left to
      // right and bottom to top, as Camera.rayFromScreen takes them; null while no resource shows
      // it or it is outside the game's window.
      cursorPosition() {
        if (input.total(input.cursorHolds) === 0 || !input.cursorAt) {
          return null;
        }
        return { x: input.cursorAt.x, y: input.cursorAt.y };
      },
      blockAction(action) {
        input.holdAction(owner, actionName(action, 'Controls.blockAction'), 1);
      },
      unblockAction(action) {
        input.holdAction(owner, actionName(action, 'Controls.unblockAction'), -1);
      },
    };
  }

  // { op: 'keyEdges', edges: [[name, 'down'|'up'], ...], down: [names] } from the game client.
  receiveEdges({ edges, down }) {
    this.down = new Set(Array.isArray(down) ? down : []);
    if (this.paused) {
      return; // a focused native screen takes the keys (NativeScreen 'input')
    }
    for (const [name, edge] of Array.isArray(edges) ? edges : []) {
      for (const bind of [...this.binds]) {
        if (bind.name !== name || (bind.state !== 'both' && bind.state !== edge)) {
          continue;
        }
        if (!this.binds.includes(bind)) {
          continue; // removed by an earlier handler of this edge
        }
        try {
          bind.handler(name, edge);
        } catch (error) {
          this.log(bind.owner, error?.stack ?? String(error));
        }
      }
    }
  }

  // { op: 'keyLabels', labels: { name: label } } for the current layout.
  receiveLabels({ labels }) {
    this.labels = new Map(Object.entries(labels ?? {}).filter(([name]) => KNOWN.has(name)));
  }

  // { op: 'cursorAt', x, y } while a resource shows the pointer: where it is, -1 .. 1 left to
  // right and bottom to top (Camera.rayFromScreen's), x and y null outside the game's window.
  receiveCursor({ x, y }) {
    const on = Number.isFinite(x) && Number.isFinite(y);
    this.cursorAt = on
      ? { x: Math.max(-1, Math.min(1, x)), y: Math.max(-1, Math.min(1, y)) }
      : null;
  }

  // { op: 'inputHost', captured, pointer } when the chat, a focused view or an overlay takes typed
  // input, or the pointer is shown for one of them.
  receiveHost({ captured, pointer }) {
    this.captured = Boolean(captured);
    this.pointer = Boolean(pointer);
    this.tellCursor();
  }

  // The resource stopped: its binds go and its holds are given back.
  removeOwner(owner) {
    this.binds = this.binds.filter((bind) => bind.owner !== owner);
    this.disableHolds.delete(owner);
    this.cursorHolds.delete(owner);
    this.actionHolds.delete(owner);
    this.publishControls();
  }
}

module.exports = { ResourceInput, keyName, KEYS };
