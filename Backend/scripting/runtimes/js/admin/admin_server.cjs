'use strict';

// The server's built-in KCDS-Admin: one page and its JSON API on its own port, for the
// owner and the moderators they add. Off unless the owner sets KCDS_ADMIN_PORT and
// KCDS_ADMIN_PASSWORD; it listens on 127.0.0.1 unless KCDS_ADMIN_BIND says otherwise, and over
// HTTPS with KCDS_ADMIN_TLS_CERT and KCDS_ADMIN_TLS_KEY (PEM files).
//
//   POST /api/login {name, password}      a session cookie (HttpOnly, SameSite=Strict, 12 hours)
//                                         and the session; a wrong one {error, triesLeft}, and
//                                         429 {error, retryAfter} (seconds, also Retry-After)
//   POST /api/logout
//   GET  /api/session                     {name, role, expires, listen: {scheme, bind, port}}
//   GET  /api/status                      the status API's info.json
//   GET  /api/players                     [{id, name, ping, ip, identifiers, position}]
//   POST /api/players/{id}/kick           {reason}
//   POST /api/players/{id}/message        {text}
//   POST /api/players/{id}/teleport       {x, y, z}
//   POST /api/players/{id}/ban            {reason, minutes (0 for ever)}: their identifiers but ip
//   GET  /api/bans, POST /api/bans        {identifiers, reason, minutes}, DELETE /api/bans/{id}
//   GET  /api/log?after=N                 {lines: [{seq, time, source, text}]}
//   POST /api/command {text}              owner: a console command
//   GET  /api/resources                   [{name, version, running}]
//   POST /api/resources/{name}/{start|stop|restart}   owner
//   GET  /api/accounts, POST /api/accounts {name, password}, DELETE /api/accounts/{name}   owner
//   GET  /api/config, PUT /api/config {text}   owner: server.json as text; a save is checked JSON
//   GET  /api/backups, POST /api/backups   owner: the backups, and one made now
//   GET  /api/crashes                     owner: the crash reports [{name, kind, startedAt, endedAt}]
//   GET  /api/crashes/{name}              owner: one report, with the last log lines
//   GET  /api/updates                     owner: {available, reason, current, channel, staged}
//   POST /api/updates/check               owner: the newest release {release, sequence, newer}
//   POST /api/updates/install {minutes}   owner: downloads it, then restarts in minutes into it
//   GET  /api/server                      the planned stop or restart, or null (moderators too)
//   POST /api/server/{restart|stop} {minutes}, POST /api/server/cancel                 owner
//
// Requests are bounded (64 KiB of body), a POST or DELETE must carry X-KCDS-Admin: 1 (a page of
// another site cannot send it), sign-ins are limited to 5 failures per address in 5 minutes, and
// every action is written to the log with the account that did it.

const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const path = require('node:path');

const { AdminAccounts } = require('./admin_accounts.cjs');

const kMaxBody = 64 * 1024;
const kSessionMs = 12 * 60 * 60 * 1000;
const kMaxSessions = 256;
const kFailureWindowMs = 5 * 60 * 1000;
const kMaxFailures = 5;
const kMaxTextBytes = 512;
const kKickReasonBytes = 200;
const kCookie = 'kcds_admin';
const PAGE = path.join(__dirname, 'admin_page.html');

function text(value, limit = kMaxTextBytes) {
  return typeof value === 'string' && Buffer.byteLength(value) <= limit ? value.trim() : null;
}

// A kick's reason as the game takes it: at most 200 bytes, cut on a whole character.
function kickText(reason) {
  let cut = '';
  for (const character of reason) {
    if (Buffer.byteLength(cut + character) > kKickReasonBytes) {
      break;
    }
    cut += character;
  }
  return cut;
}

class AdminServer {
  // host: the resource host; options: { port, bind, password, tlsCert, tlsKey, root, log,
  // consoleCommand(text) }.
  constructor(host, options = {}) {
    this.host = host;
    // 0: any free port (tests); main.cjs gives a real one or none.
    this.port =
      Number.isInteger(options.port) && options.port >= 0 && options.port < 65536
        ? options.port
        : null;
    this.bind = options.bind || '127.0.0.1';
    this.log = options.log ?? (() => {});
    this.consoleCommand = options.consoleCommand ?? (async () => {});
    // The server's settings file (server.json), edited by the owner.
    this.configFile = typeof options.configFile === 'string' ? options.configFile : '';
    this.accounts = new AdminAccounts(options.password, options.root, this.log);
    this.tls = null;
    if (options.tlsCert && options.tlsKey) {
      try {
        this.tls = { cert: fs.readFileSync(options.tlsCert), key: fs.readFileSync(options.tlsKey) };
      } catch (error) {
        this.log(`KCDS-Admin TLS files could not be read: ${error.message}; the admin stays off`);
        this.port = null;
      }
    }
    this.sessions = new Map();
    this.failures = new Map();
    this.now = () => Date.now();
    this.server = null;
  }

  enabled() {
    return this.port !== null && this.accounts.enabled();
  }

  start() {
    if (!this.enabled() || this.server) {
      if (this.port !== null && !this.accounts.enabled()) {
        this.log('KCDS-Admin off: KCDS_ADMIN_PASSWORD must be at least 8 characters');
      }
      return Promise.resolve();
    }
    const handler = (request, response) => {
      this.serve(request, response).catch((error) => {
        this.log(`KCDS-Admin ${request.method} ${request.url} failed: ${error?.stack ?? error}`);
        this.answer(response, 500, { error: 'internal' });
      });
    };
    this.server = this.tls ? https.createServer(this.tls, handler) : http.createServer(handler);
    this.server.requestTimeout = 15000;
    return new Promise((resolve) => {
      this.server.once('error', (error) => {
        this.log(`KCDS-Admin could not listen on ${this.bind}:${this.port}: ${error.message}`);
        this.server = null;
        resolve();
      });
      this.server.listen(this.port, this.bind, () => {
        const scheme = this.tls ? 'https' : 'http';
        this.log(`KCDS-Admin on ${scheme}://${this.bind}:${this.server.address().port}/`);
        resolve();
      });
    });
  }

  address() {
    return this.server?.address() ?? null;
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

  answer(response, status, body, headers = {}) {
    if (response.headersSent) {
      return;
    }
    const json = typeof body === 'string' ? body : JSON.stringify(body ?? {});
    response.writeHead(status, {
      'content-type': typeof body === 'string' ? 'text/html; charset=utf-8' : 'application/json',
      'cache-control': 'no-store',
      'x-content-type-options': 'nosniff',
      'x-frame-options': 'DENY',
      'referrer-policy': 'no-referrer',
      ...headers,
    });
    response.end(json);
  }

  cookieOf(request) {
    for (const part of String(request.headers.cookie ?? '').split(';')) {
      const [name, value] = part.trim().split('=');
      if (name === kCookie && /^[0-9a-f]{64}$/.test(value ?? '')) {
        return value;
      }
    }
    return '';
  }

  activeSession(request) {
    const token = this.cookieOf(request);
    const session = token ? this.sessions.get(token) : null;
    if (!session || session.expires <= this.now()) {
      if (session) {
        this.sessions.delete(token);
      }
      return null;
    }
    return session;
  }

  async body(request) {
    const chunks = [];
    let size = 0;
    for await (const chunk of request) {
      size += chunk.length;
      if (size > kMaxBody) {
        throw Object.assign(new Error('body too large'), { status: 413 });
      }
      chunks.push(chunk);
    }
    if (size === 0) {
      return {};
    }
    try {
      const value = JSON.parse(Buffer.concat(chunks).toString('utf8'));
      return value && typeof value === 'object' ? value : {};
    } catch {
      throw Object.assign(new Error('invalid JSON'), { status: 400 });
    }
  }

  // The failed sign-ins from this address in the last 5 minutes, oldest first.
  recentFailures(address) {
    const now = this.now();
    const times = (this.failures.get(address) ?? []).filter((at) => now - at < kFailureWindowMs);
    this.failures.set(address, times);
    return times;
  }

  // The session as the page shows it: who, until when, and where this admin listens.
  sessionView(session) {
    const address = this.address();
    return {
      name: session.name,
      role: session.role,
      expires: session.expires,
      listen: {
        scheme: this.tls ? 'https' : 'http',
        bind: this.bind,
        port: address ? address.port : this.port,
      },
    };
  }

  async login(request, response) {
    const address = request.socket.remoteAddress ?? '';
    const failures = this.recentFailures(address);
    if (failures.length >= kMaxFailures) {
      // Unlocked when the oldest of the failures leaves the 5 minutes.
      const retryAfter = Math.max(
        1,
        Math.ceil((failures[0] + kFailureWindowMs - this.now()) / 1000),
      );
      this.answer(
        response,
        429,
        { error: 'too many failed sign-ins; wait 5 minutes', retryAfter },
        { 'retry-after': String(retryAfter) },
      );
      return;
    }
    const body = await this.body(request);
    const role = await this.accounts.verify(body.name, body.password);
    if (!role) {
      failures.push(this.now());
      this.log(
        `KCDS-Admin: a failed sign-in as ${JSON.stringify(
          String(body.name ?? ''),
        )} from ${address}`,
      );
      this.answer(response, 401, {
        error: 'wrong name or password',
        triesLeft: kMaxFailures - failures.length,
      });
      return;
    }
    if (this.sessions.size >= kMaxSessions) {
      const oldest = [...this.sessions].sort((a, b) => a[1].expires - b[1].expires)[0];
      this.sessions.delete(oldest[0]);
    }
    const token = crypto.randomBytes(32).toString('hex');
    const session = { name: body.name, role, expires: this.now() + kSessionMs };
    this.sessions.set(token, session);
    this.log(`KCDS-Admin: ${body.name} signed in (${role}) from ${address}`);
    const secure = this.tls ? '; Secure' : '';
    this.answer(response, 200, this.sessionView(session), {
      'set-cookie': `${kCookie}=${token}; HttpOnly; SameSite=Strict; Path=/; Max-Age=${
        kSessionMs / 1000
      }${secure}`,
    });
  }

  page(response) {
    const nonce = crypto.randomBytes(16).toString('base64');
    const html = fs.readFileSync(PAGE, 'utf8').replaceAll('{{nonce}}', nonce);
    this.answer(response, 200, html, {
      'content-security-policy': `default-src 'none'; script-src 'nonce-${nonce}'; style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; form-action 'self'; frame-ancestors 'none'; base-uri 'none'`,
    });
  }

  playerOf(id) {
    const player = this.host.players.get(Number(id));
    return player ?? null;
  }

  players() {
    return [...this.host.players.values()]
      .map((player) => {
        let position = null;
        try {
          const at = player.position;
          position = at ? { x: at.x, y: at.y, z: at.z } : null;
        } catch {
          position = null;
        }
        return {
          id: player.id,
          name: player.nickname,
          ping: player.ping,
          ip: player.ip,
          identifiers: player.identifiers.map(({ kind, id, verified }) => ({ kind, id, verified })),
          position,
        };
      })
      .sort((a, b) => a.id - b.id);
  }

  resources() {
    const names = new Set([...this.host.manifests.keys(), ...this.host.resources.keys()]);
    return [...names].sort().map((name) => ({
      name,
      version: this.host.manifests.get(name)?.manifest.version ?? '',
      running: this.host.resources.has(name),
    }));
  }

  async serve(request, response) {
    const url = new URL(request.url, 'http://admin');
    const route = url.pathname;
    const method = request.method;
    if (method === 'GET' && (route === '/' || route === '/index.html')) {
      this.page(response);
      return;
    }
    if (method === 'GET' && route === '/favicon.ico') {
      response.writeHead(204, { 'cache-control': 'max-age=86400' });
      response.end();
      return;
    }
    if (!route.startsWith('/api/')) {
      this.answer(response, 404, { error: 'not found' });
      return;
    }
    // A page of another site cannot add this header without our permission (no CORS here).
    if (method !== 'GET' && request.headers['x-kcds-admin'] !== '1') {
      this.answer(response, 403, { error: 'missing X-KCDS-Admin header' });
      return;
    }
    if (method === 'POST' && route === '/api/login') {
      await this.login(request, response);
      return;
    }
    const session = this.activeSession(request);
    if (!session) {
      this.answer(response, 401, { error: 'sign in first' });
      return;
    }
    const owner = session.role === 'owner';
    const done = (what) => {
      this.log(`KCDS-Admin: ${session.name} ${what}`);
      this.answer(response, 200, { ok: true });
    };
    const refuse = (status, error) => this.answer(response, status, { error });
    const parts = route
      .split('/')
      .slice(2)
      .map((part) => decodeURIComponent(part));

    try {
      if (method === 'POST' && route === '/api/logout') {
        this.sessions.delete(this.cookieOf(request));
        this.answer(
          response,
          200,
          { ok: true },
          {
            'set-cookie': `${kCookie}=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0`,
          },
        );
        return;
      }
      if (method === 'GET' && route === '/api/session') {
        this.answer(response, 200, this.sessionView(session));
        return;
      }
      if (method === 'GET' && route === '/api/status') {
        this.answer(response, 200, this.host.statusRoutes.info());
        return;
      }
      if (method === 'GET' && route === '/api/players') {
        this.answer(response, 200, this.players());
        return;
      }
      if (method === 'GET' && route === '/api/log') {
        const after = Number(url.searchParams.get('after') ?? 0);
        this.answer(response, 200, { lines: this.host.adminLog.after(after) });
        return;
      }
      if (parts[0] === 'players' && parts.length === 3 && method === 'POST') {
        await this.playerAction(request, response, session, parts[1], parts[2], done, refuse);
        return;
      }
      if (parts[0] === 'bans') {
        await this.bansAction(request, response, session, parts, method, done, refuse);
        return;
      }
      if (method === 'GET' && route === '/api/server') {
        this.answer(response, 200, { pending: this.host.serverSchedule.pending() });
        return;
      }
      if (!owner) {
        refuse(403, 'only the owner may do this');
        return;
      }
      if (method === 'POST' && route === '/api/command') {
        const command = text((await this.body(request)).text, 1024);
        if (!command) {
          refuse(400, 'a command of at most 1024 bytes');
          return;
        }
        this.log(`KCDS-Admin: ${session.name} runs "${command}"`);
        await this.consoleCommand(command);
        this.answer(response, 200, { ok: true });
        return;
      }
      if (method === 'GET' && route === '/api/resources') {
        this.answer(response, 200, this.resources());
        return;
      }
      if (parts[0] === 'resources' && parts.length === 3 && method === 'POST') {
        const [, name, action] = parts;
        if (!['start', 'stop', 'restart'].includes(action) || !this.host.manifests.has(name)) {
          refuse(404, 'no such resource or action');
          return;
        }
        await this.consoleCommand(`${action} ${name}`);
        done(`${action}s the resource ${name}`);
        return;
      }
      if (method === 'GET' && route === '/api/config') {
        if (!this.configFile) {
          refuse(404, 'the server did not say where its server.json is');
          return;
        }
        const exists = fs.existsSync(this.configFile);
        this.answer(response, 200, {
          path: this.configFile,
          text: exists ? fs.readFileSync(this.configFile, 'utf8') : '{\n}\n',
          exists,
        });
        return;
      }
      if (method === 'PUT' && route === '/api/config') {
        if (!this.configFile) {
          refuse(404, 'the server did not say where its server.json is');
          return;
        }
        const body = await this.body(request);
        const configText = typeof body.text === 'string' ? body.text : '';
        let parsed;
        try {
          parsed = JSON.parse(configText);
        } catch (error) {
          refuse(400, `not valid JSON: ${error.message}`);
          return;
        }
        if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
          refuse(400, 'server.json is one JSON object');
          return;
        }
        const temporary = `${this.configFile}.${process.pid}.tmp`;
        fs.writeFileSync(temporary, configText.endsWith('\n') ? configText : `${configText}\n`);
        try {
          fs.renameSync(temporary, this.configFile);
        } catch {
          // Windows refuses a rename over a file another program holds open: written in place.
          fs.copyFileSync(temporary, this.configFile);
          fs.rmSync(temporary, { force: true });
        }
        done('saves server.json (it applies at the next start)');
        return;
      }
      if (method === 'GET' && route === '/api/backups') {
        this.answer(response, 200, this.host.serverBackups.list());
        return;
      }
      if (method === 'POST' && route === '/api/backups') {
        const made = this.host.serverBackups.create(session.name);
        this.answer(response, 200, made);
        return;
      }
      if (method === 'GET' && route === '/api/crashes') {
        this.answer(response, 200, this.host.crashReports.list());
        return;
      }
      if (method === 'GET' && parts[0] === 'crashes' && parts.length === 2) {
        const report = this.host.crashReports.read(parts[1]);
        if (!report) {
          refuse(404, 'no such crash report');
          return;
        }
        this.answer(response, 200, report);
        return;
      }
      if (method === 'GET' && route === '/api/updates') {
        this.answer(response, 200, this.host.serverUpdates.state());
        return;
      }
      if (method === 'POST' && route === '/api/updates/check') {
        try {
          this.answer(response, 200, await this.host.serverUpdates.check());
        } catch (error) {
          refuse(502, error.message);
        }
        return;
      }
      if (method === 'POST' && route === '/api/updates/install') {
        const minutes = Number((await this.body(request)).minutes ?? 1);
        let staged;
        try {
          staged = await this.host.serverUpdates.stage();
        } catch (error) {
          refuse(502, error.message);
          return;
        }
        const pending = this.host.serverSchedule.request({
          restart: true,
          minutes,
          by: session.name,
        });
        this.log(
          `KCDS-Admin: ${session.name} installs update ${staged.release}: the server restarts ` +
            `in ${minutes} minutes`,
        );
        this.answer(response, 200, { ...staged, pending });
        return;
      }
      if (method === 'POST' && (route === '/api/server/restart' || route === '/api/server/stop')) {
        const minutes = Number((await this.body(request)).minutes ?? 0);
        const restart = route.endsWith('restart');
        const pending = this.host.serverSchedule.request({ restart, minutes, by: session.name });
        this.log(
          `KCDS-Admin: ${session.name} ${
            restart ? 'restarts' : 'stops'
          } the server in ${minutes} minutes`,
        );
        this.answer(response, 200, { pending });
        return;
      }
      if (method === 'POST' && route === '/api/server/cancel') {
        if (!this.host.serverSchedule.cancel(session.name)) {
          refuse(404, 'nothing is planned');
          return;
        }
        done('calls off the planned stop');
        return;
      }
      if (method === 'GET' && route === '/api/accounts') {
        this.answer(response, 200, this.accounts.list());
        return;
      }
      if (method === 'POST' && route === '/api/accounts') {
        const body = await this.body(request);
        this.accounts.setModerator(body.name, body.password);
        done(`sets the moderator account ${body.name}`);
        return;
      }
      if (parts[0] === 'accounts' && parts.length === 2 && method === 'DELETE') {
        if (!this.accounts.removeModerator(parts[1])) {
          refuse(404, 'no such moderator');
          return;
        }
        for (const [token, other] of this.sessions) {
          if (other.name === parts[1]) {
            this.sessions.delete(token);
          }
        }
        done(`removes the moderator account ${parts[1]}`);
        return;
      }
      refuse(404, 'not found');
    } catch (error) {
      if (error?.status) {
        refuse(error.status, error.message);
      } else if (error instanceof TypeError || error instanceof RangeError) {
        refuse(400, error.message);
      } else {
        throw error;
      }
    }
  }

  async playerAction(request, response, session, id, action, done, refuse) {
    const player = this.playerOf(id);
    if (!player) {
      refuse(404, 'no such player');
      return;
    }
    const body = await this.body(request);
    const who = `${player.nickname} (${player.id})`;
    if (action === 'kick') {
      const reason = text(body.reason) || 'Kicked by an admin.';
      player.kick(kickText(reason));
      done(`kicks ${who}: ${reason}`);
    } else if (action === 'message') {
      const message = text(body.text);
      if (!message) {
        refuse(400, 'a message of 1 to 512 bytes');
        return;
      }
      this.host.sendAdminMessage(player, message);
      done(`messages ${who}: ${message}`);
    } else if (action === 'teleport') {
      const point = { x: Number(body.x), y: Number(body.y), z: Number(body.z) };
      if (![point.x, point.y, point.z].every(Number.isFinite) || !player.teleport(point)) {
        refuse(400, 'a position x, y, z in the world');
        return;
      }
      done(`teleports ${who} to ${point.x}, ${point.y}, ${point.z}`);
    } else if (action === 'ban') {
      // Every identifier but the address: behind a shared NAT it would ban strangers.
      const identifiers = player.identifiers
        .filter(({ kind }) => kind !== 'ip')
        .map(({ kind, id: value }) => `${kind}:${value}`);
      const minutes = Number.isSafeInteger(body.minutes) ? body.minutes : 0;
      const ban = this.host.serverBans.add({
        identifiers,
        reason: text(body.reason) || 'Banned from this server.',
        by: session.name,
        minutes,
      });
      player.kick(kickText(this.host.serverBans.refusal(ban)));
      done(
        `bans ${who} (${identifiers.join(', ')}) ${
          minutes ? `for ${minutes} minutes` : 'for ever'
        }: ${ban.reason}`,
      );
    } else {
      refuse(404, 'no such action');
    }
  }

  async bansAction(request, response, session, parts, method, done, refuse) {
    if (method === 'GET' && parts.length === 1) {
      this.answer(response, 200, this.host.serverBans.list());
      return;
    }
    if (method === 'POST' && parts.length === 1) {
      const body = await this.body(request);
      const ban = this.host.serverBans.add({
        identifiers: body.identifiers,
        reason: text(body.reason) ?? '',
        by: session.name,
        minutes: Number.isSafeInteger(body.minutes) ? body.minutes : 0,
      });
      done(`bans ${ban.identifiers.join(', ')}: ${ban.reason}`);
      return;
    }
    if (method === 'DELETE' && parts.length === 2) {
      if (!this.host.serverBans.remove(parts[1])) {
        refuse(404, 'no such ban');
        return;
      }
      done(`lifts the ban ${parts[1]}`);
      return;
    }
    refuse(404, 'not found');
  }
}

module.exports = { AdminServer };
