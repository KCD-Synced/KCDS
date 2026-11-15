'use strict';

// A player's seat on a placed smart object, in their game (the server's side is
// Backend/scripting/runtimes/js/smart_objects/player_seats.cjs). The game client reports a press of
// the game's own seat keys ({ op: 'gameUse', action: 'use_bed_sit' | 'use_bed_sleep' }); this
// module passes it to the server through the built-in event 'kcds:seat' that scripts never see,
// and the server decides whether a placed seat is near and free. Seated, a "Stand up" hint on
// script_action (U) asks to stand, as at a tabletop.

const EVENT = 'kcds:seat';
const OWNER = 'kcds:seat';

class ResourcePlacedSeats {
  constructor({ send, actionHints }) {
    this.send = send;
    this.hints = actionHints.facade(OWNER);
    this.standHint = null;
  }

  // The game's seat key pressed.
  receiveUse(data) {
    if (typeof data?.action === 'string') {
      this.request({ op: 'use', action: data.action });
    }
  }

  // A 'kcds:seat' message from the server: seated or stood.
  receive(payload) {
    if (payload?.op === 'seated') {
      this.showStand();
    } else if (payload?.op === 'stood') {
      this.hideStand();
    }
  }

  showStand() {
    if (this.standHint) {
      return;
    }
    this.standHint = this.hints.create({ key: 'script_action', text: 'Stand up' }, () => {
      this.request({ op: 'stand' });
    });
  }

  hideStand() {
    if (this.standHint) {
      this.hints.remove(this.standHint);
      this.standHint = null;
    }
  }

  request(payload) {
    this.send({ op: 'event', name: EVENT, payload: JSON.stringify(payload) });
  }

  disconnect() {
    this.hideStand();
  }
}

module.exports = { ResourcePlacedSeats, SEAT_EVENT: EVENT };
