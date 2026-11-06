'use strict';

// The client-only Chat global and the reserved chat events.
//
// The game client's chat overlay hands every typed line here first ({ op: 'chatTyped', text });
// the chatSend handlers see it, and unless one returned the literal false it goes to the server
// ({ op: 'chatOut', text }). Chat.send skips chatSend. Every line the server sends this player
// arrives as { op: 'chatMessage', author, text, color } and is raised as chatMessage. The
// overlay's state comes back as { op: 'chatHost', open, visible }; the calls that change it are
// { op: 'chatUI', visible }, { op: 'chatOpen' } and { op: 'chatClose' }. The chat key pressed while
// the overlay is hidden comes as { op: 'chatKey' } and is raised as chatKey.

// The server accepts lines up to this many UTF-8 bytes.
const MAX_BYTES = 1024;

function byteLength(text) {
  return new TextEncoder().encode(text).length;
}

function chatText(text) {
  if (typeof text !== 'string') {
    throw new TypeError('Chat: text must be a string');
  }
  if (text.length === 0 || byteLength(text) > MAX_BYTES) {
    throw new RangeError(`Chat: text must be 1 to ${MAX_BYTES} UTF-8 bytes`);
  }
  if (/[\x00-\x1f\x7f]/.test(text)) {
    throw new RangeError('Chat: text must not contain line breaks or control characters');
  }
  return text;
}

class ResourceChat {
  constructor({ send, emitSync, emit }) {
    this.send = send;
    this.emitSync = emitSync;
    this.emit = emit;
    this.open = false;
    this.visible = true;
  }

  // A line the player typed in the overlay: chatSend handlers decide whether it leaves.
  receiveTyped({ text }) {
    if (typeof text !== 'string') {
      return;
    }
    if (this.emitSync('chatSend', [text])) {
      this.send({ op: 'chatOut', text });
    }
  }

  receiveMessage({ author, text, color }) {
    if (typeof text !== 'string') {
      return;
    }
    const message = {
      author: typeof author === 'string' ? author : '',
      text,
      color: Number.isInteger(color) ? color >>> 0 : 0,
    };
    this.emit('chatMessage', [message]);
  }

  // The player pressed the chat key while a resource hid the chat (Chat.setUIVisible(false)):
  // nothing opened, so a resource's own chat window can.
  receiveKey() {
    this.emit('chatKey', []);
  }

  receiveHost({ open, visible }) {
    this.open = Boolean(open);
    this.visible = visible !== false;
  }

  facade() {
    const chat = this;
    return Object.freeze({
      send(text) {
        chat.send({ op: 'chatOut', text: chatText(text) });
      },
      setUIVisible(visible) {
        if (typeof visible !== 'boolean') {
          throw new TypeError('Chat: visible must be a boolean');
        }
        chat.visible = visible;
        if (!visible) {
          chat.open = false;
        }
        chat.send({ op: 'chatUI', visible });
      },
      isUIVisible() {
        return chat.visible;
      },
      open() {
        // A hidden overlay does not open; the game client reports what really happened.
        if (chat.visible) {
          chat.open = true;
        }
        chat.send({ op: 'chatOpen' });
      },
      close() {
        chat.open = false;
        chat.send({ op: 'chatClose' });
      },
      isOpen() {
        return chat.open;
      },
    });
  }
}

module.exports = { ResourceChat, MAX_BYTES };
