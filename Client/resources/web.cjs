'use strict';

// The client-only Web global: resource-owned web views that the
// game client draws over the frame (Client/ui/web). This runtime numbers the views, keeps the
// state that the synchronous queries answer from, sends every change to the game as
// { op: 'web', call, ... } and turns the game's reports back into browser* events and Web.on
// calls for the owning resource only. A resource reaches only its own views.

// A kcds:// URL may carry a query and a fragment after the file's path (a page reads its language
// from #cs); the file is the path before them.
const URL_PATTERN = /^(kcds:\/\/resources\/[^?#]+([?#].*)?|https?:\/\/[^\s]+)$/i;

function checkUrl(url) {
  if (typeof url !== 'string') {
    throw new TypeError('URL must be a string');
  }
  if (!URL_PATTERN.test(url)) {
    throw new Error('Web views load kcds://resources/<resource>/<path> or http(s) URLs');
  }
}

function checkNumber(value, name) {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError(`${name} must be a finite number`);
  }
  return Math.round(value);
}

// A world coordinate: any finite number, not rounded.
function checkCoordinate(value, name) {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError(`${name} must be a finite number`);
  }
  return value;
}

function optionalNumber(options, name) {
  if (options[name] === undefined) {
    return 0;
  }
  return checkNumber(options[name], name);
}

function optionalFlag(options, name, fallback) {
  if (options[name] === undefined) {
    return fallback;
  }
  if (typeof options[name] !== 'boolean') {
    throw new TypeError(`${name} must be a boolean`);
  }
  return options[name];
}

// A view of open's anchor 'center': where it goes on this screen, never past its edges.
function centered(wanted, screen) {
  const width = Math.min(wanted.width, screen.width);
  const height = Math.min(wanted.height, screen.height);
  return {
    x: Math.floor((screen.width - width) / 2),
    y: Math.floor((screen.height - height) / 2),
    width,
    height,
  };
}

class ResourceWebViews {
  constructor({
    send = () => {},
    log = () => {},
    emit = () => Promise.resolve(),
    raise = () => Promise.resolve(),
  } = {}) {
    this.send = send;
    this.log = log;
    this.emit = emit; // (eventName, args) -> Promise: native events for one resource
    this.raise = raise; // (eventName, args) -> Promise: native events for every resource
    this.views = new Map(); // id -> { id, owner, visible, focused, offscreen, handlers }
    this.nextId = 1;
    this.ready = false;
    this.screen = { width: 0, height: 0 };
  }

  // The game reported its browser state: { ready, width, height }.
  updateHost({ ready, width, height }) {
    if (typeof ready === 'boolean') {
      this.ready = ready;
    }
    if (Number.isFinite(width) && Number.isFinite(height)) {
      const changed = width !== this.screen.width || height !== this.screen.height;
      const had = this.screen.width > 0 && this.screen.height > 0;
      this.screen = { width, height };
      if (changed && width > 0 && height > 0) {
        this.placeAnchored();
        if (had) {
          this.raise('gameResolutionChanged', [width, height]).catch((error) =>
            this.log('host', String(error)),
          );
        }
      }
    }
  }

  // The views with an anchor take their place on the screen as it is now.
  placeAnchored() {
    for (const view of this.views.values()) {
      if (view.anchored) {
        const place = centered(view.anchored, this.screen);
        this.send({
          op: 'web',
          call: 'resize',
          id: view.id,
          width: place.width,
          height: place.height,
        });
        this.send({ op: 'web', call: 'move', id: view.id, x: place.x, y: place.y });
      }
    }
  }

  owned(owner, viewId) {
    const view = this.views.get(viewId);
    return view && view.owner === owner ? view : null;
  }

  // The Web global of one resource.
  facade(owner) {
    const web = this;
    const call = (name, viewId, fields = {}) => {
      web.send({ op: 'web', call: name, id: viewId, ...fields });
    };
    return {
      open(url, options = {}) {
        checkUrl(url);
        if (options === null || typeof options !== 'object') {
          throw new TypeError('options must be an object');
        }
        if (!web.ready) {
          throw new Error('The browser is not ready yet');
        }
        const view = {
          id: web.nextId++,
          owner,
          visible: optionalFlag(options, 'visible', true),
          focused: optionalFlag(options, 'focus', false),
          offscreen: false,
          handlers: new Map(),
        };
        const geometry = {
          x: optionalNumber(options, 'x'),
          y: optionalNumber(options, 'y'),
          width: optionalNumber(options, 'width'),
          height: optionalNumber(options, 'height'),
          zIndex: optionalNumber(options, 'zIndex'),
        };
        if (options.anchor !== undefined) {
          if (options.anchor !== 'center') {
            throw new TypeError("anchor must be 'center'");
          }
          if (!(geometry.width > 0) || !(geometry.height > 0)) {
            throw new TypeError("anchor 'center' needs a width and a height");
          }
          view.anchored = { width: geometry.width, height: geometry.height };
          Object.assign(geometry, centered(view.anchored, web.screen));
        }
        web.views.set(view.id, view);
        call('create', view.id, {
          owner,
          url,
          ...geometry,
          visible: view.visible,
          focus: view.focused,
        });
        return view.id;
      },

      close(viewId) {
        const view = web.owned(owner, viewId);
        if (!view) {
          return false;
        }
        web.views.delete(viewId);
        view.handlers.clear();
        call('destroy', viewId);
        return true;
      },

      show(viewId) {
        return web.setFlag(owner, viewId, 'visible', true, 'visible');
      },

      hide(viewId) {
        return web.setFlag(owner, viewId, 'visible', false, 'visible');
      },

      focus(viewId, focused = true) {
        if (typeof focused !== 'boolean') {
          throw new TypeError('focused must be a boolean');
        }
        return web.setFlag(owner, viewId, 'focused', focused, 'focus');
      },

      // Any resource's: a script need not own the page to know the player is typing in it.
      anyFocused() {
        return [...web.views.values()].some((view) => view.focused && view.visible);
      },

      isVisible(viewId) {
        return Boolean(web.owned(owner, viewId)?.visible);
      },

      setBackground(viewId, background = true) {
        if (typeof background !== 'boolean') {
          throw new TypeError('background must be a boolean');
        }
        return web.setFlag(owner, viewId, 'offscreen', background, 'offscreen');
      },

      isBackground(viewId) {
        return Boolean(web.owned(owner, viewId)?.offscreen);
      },

      navigate(viewId, url) {
        checkUrl(url);
        if (!web.owned(owner, viewId)) {
          return false;
        }
        call('load', viewId, { url });
        return true;
      },

      resize(viewId, width, height) {
        const w = checkNumber(width, 'width');
        const h = checkNumber(height, 'height');
        const view = web.owned(owner, viewId);
        if (!view) {
          return false;
        }
        if (view.anchored) {
          view.anchored = { width: w, height: h };
          web.placeAnchored();
          return true;
        }
        call('resize', viewId, { width: w, height: h });
        return true;
      },

      move(viewId, x, y) {
        const left = checkNumber(x, 'x');
        const top = checkNumber(y, 'y');
        const view = web.owned(owner, viewId);
        if (!view) {
          return false;
        }
        view.anchored = null;
        call('move', viewId, { x: left, y: top });
        return true;
      },

      // KCDS extension: the view follows a world point. The game places it every frame with that
      // frame's camera (its top-left corner on the point, moved by offsetX and offsetY pixels)
      // and does not draw it while the point is behind the camera or off screen. null lets it
      // keep its own position again.
      setViewWorldPosition(viewId, point, offsetX = 0, offsetY = 0) {
        const dx = checkNumber(offsetX, 'offsetX');
        const dy = checkNumber(offsetY, 'offsetY');
        let anchor = { value: false, x: 0, y: 0, z: 0 };
        if (point !== null) {
          if (typeof point !== 'object') {
            throw new TypeError('point must be a position or null');
          }
          anchor = {
            value: true,
            x: checkCoordinate(point.x, 'point.x'),
            y: checkCoordinate(point.y, 'point.y'),
            z: checkCoordinate(point.z, 'point.z'),
          };
        }
        if (!web.owned(owner, viewId)) {
          return false;
        }
        call('anchor', viewId, { ...anchor, offsetX: dx, offsetY: dy });
        return true;
      },

      on(viewId, eventName, handler) {
        if (typeof eventName !== 'string') {
          throw new TypeError('Event name must be a string');
        }
        if (typeof handler !== 'function') {
          throw new TypeError('Handler must be a function');
        }
        const view = web.owned(owner, viewId);
        if (!view) {
          throw new Error('Unknown web view');
        }
        if (!view.handlers.has(eventName)) {
          view.handlers.set(eventName, []);
        }
        view.handlers.get(eventName).push(handler);
      },

      off(viewId, eventName, handler) {
        const view = web.owned(owner, viewId);
        const list = view?.handlers.get(eventName);
        if (!list) {
          return false;
        }
        const kept = handler === undefined ? [] : list.filter((entry) => entry !== handler);
        const removed = kept.length !== list.length;
        if (kept.length) {
          view.handlers.set(eventName, kept);
        } else {
          view.handlers.delete(eventName);
        }
        return removed;
      },

      emit(viewId, eventName, payload) {
        if (typeof eventName !== 'string') {
          throw new TypeError('Event name must be a string');
        }
        if (!web.owned(owner, viewId)) {
          return false;
        }
        const json = JSON.stringify(payload === undefined ? null : payload);
        call('emit', viewId, { name: eventName, payload: json === undefined ? 'null' : json });
        return true;
      },

      screenSize() {
        return { width: web.screen.width, height: web.screen.height };
      },
    };
  }

  setFlag(owner, viewId, key, value, field) {
    const view = this.owned(owner, viewId);
    if (!view) {
      return false;
    }
    view[key] = value;
    this.send({ op: 'web', call: field, id: viewId, value });
    return true;
  }

  // A message the game client sent (Client/ui/web take_messages), already parsed.
  receive(message) {
    if (message.type === 'web-event') {
      const view = this.owned(message.owner, message.args?.[0]?.viewId);
      if (!view) {
        return;
      }
      this.emit(message.owner, message.event, message.args).catch((error) =>
        this.log(message.owner, String(error)),
      );
      return;
    }
    if (message.type === 'web-call') {
      const view = this.owned(message.owner, message.viewId);
      const handlers = view?.handlers.get(message.name);
      if (!handlers) {
        return;
      }
      let payload = null;
      if (typeof message.payload === 'string') {
        try {
          payload = JSON.parse(message.payload);
        } catch {
          payload = message.payload; // a plain string the page passed as is
        }
      }
      for (const handler of [...handlers]) {
        try {
          handler(payload);
        } catch (error) {
          this.log(message.owner, error?.stack ?? String(error));
        }
      }
    }
  }

  // The resource stopped: the game destroys its views; their handlers go with them.
  removeOwner(owner) {
    let owned = false;
    for (const [id, view] of this.views) {
      if (view.owner === owner) {
        this.views.delete(id);
        owned = true;
      }
    }
    if (owned) {
      this.send({ op: 'web', call: 'destroyOwned', owner });
    }
  }
}

module.exports = { ResourceWebViews };
