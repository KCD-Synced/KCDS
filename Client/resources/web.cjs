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

// The longest side of a screen in the world, in metres.
const MAX_SCREEN_METRES = 200;

// A screen's four corners in the world (top-left, top-right, bottom-right, bottom-left as its page
// faces), 12 numbers: its middle `position`, `heading` the way the page faces (degrees, 0 north,
// 90 east), `width` and `height` in metres, and `tilt` degrees it leans back from upright.
function screenCorners(screen) {
  const position = screen.position;
  if (!position || typeof position !== 'object') {
    throw new TypeError('screen.position must be a position');
  }
  const centre = [
    checkCoordinate(position.x, 'screen.position.x'),
    checkCoordinate(position.y, 'screen.position.y'),
    checkCoordinate(position.z, 'screen.position.z'),
  ];
  const heading = (checkCoordinate(screen.heading ?? 0, 'screen.heading') * Math.PI) / 180;
  const tilt = (checkCoordinate(screen.tilt ?? 0, 'screen.tilt') * Math.PI) / 180;
  const width = checkCoordinate(screen.width, 'screen.width');
  const height = checkCoordinate(screen.height, 'screen.height');
  if (width <= 0 || height <= 0 || width > MAX_SCREEN_METRES || height > MAX_SCREEN_METRES) {
    throw new RangeError(
      `screen width and height must be above 0 and at most ${MAX_SCREEN_METRES} m`,
    );
  }
  // Facing (sin h, cos h, 0): a viewer in front looks back along it, and their right is
  // (-cos h, sin h, 0). Leaning back turns the screen's up towards its back.
  const right = [-Math.cos(heading), Math.sin(heading), 0];
  const facing = [Math.sin(heading), Math.cos(heading), 0];
  const up = [-facing[0] * Math.sin(tilt), -facing[1] * Math.sin(tilt), Math.cos(tilt)];
  const corner = (across, upward) =>
    [0, 1, 2].map(
      (axis) =>
        centre[axis] + right[axis] * across * (width / 2) + up[axis] * upward * (height / 2),
    );
  return [...corner(-1, 1), ...corner(1, 1), ...corner(1, -1), ...corner(-1, -1)];
}

// The middle of a screen's corners (screenCorners), as { x, y, z }.
function screenMiddle(corners) {
  const middle = (axis) => (corners[axis] + corners[axis + 6]) / 2;
  return { x: middle(0), y: middle(1), z: middle(2) };
}

// How far `point` is from a screen (its corners, screenCorners), in metres: from the nearest point
// of its surface.
function screenDistance(corners, point) {
  const at = (index) => [corners[index * 3], corners[index * 3 + 1], corners[index * 3 + 2]];
  const [topLeft, topRight, , bottomLeft] = [at(0), at(1), at(2), at(3)];
  const across = topRight.map((value, axis) => value - topLeft[axis]);
  const down = bottomLeft.map((value, axis) => value - topLeft[axis]);
  const offset = [point.x, point.y, point.z].map((value, axis) => value - topLeft[axis]);
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const clamp01 = (value) => Math.min(1, Math.max(0, value));
  const s = clamp01(dot(offset, across) / dot(across, across));
  const t = clamp01(dot(offset, down) / dot(down, down));
  const nearest = topLeft.map((value, axis) => value + across[axis] * s + down[axis] * t);
  const gap = [point.x, point.y, point.z].map((value, axis) => value - nearest[axis]);
  return Math.sqrt(dot(gap, gap));
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
        // The page's size in pixels: a screen in the world's texture is as large.
        view.page = { width: geometry.width, height: geometry.height };
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
        view.page = { width: w, height: h };
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

      // KCDS extension: the view drawn on a screen in the world, as a cinema screen or a notice
      // board: `position` its middle, `heading` the way its page faces in degrees (0 north, 90
      // east), `width` and `height` in metres, `tilt` degrees it leans back. The game draws it
      // with each frame's camera, in perspective, and not while the camera is behind it. null
      // draws it flat again. `texture: true` draws the page into the texture of the resource's
      // own screen model at that place instead (a texture as large as the page, see the docs):
      // the world then hides it behind the player's hands, walls and people.
      setViewWorldScreen(viewId, screen) {
        let corners = null;
        let texture = false;
        if (screen !== null) {
          if (typeof screen !== 'object') {
            throw new TypeError('screen must be { position, heading, width, height } or null');
          }
          corners = screenCorners(screen);
          texture = optionalFlag(screen, 'texture', false);
        }
        const view = web.owned(owner, viewId);
        if (!view) {
          return false;
        }
        view.worldScreen =
          corners === null
            ? null
            : {
                position: screenMiddle(corners),
                heading: screen.heading ?? 0,
                width: screen.width,
                height: screen.height,
                tilt: screen.tilt ?? 0,
                texture,
                corners,
              };
        const fields = { value: corners !== null };
        (corners ?? new Array(12).fill(0)).forEach((value, index) => {
          fields[`c${index}`] = value;
        });
        if (texture) {
          fields.texture = true;
          fields.pageWidth = view.page.width;
          fields.pageHeight = view.page.height;
        }
        call('screen', viewId, fields);
        return true;
      },

      // KCDS extension: this resource's views on screens in the world, as setViewWorldScreen put
      // them: [{ viewId, position (the middle), heading, width, height, tilt, texture }].
      worldScreens() {
        return [...web.views.values()]
          .filter((view) => view.owner === owner && view.worldScreen)
          .map((view) => {
            const { corners, ...screen } = view.worldScreen;
            return { viewId: view.id, ...screen, position: { ...screen.position } };
          });
      },

      // KCDS extension: the id of this resource's screen in the world nearest `point` (a
      // position: the player's, a camera's), measured to the nearest spot of its surface, within
      // `range` metres; null when none is that near. Focus that one view (focus) to work one screen
      // of many.
      nearestWorldScreen(point, range = Infinity) {
        if (!point || typeof point !== 'object') {
          throw new TypeError('point must be a position');
        }
        const from = {
          x: checkCoordinate(point.x, 'point.x'),
          y: checkCoordinate(point.y, 'point.y'),
          z: checkCoordinate(point.z, 'point.z'),
        };
        const limit = range === Infinity ? Infinity : checkNumber(range, 'range');
        let nearest = null;
        let nearestDistance = limit;
        for (const view of web.views.values()) {
          if (view.owner !== owner || !view.worldScreen) {
            continue;
          }
          const distance = screenDistance(view.worldScreen.corners, from);
          if (distance <= nearestDistance) {
            nearest = view.id;
            nearestDistance = distance;
          }
        }
        return nearest;
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
      // Escape on a focused screen in the world: the game took the focus back.
      if (message.event === 'webViewUnfocused') {
        view.focused = false;
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

module.exports = { ResourceWebViews, screenCorners };
