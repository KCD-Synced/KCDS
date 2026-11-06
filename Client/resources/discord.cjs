'use strict';

// The client-only Discord global: what client resources add to the player's Discord
// activity. KCDS publishes its own presence (Client/runtime/discord/: starting up, the main menu,
// joining or playing on a server, the KCDS logo, a timer and the KCDS website button). While the
// player is on a server a resource may set only this much: the server's name (shown
// as "KCDS - <name>"), the status line (state), the logo (large image) with its tooltip, the small
// image with its tooltip and one button of its own; KCDS's website button is always the second.
// The rest of Discord's activity (its type, timestamps, party, secrets ...) stays KCDS's.
//
//   { op: 'discordPresence', name, state, largeImage, largeText, smallImage, smallText,
//     buttonLabel, buttonUrl }
//   { op: 'discordClear' }
//
// Every setter stages its field; update() and setPresence() publish them. The game tells the
// runtime { op: 'discordStatus', available, userId } (receiveStatus). The resources share one
// staged activity, as the game has one presence.

const MAX_TEXT_BYTES = 128;
// Discord's limits for a button.
const MAX_BUTTON_LABEL_BYTES = 32;
const MAX_BUTTON_URL_BYTES = 512;
// The fields setPresence takes, and the setter of each.
const PRESENCE_SETTERS = {
  name: 'setName',
  state: 'setState',
  largeImage: 'setLargeImage',
  largeText: 'setLargeText',
  smallImage: 'setSmallImage',
  smallText: 'setSmallText',
};

const encoder = new TextEncoder();

function fail(message) {
  throw new TypeError(`Discord: ${message}`);
}

function textOf(value, name) {
  if (typeof value !== 'string') {
    fail(`${name} takes a text`);
  }
  if (encoder.encode(value).length > MAX_TEXT_BYTES) {
    fail(`${name} is at most ${MAX_TEXT_BYTES} UTF-8 bytes`);
  }
  return value;
}

// One button: { label, url }, an https url; '' and '' (or null) for none.
function buttonOf(label, url, name) {
  if (label === '' && url === '') {
    return { label: '', url: '' };
  }
  if (typeof label !== 'string' || label.trim() === '') {
    fail(`${name}: the label is a text`);
  }
  if (encoder.encode(label).length > MAX_BUTTON_LABEL_BYTES) {
    fail(`${name}: the label is at most ${MAX_BUTTON_LABEL_BYTES} UTF-8 bytes`);
  }
  if (typeof url !== 'string' || !/^https:\/\/[^\s"<>]+$/.test(url)) {
    fail(`${name}: the url is an https link`);
  }
  if (encoder.encode(url).length > MAX_BUTTON_URL_BYTES) {
    fail(`${name}: the url is at most ${MAX_BUTTON_URL_BYTES} UTF-8 bytes`);
  }
  return { label, url };
}

function empty() {
  return {
    name: '',
    state: '',
    largeImage: '',
    largeText: '',
    smallImage: '',
    smallText: '',
    button: { label: '', url: '' },
  };
}

class ResourceDiscord {
  constructor({ send }) {
    this.send = send;
    this.staged = empty();
    this.status = { available: false, userId: '' };
  }

  receiveStatus(message) {
    this.status = {
      available: message.available === true,
      userId: typeof message.userId === 'string' ? message.userId : '',
    };
  }

  setAssets(assets) {
    if (assets === null || typeof assets !== 'object') {
      fail('setAssets takes an object');
    }
    for (const key of ['largeImage', 'largeText', 'smallImage', 'smallText']) {
      if (assets[key] !== undefined) {
        this.stage(PRESENCE_SETTERS[key], assets[key]);
      }
    }
  }

  // One setter by its facade name.
  stage(setter, ...args) {
    const staged = this.staged;
    switch (setter) {
      case 'setName':
        staged.name = textOf(args[0], 'setName');
        break;
      case 'setState':
        staged.state = textOf(args[0], 'setState');
        break;
      case 'setLargeImage':
        staged.largeImage = textOf(args[0], 'setLargeImage');
        break;
      case 'setLargeText':
        staged.largeText = textOf(args[0], 'setLargeText');
        break;
      case 'setSmallImage':
        staged.smallImage = textOf(args[0], 'setSmallImage');
        break;
      case 'setSmallText':
        staged.smallText = textOf(args[0], 'setSmallText');
        break;
      case 'setButton':
        staged.button = buttonOf(args[0], args[1], 'setButton');
        break;
      default:
        fail(`unknown setter ${setter}`);
    }
  }

  setPresence(options) {
    if (options === null || typeof options !== 'object' || Array.isArray(options)) {
      fail('setPresence takes an object');
    }
    for (const key of Object.keys(options)) {
      if (!Object.hasOwn(PRESENCE_SETTERS, key) && key !== 'button') {
        fail(
          `setPresence: unknown field ${key} (name, state, largeImage, largeText, smallImage, ` +
            'smallText, button)',
        );
      }
    }
    for (const [key, value] of Object.entries(options)) {
      if (key === 'button') {
        if (value === null) {
          this.stage('setButton', '', '');
        } else if (typeof value === 'object') {
          this.stage('setButton', value.label, value.url);
        } else {
          fail('button is { label, url } or null');
        }
      } else {
        this.stage(PRESENCE_SETTERS[key], value);
      }
    }
    return this.update();
  }

  // Publishes the staged fields: the server's name, the status line, the logo and the small image
  // with their tooltips, and one button.
  update() {
    if (!this.status.available) {
      return false;
    }
    const { name, state, largeImage, largeText, smallImage, smallText, button } = this.staged;
    this.send({
      op: 'discordPresence',
      name,
      state,
      largeImage,
      largeText,
      smallImage,
      smallText,
      buttonLabel: button.label,
      buttonUrl: button.url,
    });
    return true;
  }

  clear() {
    this.staged = empty();
    if (!this.status.available) {
      return false;
    }
    this.send({ op: 'discordClear' });
    return true;
  }

  facade() {
    const api = {};
    for (const setter of Object.values(PRESENCE_SETTERS)) {
      api[setter] = (value) => {
        this.stage(setter, value);
      };
    }
    api.setAssets = (assets) => this.setAssets(assets);
    api.setButton = (label, url) => {
      this.stage('setButton', label ?? '', url ?? '');
    };
    api.setPresence = (options) => this.setPresence(options);
    api.update = () => this.update();
    api.clear = () => this.clear();
    api.reset = () => {
      this.staged = empty();
    };
    api.getUserId = () => this.status.userId;
    api.isAvailable = () => this.status.available;
    return Object.freeze(api);
  }
}

module.exports = { ResourceDiscord, MAX_TEXT_BYTES };
