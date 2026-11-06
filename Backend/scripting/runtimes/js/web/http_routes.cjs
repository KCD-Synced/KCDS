'use strict';

// HTTP routes of server resources: a website, a launcher backend or
// a bot can ask the server's resources over HTTP. The public resource contract declares no HTTP
// API, so this is KCDS's own: the global `Http` of a server resource.
//
//   Http.enabled                       whether the server listens (the server owner's choice)
//   Http.route(method, path, handler)  serves `<method> /<resource><path>`; returns a function that
//                                      removes the route
//
// The handler gets { method, path, query, headers, body } (the body as text) and returns, or
// resolves to, { status, headers, body } (a body that is not a string or a Buffer is sent as
// JSON). It runs as the resource's own code. One listener serves every resource; it listens only
// when the server owner sets KCDS_HTTP_PORT (and KCDS_HTTP_BIND, 0.0.0.0 by default), since it
// opens the server to the network. Requests are bounded: kMaxBody bytes of body, kTimeoutMs for
// an answer, kMaxRoutes routes per resource, and kRatePerSecond requests per second from one
// address (bursts up to kRateBurst; more get 429). A resource's routes go with it when it stops.

const http = require('node:http');

const kMaxBody = 1 << 20;
const kTimeoutMs = 10000;
const kMaxRoutes = 64;
const kRatePerSecond = 20;
const kRateBurst = 40;
// Addresses remembered for the rate at once; the least recently seen is forgotten first.
const kRateAddresses = 4096;
const kMethods = new Set(['GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'HEAD', 'OPTIONS']);

function answer(response, status, body, headers = {}) {
  if (response.headersSent) {
    return;
  }
  let payload = body;
  const sent = { ...headers };
  if (payload === undefined || payload === null) {
    payload = '';
  } else if (typeof payload !== 'string' && !Buffer.isBuffer(payload)) {
    payload = JSON.stringify(payload);
    if (!Object.keys(sent).some((key) => key.toLowerCase() === 'content-type')) {
      sent['content-type'] = 'application/json';
    }
  }
  response.writeHead(status, sent);
  response.end(payload);
}

class HttpRoutes {
  // port: where to listen (0 any free port, missing: not at all); bind: the address; log(text).
  constructor({ port, bind, log }) {
    this.port = Number.isInteger(port) && port >= 0 && port < 65536 ? port : null;
    this.bind = bind || '0.0.0.0';
    this.log = log ?? (() => {});
    // resource name -> Map("METHOD /path" -> { handler, scoped })
    this.routes = new Map();
    this.server = null;
    // address -> { tokens, at } (a token bucket per client address)
    this.buckets = new Map();
    // The server's built-in status paths (web/status_routes.cjs), served first.
    this.status = null;
    this.now = () => Date.now();
  }

  // Whether a request from this address may be served now.
  admit(address) {
    const now = this.now();
    let bucket = this.buckets.get(address);
    if (bucket) {
      this.buckets.delete(address);
      bucket.tokens = Math.min(
        kRateBurst,
        bucket.tokens + ((now - bucket.at) / 1000) * kRatePerSecond,
      );
      bucket.at = now;
    } else {
      bucket = { tokens: kRateBurst, at: now };
      if (this.buckets.size >= kRateAddresses) {
        this.buckets.delete(this.buckets.keys().next().value);
      }
    }
    this.buckets.set(address, bucket);
    if (bucket.tokens < 1) {
      return false;
    }
    bucket.tokens -= 1;
    return true;
  }

  start() {
    if (this.port === null || this.server) {
      return Promise.resolve();
    }
    this.server = http.createServer((request, response) => this.serve(request, response));
    this.server.requestTimeout = kTimeoutMs;
    return new Promise((resolve) => {
      this.server.once('error', (error) => {
        this.log(`HTTP routes could not listen on ${this.bind}:${this.port}: ${error.message}`);
        this.server = null;
        resolve();
      });
      this.server.listen(this.port, this.bind, () => {
        this.log(`HTTP routes listen on ${this.bind}:${this.server.address().port}`);
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

  // The `Http` global of one resource and stop(), which removes its routes. scoped(fn) runs fn as
  // the resource's code.
  forResource(name, scoped) {
    const routes = new Map();
    this.routes.set(name, routes);
    const prefix = `/${name}`;
    const api = {
      route(method, path, handler) {
        const verb = String(method).toUpperCase();
        if (!kMethods.has(verb)) {
          throw new TypeError(`Unknown HTTP method ${method}`);
        }
        if (typeof path !== 'string' || !path.startsWith('/') || path.includes('?')) {
          throw new TypeError('A route path starts with / and has no query');
        }
        if (typeof handler !== 'function') {
          throw new TypeError('A route needs a handler function');
        }
        const key = `${verb} ${prefix}${path === '/' ? '' : path}`;
        if (!routes.has(key) && routes.size >= kMaxRoutes) {
          throw new Error(`A resource has at most ${kMaxRoutes} HTTP routes`);
        }
        const entry = { handler, scoped };
        routes.set(key, entry);
        return () => {
          if (routes.get(key) === entry) {
            routes.delete(key);
          }
        };
      },
    };
    Object.defineProperty(api, 'enabled', { get: () => this.server !== null });
    return {
      api: Object.freeze(api),
      stop: () => {
        routes.clear();
        if (this.routes.get(name) === routes) {
          this.routes.delete(name);
        }
      },
    };
  }

  find(method, path) {
    let known = false;
    for (const routes of this.routes.values()) {
      const entry = routes.get(`${method} ${path}`);
      if (entry) {
        return { entry };
      }
      for (const key of routes.keys()) {
        if (key.slice(key.indexOf(' ') + 1) === path) {
          known = true;
        }
      }
    }
    return { known };
  }

  serve(request, response) {
    if (!this.admit(request.socket.remoteAddress ?? '')) {
      answer(response, 429, { error: 'too many requests' }, { 'retry-after': '1' });
      request.resume();
      return;
    }
    const url = new URL(request.url, 'http://route');
    const path = url.pathname.length > 1 ? url.pathname.replace(/\/$/, '') : url.pathname;
    const status = this.status?.handle(request.method, path);
    if (status) {
      answer(response, status.status, status.body, status.headers);
      request.resume();
      return;
    }
    const { entry, known } = this.find(request.method, path);
    if (!entry) {
      answer(response, known ? 405 : 404, { error: known ? 'method not allowed' : 'not found' });
      request.resume();
      return;
    }
    const chunks = [];
    let size = 0;
    let refused = false;
    request.on('data', (chunk) => {
      size += chunk.length;
      if (size > kMaxBody) {
        if (!refused) {
          refused = true;
          answer(response, 413, { error: 'body too large' });
          request.destroy();
        }
        return;
      }
      chunks.push(chunk);
    });
    request.on('end', () => {
      if (refused) {
        return;
      }
      const call = {
        method: request.method,
        path: path.slice(path.indexOf('/', 1) < 0 ? path.length : path.indexOf('/', 1)) || '/',
        query: Object.fromEntries(url.searchParams),
        headers: { ...request.headers },
        body: Buffer.concat(chunks).toString('utf8'),
      };
      let timer = null;
      const late = new Promise((resolve) => {
        timer = setTimeout(
          () => resolve({ status: 504, body: { error: 'no answer' } }),
          kTimeoutMs,
        );
      });
      const result = new Promise((resolve) => {
        resolve(entry.scoped(() => entry.handler(call)));
      });
      Promise.race([result, late])
        .then((reply) => {
          const value = reply ?? {};
          answer(
            response,
            Number.isInteger(value.status) ? value.status : 200,
            value.body,
            value.headers,
          );
        })
        .catch((error) => {
          this.log(`HTTP route ${request.method} ${path} failed: ${error?.stack ?? error}`);
          answer(response, 500, { error: 'route failed' });
        })
        .finally(() => clearTimeout(timer));
    });
  }
}

module.exports = { HttpRoutes, kMaxBody, kTimeoutMs, kMaxRoutes, kRatePerSecond, kRateBurst };
