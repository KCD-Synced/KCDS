'use strict';

// A handcuffed player (a KCDS addition): Player.setCuffed(true | false),
// Player.cuffed and the event playerCuffedChange(player, cuffed).
//
// Being cuffed is desired state the server keeps per player, as a pillory stay: the owner's
// client gets { op: 'cuffs', id, cuffed } and gets it again when their world is ready again.
// While cuffed the player's hands are held behind the back (the game's hands_tied_back_idle
// clip, played as the player's animation, so every other client sees it through the owner's
// replicated animation) and their game lets only walking and looking around through: every other
// action of theirs is refused. Only setCuffed(false) takes the cuffs off; while they are on, the
// resources' playAnimation and stopAnimation of that player are refused, so nothing ends the pose.
// A resource keeps a player cuffed across a relog by reading Player.cuffed at playerDisconnect and
// setting it again when they are back.

// The game's clip of hands tied behind the back (Animations/Mannequin/ADB/kcd_male_database.adb,
// fragment Tied_VAR); the legs keep walking under it.
const CUFFED_CLIP = 'hands_tied_back_idle';

class Cuffs {
  constructor(host) {
    this.host = host;
    this.cuffed = new WeakSet();
  }

  isCuffed(player) {
    return this.cuffed.has(player);
  }

  // Player.setCuffed: true when the state is set (also when it already was); false for a player
  // who left.
  set(player, on) {
    if (typeof on !== 'boolean') {
      throw new TypeError('setCuffed takes true or false');
    }
    if (this.host.players.get(player.id) !== player) {
      return false;
    }
    this.host.requireLivePlayerCall(player);
    if (on === this.cuffed.has(player)) {
      return true;
    }
    if (on) {
      this.cuffed.add(player);
    } else {
      this.cuffed.delete(player);
    }
    this.apply(player);
    this.host.emit('playerCuffedChange', player, on);
    return true;
  }

  // Tells the player's game, and puts the pose on or takes it off; a player still loading hears
  // it when their world is ready (arrived).
  apply(player) {
    if (!player.ready) {
      return;
    }
    const on = this.cuffed.has(player);
    this.host.bridge.send({ op: 'cuffs', id: player.id, cuffed: on });
    if (on && !this.posed(player)) {
      this.host.playerAnimations.play(player, '', { clip: CUFFED_CLIP, loop: true });
    } else if (!on && this.posed(player)) {
      this.host.playerAnimations.stop(player);
    }
  }

  // Whether the player's animation is the cuffed pose (the player's animations put it on a new
  // body by themselves).
  posed(player) {
    const current = this.host.playerAnimations.stateOf(player).current;
    return Boolean(current) && current.kind === 'play' && current.order.clip === CUFFED_CLIP;
  }

  // The player's world is ready (again): a cuffed player is told again.
  arrived(player) {
    if (this.cuffed.has(player)) {
      this.apply(player);
    }
  }
}

module.exports = { Cuffs, CUFFED_CLIP };
