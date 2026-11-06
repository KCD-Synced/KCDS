'use strict';

// A server vendor's trade screen on this machine: the client events
// vendorOpened(session, npc) and vendorClosed(session, reason). The game client
// (Client/runtime/vendors) reports { op: 'vendorEvent', session, name: 'opened' | 'closed', npc,
// reason } once the game's shop screen actually came up and when it went away. Every
// vendorOpened is followed by exactly one vendorClosed; a screen that never came up raises
// neither.

const CLOSE_REASONS = new Set(['player', 'server', 'replaced', 'levelChange', 'sessionOver']);

class ResourceVendorScreen {
  constructor({ emit }) {
    this.emit = emit;
    // Sessions whose screen is up.
    this.open = new Set();
  }

  receiveEvent({ session, name, npc, reason }) {
    if (!Number.isInteger(session) || session <= 0) {
      return;
    }
    if (name === 'opened') {
      if (this.open.has(session)) {
        return;
      }
      this.open.add(session);
      this.emit('vendorOpened', [session, Number.isInteger(npc) && npc > 0 ? npc : null]);
    } else if (name === 'closed') {
      if (!this.open.delete(session)) {
        return;
      }
      this.emit('vendorClosed', [session, CLOSE_REASONS.has(reason) ? reason : 'player']);
    }
  }
}

module.exports = { ResourceVendorScreen };
