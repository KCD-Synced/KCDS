'use strict';

// The client Keybinds global: resource actions in the game's own key binding settings.
//
// A resource adds actions (and its own categories); they show in the game's Settings > Key
// bindings among the game's own, in a game category or in the resource's, and the player rebinds
// them there like any game action. The game client keeps the player's choices on this computer for
// each server (%LOCALAPPDATA%\KCDS\keybinds), never in the game's profile. The game's screen has
// keyboard and mouse only; an action's controller button is set by scripts (Keybinds.set) and kept
// the same way. Handlers fire through the Key path: in a world, game window in front, nothing
// taking typed input.
//
//   runtime -> game:  { op: 'keybinds', groups: [{ name, label }],
//                       actions: [{ name, group, label, key, controller }] }
//                     { op: 'keybindSet', name, keys?, controller? }, { op: 'keybindReset', name }
//   game -> runtime:  { op: 'keybindChoices', actions: [{ name, keys, controller }] }
//
// Inputs between the two are the engine's names (f6, np_1, tilde, pgup, mouse4, xi_a).

const { keyName, KEYS } = require('./input.cjs');

// The game's own categories, in the screen's order.
const GAME_CATEGORIES = [
  'general',
  'movement',
  'combat',
  'chat',
  'qam',
  'gui',
  'photomode',
  'other',
];
const ID = /^[a-z0-9]+(?:_[a-z0-9]+)*$/;
const MAX_ID = 24;
const MAX_LABEL = 96;
const MAX_ACTIONS = 256;
const STATES = new Set(['down', 'up', 'both']);
const KIND = new Map(KEYS.map(([name, kind]) => [name, kind]));

// KCDS key names that the engine names otherwise.
const TO_ENGINE = { grave: 'tilde', pageup: 'pgup', pagedown: 'pgdn', shift: 'lshift' };
TO_ENGINE.ctrl = 'lctrl';
TO_ENGINE.alt = 'lalt';
for (let n = 0; n <= 9; n++) {
  TO_ENGINE[`numpad${n}`] = `np_${n}`;
}
const FROM_ENGINE = Object.fromEntries(
  Object.entries(TO_ENGINE)
    .filter(([name]) => !['shift', 'ctrl', 'alt'].includes(name))
    .map(([name, engine]) => [engine, name]),
);

function toEngine(name) {
  return TO_ENGINE[name] ?? name;
}

// The KCDS key name of an engine input, or null for one KCDS does not poll (mouse wheel, ...).
function fromEngine(input) {
  const name = FROM_ENGINE[input] ?? input;
  return KIND.has(name) ? name : null;
}

function checkId(id, what) {
  if (typeof id !== 'string' || id.length > MAX_ID || !ID.test(id)) {
    throw new TypeError(
      `${what} is 1 to ${MAX_ID} lower-case letters, digits and single underscores`,
    );
  }
  return id;
}

function checkLabel(label, what) {
  if (typeof label !== 'string' || label.trim() === '' || label.length > MAX_LABEL) {
    throw new TypeError(`${what} is a text of 1 to ${MAX_LABEL} characters`);
  }
  return label;
}

// A keyboard or mouse key's name (null: none).
function keyboardKey(key, what) {
  if (key === null || key === undefined) {
    return null;
  }
  const name = keyName(key);
  if (KIND.get(name) === 'pad') {
    throw new Error(`${what} takes a keyboard or mouse key; '${key}' is a controller button`);
  }
  return name;
}

// A controller button's name (null: none).
function controllerButton(button, what) {
  if (button === null || button === undefined) {
    return null;
  }
  const name = keyName(button);
  if (KIND.get(name) !== 'pad') {
    throw new Error(`${what} takes a controller button such as 'xi_a'; '${button}' is not one`);
  }
  return name;
}

// The resource's part of the engine names: lower-case letters and digits, single underscores.
function resourcePart(resource) {
  const part = String(resource)
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '_')
    .replace(/^_+|_+$/g, '')
    .slice(0, 24);
  return part || 'resource';
}

class ResourceKeybinds {
  constructor({ send = () => {}, input, log = () => {} } = {}) {
    this.send = send;
    this.input = input;
    this.log = log;
    this.categories = new Map(); // engine name -> { owner, id, label }
    this.actions = new Map(); // engine name -> { owner, id, label, group, key, controller, handlers }
    this.choices = new Map(); // engine name -> { keys: [engine inputs], controller }
    this.publishQueued = false;
  }

  actionName(owner, id) {
    return `kcds_a_${resourcePart(owner)}__${id}`;
  }

  categoryName(owner, id) {
    return `kcds_g_${resourcePart(owner)}__${id}`;
  }

  // The whole set goes to the game client once per turn of the event loop.
  publish() {
    if (this.publishQueued) {
      return;
    }
    this.publishQueued = true;
    queueMicrotask(() => {
      this.publishQueued = false;
      this.send({
        op: 'keybinds',
        groups: [...this.categories].map(([name, category]) => ({ name, label: category.label })),
        actions: [...this.actions].map(([name, action]) => ({
          name,
          group: action.group,
          label: action.label,
          key: action.key ? toEngine(action.key) : '',
          controller: action.controller ?? '',
        })),
      });
    });
  }

  // The KCDS key names an action is bound to now, and its controller button.
  bound(name) {
    const action = this.actions.get(name);
    const choice = this.choices.get(name);
    if (!choice) {
      return { keys: action?.key ? [action.key] : [], controller: action?.controller ?? null };
    }
    const keys = choice.keys.map(fromEngine).filter((key) => key && KIND.get(key) !== 'pad');
    const controller = choice.controller ? fromEngine(choice.controller) : null;
    return { keys, controller: controller && KIND.get(controller) === 'pad' ? controller : null };
  }

  // The action's key binds in the Key path follow what it is bound to.
  rebind(name) {
    this.input.binds = this.input.binds.filter((bind) => bind.keybind !== name);
    const action = this.actions.get(name);
    if (!action || action.handlers.length === 0) {
      return;
    }
    const { keys, controller } = this.bound(name);
    const pressed = controller ? [...keys, controller] : keys;
    for (const key of pressed) {
      this.input.binds.push({
        owner: action.owner,
        name: key,
        state: 'both',
        keybind: name,
        handler: (_key, edge) => this.fire(name, edge),
      });
    }
    if (pressed.length > 0) {
      this.input.ensureTable();
    }
  }

  fire(name, edge) {
    const action = this.actions.get(name);
    if (!action) {
      return;
    }
    for (const entry of [...action.handlers]) {
      if (entry.state !== 'both' && entry.state !== edge) {
        continue;
      }
      try {
        entry.handler(action.id, edge);
      } catch (error) {
        this.log(action.owner, error?.stack ?? String(error));
      }
    }
  }

  describe(name) {
    const action = this.actions.get(name);
    const { keys, controller } = this.bound(name);
    const category = this.categories.get(action.group);
    return {
      id: action.id,
      label: action.label,
      category: category ? category.id : action.group,
      key: keys[0] ?? null,
      keys,
      controller,
      defaultKey: action.key,
      defaultController: action.controller,
    };
  }

  // { op: 'keybindChoices', actions: [{ name, keys, controller }] } from the game client.
  receiveChoices({ actions }) {
    this.choices = new Map();
    for (const entry of Array.isArray(actions) ? actions : []) {
      if (entry && typeof entry.name === 'string' && this.actions.has(entry.name)) {
        this.choices.set(entry.name, {
          keys: Array.isArray(entry.keys)
            ? entry.keys.filter((key) => typeof key === 'string')
            : [],
          controller: typeof entry.controller === 'string' ? entry.controller : '',
        });
      }
    }
    for (const name of this.actions.keys()) {
      this.rebind(name);
    }
  }

  // The resource stopped: its actions and categories leave the screen.
  removeOwner(owner) {
    let changed = false;
    for (const [name, action] of [...this.actions]) {
      if (action.owner === owner) {
        this.actions.delete(name);
        this.choices.delete(name);
        this.rebind(name);
        changed = true;
      }
    }
    for (const [name, category] of [...this.categories]) {
      if (category.owner === owner) {
        this.categories.delete(name);
        changed = true;
      }
    }
    if (changed) {
      this.publish();
    }
  }

  facade(owner) {
    const keybinds = this;
    const own = (id) => {
      const name = keybinds.actionName(owner, checkId(id, 'An action id'));
      const action = keybinds.actions.get(name);
      if (!action || action.owner !== owner) {
        throw new Error(`No key binding action '${id}' of this resource`);
      }
      return name;
    };
    return {
      addCategory(id, label) {
        const name = keybinds.categoryName(owner, checkId(id, 'A category id'));
        keybinds.categories.set(name, { owner, id, label: checkLabel(label, 'A category label') });
        keybinds.publish();
        return id;
      },

      add(id, options = {}) {
        const name = keybinds.actionName(owner, checkId(id, 'An action id'));
        if (typeof options !== 'object' || options === null) {
          throw new TypeError('Keybinds.add takes an id and an options object');
        }
        const label = checkLabel(options.label, 'An action label');
        const category = options.category ?? 'general';
        let group = category;
        if (!GAME_CATEGORIES.includes(category)) {
          group = keybinds.categoryName(owner, checkId(category, 'A category'));
          if (!keybinds.categories.has(group)) {
            throw new Error(
              `Unknown category '${category}': one of ${GAME_CATEGORIES.join(
                ', ',
              )} or one added with Keybinds.addCategory`,
            );
          }
        }
        const key = keyboardKey(options.key, 'Keybinds.add key');
        const controller = controllerButton(options.controller, 'Keybinds.add controller');
        const previous = keybinds.actions.get(name);
        if (!previous && keybinds.actions.size >= MAX_ACTIONS) {
          throw new Error(`At most ${MAX_ACTIONS} key binding actions`);
        }
        keybinds.actions.set(name, {
          owner,
          id,
          label,
          group,
          key,
          controller,
          handlers: previous ? previous.handlers : [],
        });
        keybinds.rebind(name);
        keybinds.publish();
        return true;
      },

      remove(id) {
        const name = keybinds.actionName(owner, checkId(id, 'An action id'));
        if (!keybinds.actions.has(name) || keybinds.actions.get(name).owner !== owner) {
          return false;
        }
        keybinds.actions.delete(name);
        keybinds.choices.delete(name);
        keybinds.rebind(name);
        keybinds.publish();
        return true;
      },

      get(id) {
        const name = keybinds.actionName(owner, checkId(id, 'An action id'));
        return keybinds.actions.has(name) ? keybinds.describe(name) : null;
      },

      list() {
        return [...keybinds.actions]
          .filter(([, action]) => action.owner === owner)
          .map(([name]) => keybinds.describe(name));
      },

      categories() {
        const mine = [...keybinds.categories.values()]
          .filter((category) => category.owner === owner)
          .map((category) => category.id);
        return [...GAME_CATEGORIES, ...mine];
      },

      set(id, binding = {}) {
        const name = own(id);
        if (typeof binding !== 'object' || binding === null) {
          throw new TypeError('Keybinds.set takes an id and { key, controller }');
        }
        const message = { op: 'keybindSet', name };
        const current = keybinds.choices.get(name) ?? {
          keys: keybinds.bound(name).keys.map(toEngine),
          controller: keybinds.bound(name).controller ?? '',
        };
        const next = { keys: current.keys, controller: current.controller };
        if (binding.key !== undefined) {
          const key = keyboardKey(binding.key, 'Keybinds.set key');
          next.keys = key ? [toEngine(key)] : [];
          message.keys = next.keys;
        }
        if (binding.controller !== undefined) {
          const button = controllerButton(binding.controller, 'Keybinds.set controller');
          next.controller = button ?? '';
          message.controller = next.controller;
        }
        keybinds.choices.set(name, next);
        keybinds.rebind(name);
        keybinds.send(message);
        return true;
      },

      reset(id) {
        const name = own(id);
        keybinds.choices.delete(name);
        keybinds.rebind(name);
        keybinds.send({ op: 'keybindReset', name });
        return true;
      },

      on(id, stateOrHandler, handler) {
        const name = own(id);
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
          throw new TypeError('Keybinds.on takes a handler');
        }
        keybinds.actions.get(name).handlers.push({ state, handler: callback });
        keybinds.rebind(name);
        return true;
      },

      off(id, state, handler) {
        const name = own(id);
        if (state !== undefined && !STATES.has(state)) {
          throw new Error(`Unknown key state '${state}'`);
        }
        const action = keybinds.actions.get(name);
        const before = action.handlers.length;
        action.handlers = action.handlers.filter(
          (entry) =>
            !(
              (state === undefined || entry.state === state) &&
              (handler === undefined || entry.handler === handler)
            ),
        );
        keybinds.rebind(name);
        return action.handlers.length !== before;
      },
    };
  }
}

module.exports = { ResourceKeybinds, GAME_CATEGORIES, toEngine, fromEngine };
