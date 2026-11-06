'use strict';

const { DiskFile } = require('./bundle_cache.cjs');

// What the servers' resources stream to this game (Assets). A resource's stream/
// folder holds new game files at their game paths under a folder of its own,
// stream/<folder>/kcds/<resource>/... (objects/kcds/<resource>/chair.cgf, a texture, a sound, an
// animation, an effect); the runtime sends them with the resource's other client files and asks
// the game client to mount them, which packs them per top folder and mounts the packs where the
// game's own loaders read them, so they load by path like the game's own:
//
//   { op: 'assetsMount', resource, files: ['stream/...'] }
//   from the game: { op: 'assetsMounted', resource, paths: ['objects/kcds/...'] }
//
// Assets.has(path) answers whether a mounted stream carries the path (only new files count).
//
// A World Architect custom map a server runs (Client/runtime/maps/custom_maps.hpp) travels the
// same way: a resource's maps/kcds_map_<name>/level.pak, recast.pak (its navmesh) and map.json go
// to the game client in
// pieces, which keeps them in the player's KCDS maps folder and lists the map before the world
// loads:
//
//   { op: 'customMapFile', resource, map, name, offset, total, data (base64) }

const STREAM_FOLDER = 'stream/';
const REPLACE_FOLDER = 'replace/';
// A custom map's files in a resource, and the pieces they are sent in (a message to the game is
// at most 64 KiB; base64 makes a piece a third larger).
const MAP_FILE = /^maps\/(kcds_map_[a-z0-9_]{1,32})\/(level\.pak|recast\.pak|map\.json)$/;
const MAP_PIECE = 24576;

function base64(bytes) {
  let text = '';
  for (let offset = 0; offset < bytes.length; offset += 8192) {
    text += String.fromCharCode(...bytes.subarray(offset, offset + 8192));
  }
  return btoa(text);
}

class ResourceAssets {
  constructor({ send, sendFile, log }) {
    this.send = send;
    this.sendFile = sendFile;
    this.log = log;
    // resource -> the paths the game reads from its stream, lower case
    this.mounted = new Map();
    // resource -> the files asked to be mounted
    this.asked = new Map();
    // resources whose custom maps were sent
    this.mapsSent = new Set();
  }

  // The resource's custom maps (maps/kcds_map_<name>/level.pak and map.json) go to the game
  // client, once per resource run; map.json last, which tells the game the map is complete.
  shipMaps(resource, files, read) {
    if (this.mapsSent.has(resource)) {
      return;
    }
    this.mapsSent.add(resource);
    const maps = files
      .map((file) => MAP_FILE.exec(file))
      .filter(Boolean)
      .sort((a, b) => (a[2] === 'map.json') - (b[2] === 'map.json'));
    for (const [file, map, name] of maps) {
      const bytes = read(file);
      if (!bytes) {
        continue;
      }
      if (bytes instanceof DiskFile) {
        // A big file the game copies from its download cache.
        this.send({ op: 'customMapFile', resource, map, name, cached: bytes.hash });
        continue;
      }
      for (let offset = 0; offset === 0 || offset < bytes.length; offset += MAP_PIECE) {
        this.send({
          op: 'customMapFile',
          resource,
          map,
          name,
          offset,
          total: bytes.length,
          data: base64(bytes.subarray(offset, offset + MAP_PIECE)),
        });
      }
    }
  }

  // The resource's streamed files go to the game client and are mounted, once per resource run.
  ship(resource, files, read) {
    if (this.asked.has(resource)) {
      return;
    }
    const streamed = files.filter((file) => file.startsWith(STREAM_FOLDER));
    const replacing = files.filter((file) => file.startsWith(REPLACE_FOLDER));
    if (streamed.length === 0 && replacing.length === 0) {
      return;
    }
    for (const file of [...streamed, ...replacing]) {
      const bytes = read(file);
      if (!bytes) {
        continue;
      }
      try {
        this.sendFile(resource, file, bytes);
      } catch (error) {
        this.log(resource, String(error));
      }
    }
    this.asked.set(resource, [...streamed, ...replacing]);
    if (streamed.length > 0) {
      this.send({ op: 'assetsMount', resource, files: streamed });
    }
    // Files standing in for the game's own at their paths (replace/<path>).
    if (replacing.length > 0) {
      this.send({ op: 'assetsMount', resource, files: replacing, replace: true });
    }
  }

  receiveMounted(message) {
    if (typeof message.resource !== 'string' || !Array.isArray(message.paths)) {
      return;
    }
    const paths = new Set(
      message.paths.filter((path) => typeof path === 'string').map((path) => path.toLowerCase()),
    );
    this.mounted.set(message.resource, paths);
  }

  has(path) {
    if (typeof path !== 'string') {
      throw new TypeError('Assets.has takes a path');
    }
    const wanted = path.split('\\').join('/').replace(/^\/+/, '').toLowerCase();
    for (const paths of this.mounted.values()) {
      if (paths.has(wanted)) {
        return true;
      }
    }
    return false;
  }

  facade() {
    return Object.freeze({ has: (path) => this.has(path) });
  }
}

module.exports = { ResourceAssets };
