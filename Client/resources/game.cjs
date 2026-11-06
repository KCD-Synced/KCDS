'use strict';

// The client-only Game global: the game itself. Game.quit() closes the game as the
// main menu's Quit does; the player's departure reaches the server at once (its playerDisconnect
// follows, where a resource saves the player):
//
//   { op: 'gameQuit' }

class ResourceGame {
  constructor({ send }) {
    this.send = send;
  }

  facade() {
    const send = this.send;
    return Object.freeze({
      // Closes the game, as "Quit game" in the menu does.
      quit() {
        send({ op: 'gameQuit' });
      },
    });
  }
}

module.exports = { ResourceGame };
