'use strict';

// The game's own travel between maps. Each level's map has points that lead to another level
// (the nomads' cart to Kuttenberg, and back to Trosky): in the single-player game a fast travel
// to one of them hands the player to the story, which loads the other level. KCDS runs without
// the story, so the client tells the server where each fast travel ended, and a travel that ended
// at such a point of the player's map is a trip to its level:
//
//   playerTravelling(player, trip) fires first, trip = { from, to, position, rotation, available }.
//   A handler returns false to keep the player where they are, or { map, position, rotation } to
//   send them elsewhere (another place on any map the server runs). `available` says whether this
//   server runs the map the point leads to; a server on one map keeps everyone at the point unless
//   a handler sends them somewhere.
//
// Then the player goes to the map (Player.setMap), at the place the level puts arriving players
// (maps/level_travel.json, Dev/tools/maps/build_level_travel.py), and playerMapChanged follows.
// World.setLevelTravel(false) turns the game's travel off for the whole server.

const LEVELS = require('./level_travel.json');

// How far from a travel point a fast travel may end and still be a trip to its level (metres):
// the game ends a travel on the road beside the point, within its discovery distance.
const MARK_REACH = 60;

class LevelTravel {
  constructor(host) {
    this.host = host;
    this.enabled = true;
  }

  // The travel point of `map` a fast travel ending at `position` used, or null.
  markAt(map, position) {
    const level = LEVELS[map];
    if (!level) {
      return null;
    }
    let found = null;
    let nearest = MARK_REACH;
    for (const mark of level.marks) {
      const dx = mark.position.x - position.x;
      const dy = mark.position.y - position.y;
      const distance = Math.hypot(dx, dy);
      if (distance <= nearest) {
        nearest = distance;
        found = mark;
      }
    }
    return found;
  }

  // Where the level `map` puts a player who arrives by its travel: { position, rotation }, or
  // null for a map the table does not know.
  arrivalOf(map) {
    return LEVELS[map]?.arrival ?? null;
  }

  // The client's report: the player's fast travel ended at { x, y, z }. True when a trip started.
  travelEnded(player, position) {
    if (!this.enabled || !player?.ready) {
      return false;
    }
    const from = this.host.mapOfPlayer(player);
    const mark = this.markAt(from, position);
    if (!mark) {
      return false;
    }
    const arrival = this.arrivalOf(mark.to);
    const trip = {
      from,
      to: mark.to,
      position: arrival ? { ...arrival.position } : null,
      rotation: arrival?.rotation ? { ...arrival.rotation } : null,
      available: this.host.maps.includes(mark.to),
    };
    let redirect = null;
    const accepted = this.host.emitCollecting(
      'playerTravelling',
      (answer) => {
        if (redirect === null && answer !== null && typeof answer === 'object') {
          redirect = answer;
        }
      },
      player,
      Object.freeze({ ...trip }),
    );
    if (!accepted) {
      return false;
    }
    const map = redirect?.map ?? trip.to;
    const place = redirect ? redirect.position : trip.position;
    const facing = redirect ? redirect.rotation : trip.rotation;
    if (!this.host.maps.includes(map)) {
      // A server without the map the point leads to keeps the player at the point.
      return false;
    }
    this.host.setPlayerMap(player, map, place ?? undefined, facing ?? undefined);
    return true;
  }

  // World.setLevelTravel.
  setEnabled(enabled) {
    if (typeof enabled !== 'boolean') {
      throw new TypeError('World.setLevelTravel takes true or false');
    }
    this.enabled = enabled;
  }
}

module.exports = { LevelTravel, MARK_REACH };
