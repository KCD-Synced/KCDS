'use strict';

// The client resource files over HTTP: a game joining the server
// downloads each file it lacks whole, instead of in 16 KB chunks over the game connection.
//
//   GET /kcds/files/<sha256>   the file of the active catalog with that hash, as bytes
//   HEAD /kcds/files/<sha256>  its size
//   GET /kcds/info.json        the server's public card (web/status_routes.cjs info.json: name,
//                              players, cap, card art, version), for anyone: a game that keeps the
//                              server as a favourite by its address, which the Master does not list
//
// It listens on TCP with the game's own port number (KCDS_GAME_PORT, and the game's bind address,
// KCDS_GAME_BIND), so every server has it at the same place and the owner opens one number for
// TCP and UDP. Only the files of the catalog players are sent are served, by their hash: the game
// checks every file's SHA-256 against the catalog, so the bytes cannot be other than the server's.
// Requests are bounded: kMaxConnections from one address at once, kTimeoutMs for an answer.
//
// Only a connected player's game is answered: its catalogInfo reply carries a token of its own
// (httpToken), which every request sends as the x-kcds-token header. The token ends when that
// player leaves, so an address that never joined, or left, gets nothing.

const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const { blobSize, changedMessage, stillHashed } = require('../bundles.cjs');

const kTimeoutMs = 120000;
const kMaxConnections = 8;
const kPath = /^\/kcds\/files\/([a-f0-9]{64})$/;
const kToken = /^[A-Za-z0-9_-]{43}$/;

class ResourceFileServer {
  // store: the ResourceBundleStore; port and bind: the game's; log(text); status: the StatusRoutes
  // whose info.json is answered at /kcds/info.json (none: not answered).
  constructor({ store, port, bind, log = () => {}, status = null }) {
    this.store = store;
    this.status = status;
    this.port = Number.isInteger(port) && port > 0 && port < 65536 ? port : null;
    this.bind = typeof bind === 'string' && bind !== '' ? bind : '0.0.0.0';
    this.log = log;
    this.server = null;
    this.connections = new Map();
    // The changed big files the owner was told about, once each.
    this.reported = new Set();
    // token -> peer and peer -> token, for the connected players' games.
    this.tokens = new Map();
    this.peerTokens = new Map();
  }

  // The token of this peer's game (one per connection), made on its first ask.
  tokenFor(peer) {
    const known = this.peerTokens.get(peer);
    if (known) {
      return known;
    }
    const token = crypto.randomBytes(32).toString('base64url');
    this.tokens.set(token, peer);
    this.peerTokens.set(peer, token);
    return token;
  }

  // The peer left: its token is answered no more.
  forget(peer) {
    const token = this.peerTokens.get(peer);
    if (token) {
      this.tokens.delete(token);
      this.peerTokens.delete(peer);
    }
  }

  // Whether the files are offered (the catalog says so to the games).
  get listening() {
    return this.server !== null && this.server.listening;
  }

  serve(request, response) {
    if (request.url === '/kcds/info.json' && this.status) {
      const answer = this.status.handle(request.method, '/info.json');
      const body = typeof answer.body === 'string' ? answer.body : JSON.stringify(answer.body);
      response.writeHead(answer.status, {
        ...answer.headers,
        'content-type': 'application/json; charset=utf-8',
        'content-length': String(Buffer.byteLength(body)),
      });
      response.end(request.method === 'HEAD' ? undefined : body);
      return;
    }
    const match = kPath.exec(request.url ?? '');
    if ((request.method !== 'GET' && request.method !== 'HEAD') || !match) {
      response.writeHead(404, { 'content-type': 'text/plain' });
      response.end('Not found');
      return;
    }
    const token = request.headers['x-kcds-token'];
    if (typeof token !== 'string' || !kToken.test(token) || !this.tokens.has(token)) {
      response.writeHead(403, { 'content-type': 'text/plain' });
      response.end('Not a connected player');
      return;
    }
    const blob = this.store.blobs.get(match[1]);
    if (!blob) {
      response.writeHead(404, { 'content-type': 'text/plain' });
      response.end('Not in the catalog');
      return;
    }
    // A big file is read from the disk as it goes out; it must still be the one hashed. It is
    // opened first, never through a link swapped in after the catalog was built, and that very
    // file is checked and sent.
    let stream = null;
    if (!Buffer.isBuffer(blob)) {
      let descriptor = null;
      let same = false;
      try {
        descriptor = fs.openSync(blob.path, fs.constants.O_RDONLY | (fs.constants.O_NOFOLLOW ?? 0));
        same = stillHashed(blob, descriptor, fs.fstatSync(descriptor));
      } catch {
        same = false;
      }
      if (!same) {
        if (descriptor !== null) {
          fs.closeSync(descriptor);
        }
        if (!this.reported.has(blob)) {
          this.reported.add(blob);
          this.log(changedMessage(blob));
        }
        response.writeHead(410, { 'content-type': 'text/plain' });
        response.end('Changed since the catalog was built');
        return;
      }
      if (request.method === 'HEAD') {
        fs.closeSync(descriptor);
      } else {
        stream = fs.createReadStream(null, { fd: descriptor });
      }
    }
    response.writeHead(200, {
      'content-type': 'application/octet-stream',
      'content-length': String(blobSize(blob)),
      // Only for this player's game.
      'cache-control': 'private, no-store',
    });
    if (request.method === 'HEAD') {
      response.end();
    } else if (stream) {
      stream.on('error', () => response.destroy());
      stream.pipe(response);
    } else {
      response.end(blob);
    }
  }

  start() {
    if (this.port === null || this.server) {
      return Promise.resolve();
    }
    this.server = http.createServer((request, response) => this.serve(request, response));
    this.server.requestTimeout = kTimeoutMs;
    this.server.on('connection', (socket) => {
      const address = socket.remoteAddress ?? '';
      const open = this.connections.get(address) ?? 0;
      if (open >= kMaxConnections) {
        socket.destroy();
        return;
      }
      this.connections.set(address, open + 1);
      socket.once('close', () => {
        const left = (this.connections.get(address) ?? 1) - 1;
        if (left > 0) {
          this.connections.set(address, left);
        } else {
          this.connections.delete(address);
        }
      });
    });
    return new Promise((resolve) => {
      this.server.once('error', (error) => {
        this.log(
          `Resource files could not listen on TCP ${this.bind}:${this.port}: ${error.message}; ` +
            'players download them over the game connection',
        );
        this.server = null;
        resolve();
      });
      this.server.listen(this.port, this.bind, () => {
        this.log(`Resource files on TCP ${this.bind}:${this.server.address().port}`);
        resolve();
      });
    });
  }

  close() {
    const server = this.server;
    this.server = null;
    if (!server) {
      return Promise.resolve();
    }
    server.closeAllConnections();
    return new Promise((resolve) => server.close(() => resolve()));
  }
}

module.exports = { ResourceFileServer };
