'use strict';

// The server's status over HTTP: public metadata and the player list, for a website,
// a bot, a launcher or a server list, on the same listener as the resources' routes
// (KCDS_HTTP_PORT). Built in and read-only:
//
//   GET /info.json     what the server is: name, card art, version, protocol, level, player
//                      count and cap, whether a password is asked, the stores its licence gate
//                      takes, the running resources, how long it has run, and its join link
//                      (kcds://server/<id>) once the Master lists it
//   GET /dynamic.json  what changes: name, level, player count and cap
//   GET /players.json  the player list (scoreboard): id and name of each player, or 403 when the
//                      owner keeps it private (KCDS_PUBLIC_PLAYER_LIST=off)
//   GET /metrics       the server's metrics in the Prometheus text format (web/prometheus.cjs),
//                      every interval of KCDS_SERVER_METRICS; dynamic.json then carries the
//                      health: tick time, missed ticks a minute, CPU
//
// Nothing secret is ever in an answer: no password, no address, no identifier, no setting a
// player could use against the server. Answers are JSON, never cached, and open to any website
// (Access-Control-Allow-Origin *). These paths go before the resources' own routes.

const { renderPrometheus, healthOf } = require('./prometheus.cjs');

const kDefaultName = 'KCDS server';
const kMaxNameBytes = 64;
const kMaxImageBytes = 512;
const kOffValues = new Set(['0', 'off', 'false', 'no']);

// KCDS_SERVER_NAME as the Master listing reads it (master/registration_settings.hpp): 1 to 64
// printable bytes, the default otherwise.
function serverName(value) {
  if (typeof value !== 'string' || value === '' || Buffer.byteLength(value) > kMaxNameBytes) {
    return kDefaultName;
  }
  for (const character of value) {
    const code = character.codePointAt(0);
    if (code < 32 || code === 127) {
      return kDefaultName;
    }
  }
  return value;
}

// KCDS_SERVER_IMAGE_URL: an https URL of at most 512 bytes, or ''.
function serverImage(value) {
  if (typeof value !== 'string' || value === '' || Buffer.byteLength(value) > kMaxImageBytes) {
    return '';
  }
  try {
    return new URL(value).protocol === 'https:' ? value : '';
  } catch {
    return '';
  }
}

class StatusRoutes {
  // host: the resource host (players, resources, licence gate, password); env: the environment
  // the server owner set.
  constructor(host, env = process.env, now = () => Date.now()) {
    this.host = host;
    this.env = env;
    this.now = now;
    this.startedAt = now();
    // From the native server's hostSettings message.
    this.settings = { map: '', maxPeers: 0, version: '', protocol: 0 };
    // The native server's last metrics interval (serverMetrics), or null.
    this.metrics = null;
  }

  // A metrics interval as the native server wrote it (JSON text); one that does not parse is
  // left out.
  setMetrics(json) {
    try {
      const parsed = JSON.parse(json);
      if (parsed && typeof parsed === 'object') {
        this.metrics = parsed;
      }
    } catch {
      // The last good interval stays.
    }
  }

  prometheus() {
    // Load-testing bots (kcds-bots, KCDS_ALLOW_BOTS) apart from players, so a mixed run is read
    // right.
    const bots = this.host.botCount();
    return renderPrometheus(
      this.metrics,
      {
        players: this.host.players.size,
        max_players: this.settings.maxPeers,
        resources_running: this.host.resources.size,
        status_uptime_seconds: Math.max(0, Math.floor((this.now() - this.startedAt) / 1000)),
      },
      [
        ['players_by_kind', { kind: 'player' }, this.host.players.size - bots],
        ['players_by_kind', { kind: 'bot' }, bots],
      ],
    );
  }

  configure(settings) {
    this.settings = {
      map: typeof settings.map === 'string' ? settings.map : '',
      maxPeers: Number.isInteger(settings.maxPeers) ? settings.maxPeers : 0,
      version: typeof settings.version === 'string' ? settings.version : '',
      protocol: Number.isInteger(settings.protocol) ? settings.protocol : 0,
    };
  }

  playerListPublic() {
    const setting = this.env.KCDS_PUBLIC_PLAYER_LIST;
    return typeof setting !== 'string' || !kOffValues.has(setting.trim().toLowerCase());
  }

  dynamic() {
    return {
      name: serverName(this.env.KCDS_SERVER_NAME),
      level: this.settings.map,
      players: this.host.players.size,
      maxPlayers: this.settings.maxPeers,
      health: healthOf(this.metrics),
    };
  }

  info() {
    const licence = this.host.licence?.config ?? {};
    return {
      ...this.dynamic(),
      image: serverImage(this.env.KCDS_SERVER_IMAGE_URL),
      version: this.settings.version,
      protocol: this.settings.protocol,
      password: this.host.passwordRequired?.() === true,
      playerList: this.playerListPublic(),
      licence: {
        stores: Array.isArray(licence.stores) ? [...licence.stores] : [],
        familySharing: licence.familySharing !== false,
      },
      resources: [...this.host.resources.keys()].sort(),
      joinLink: this.host.licence?.listing ? `kcds://server/${this.host.licence.listing}` : '',
      uptime: Math.max(0, Math.floor((this.now() - this.startedAt) / 1000)),
    };
  }

  players() {
    const list = [];
    for (const player of this.host.players.values()) {
      list.push({ id: player.id, name: player.nickname });
    }
    list.sort((a, b) => a.id - b.id);
    return list;
  }

  // The answer to a request, or null when the path is not a status path.
  handle(method, path) {
    let body;
    let status = 200;
    if (path === '/info.json') {
      body = () => this.info();
    } else if (path === '/dynamic.json') {
      body = () => this.dynamic();
    } else if (path === '/metrics') {
      if (method !== 'GET' && method !== 'HEAD') {
        return { status: 405, body: { error: 'method not allowed' }, headers: { allow: 'GET' } };
      }
      return {
        status: 200,
        body: method === 'HEAD' ? '' : this.prometheus(),
        headers: { 'cache-control': 'no-store', 'content-type': 'text/plain; version=0.0.4' },
      };
    } else if (path === '/players.json') {
      if (this.playerListPublic()) {
        body = () => this.players();
      } else {
        status = 403;
        body = () => ({ error: 'the player list is private' });
      }
    } else {
      return null;
    }
    const headers = {
      'cache-control': 'no-store',
      'access-control-allow-origin': '*',
    };
    if (method !== 'GET' && method !== 'HEAD') {
      return {
        status: 405,
        body: { error: 'method not allowed' },
        headers: { ...headers, allow: 'GET' },
      };
    }
    return { status, body: method === 'HEAD' ? '' : body(), headers };
  }
}

module.exports = { StatusRoutes, serverName, serverImage };
