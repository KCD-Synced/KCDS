'use strict';

// server resources have fetch and the web platform's globals, owned by the resource.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');

const { ResourceHost } = require('./host.cjs');
const { resourceWeb, kMaxInFlight, kMaxQueued } = require('./web/resource_web.cjs');

// A local HTTP server: /hello answers at once, /hang never answers (until the test ends).
async function serve(t) {
  const hanging = new Set();
  const server = http.createServer((request, response) => {
    if (request.url === '/hello') {
      response.setHeader('content-type', 'application/json');
      response.end(JSON.stringify({ hello: 'kcds', method: request.method }));
      return;
    }
    hanging.add(response);
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => {
    for (const response of hanging) {
      response.destroy();
    }
    server.closeAllConnections();
    await new Promise((resolve) => server.close(resolve));
  });
  return `http://127.0.0.1:${server.address().port}`;
}

test('a resource fetches through Node', async (t) => {
  const base = await serve(t);
  const web = resourceWeb('shop');
  const response = await web.globals.fetch(`${base}/hello`, { method: 'POST' });
  assert.ok(response instanceof web.globals.Response);
  assert.deepEqual(await response.json(), { hello: 'kcds', method: 'POST' });
  web.stop();
});

test('the web classes are there', () => {
  const { globals } = resourceWeb('shop');
  for (const key of [
    'fetch',
    'Headers',
    'Request',
    'Response',
    'AbortController',
    'URL',
    'URLSearchParams',
    'TextEncoder',
    'TextDecoder',
    'structuredClone',
    'crypto',
  ]) {
    assert.ok(globals[key], key);
  }
});

test('stopping the resource aborts its requests in flight and waiting', async (t) => {
  const base = await serve(t);
  const web = resourceWeb('shop');
  const requests = [];
  for (let i = 0; i < kMaxInFlight + 3; ++i) {
    requests.push(web.globals.fetch(`${base}/hang`));
  }
  web.stop();
  const results = await Promise.allSettled(requests);
  assert.equal(results.length, kMaxInFlight + 3);
  for (const result of results) {
    assert.equal(result.status, 'rejected');
  }
  await assert.rejects(web.globals.fetch(`${base}/hello`), /stopped/);
});

test('the resource own abort signal still works', async (t) => {
  const base = await serve(t);
  const web = resourceWeb('shop');
  const controller = new AbortController();
  const request = web.globals.fetch(`${base}/hang`, { signal: controller.signal });
  controller.abort();
  await assert.rejects(request, (error) => error.name === 'AbortError');
  web.stop();
});

test('too many waiting requests are refused at once', async (t) => {
  const base = await serve(t);
  const web = resourceWeb('shop');
  const requests = [];
  for (let i = 0; i < kMaxInFlight + kMaxQueued; ++i) {
    requests.push(web.globals.fetch(`${base}/hang`).catch(() => {}));
  }
  await assert.rejects(web.globals.fetch(`${base}/hello`), /Too many requests/);
  web.stop();
  await Promise.all(requests);
});

test('a server resource uses fetch and node:sqlite', async (t) => {
  const base = await serve(t);
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-web-'));
  const logs = [];
  const host = new ResourceHost(root, {
    send: () => {},
    log: (resource, text) => logs.push([resource, text]),
  });
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  const folder = path.join(root, 'shop');
  fs.mkdirSync(folder, { recursive: true });
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({ name: 'shop', kcds: { serverScripts: ['main.js'] } }),
  );
  fs.writeFileSync(
    path.join(folder, 'main.js'),
    `
const { DatabaseSync } = require('node:sqlite');
const db = new DatabaseSync(':memory:');
db.exec('CREATE TABLE items (name TEXT)');
db.prepare('INSERT INTO items VALUES (?)').run('horseshoe');
const row = db.prepare('SELECT name FROM items').get();
fetch(${JSON.stringify(base + '/hello')})
  .then((response) => response.json())
  .then((body) => console.log('fetched ' + body.hello + ' ' + row.name + ' ' + new URL('a/b', 'http://x/').pathname));
`,
  );
  await host.startAll();
  for (let i = 0; i < 100 && !logs.some(([, text]) => text.startsWith('fetched')); ++i) {
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
  assert.ok(
    logs.some(([resource, text]) => resource === 'shop' && text === 'fetched kcds horseshoe /a/b'),
    JSON.stringify(logs),
  );
});

// The server owner picks the database: a resource brings its own npm package in node_modules.
test('a server resource uses its own npm database package', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-npm-'));
  const logs = [];
  const host = new ResourceHost(root, {
    send: () => {},
    log: (resource, text) => logs.push([resource, text]),
  });
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  const folder = path.join(root, 'shop');
  const store = path.join(folder, 'node_modules', 'tiny-store');
  fs.mkdirSync(store, { recursive: true });
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({
      name: 'shop',
      dependencies: { 'tiny-store': '1.0.0' },
      kcds: { serverScripts: ['main.js'] },
    }),
  );
  fs.writeFileSync(
    path.join(store, 'package.json'),
    JSON.stringify({ name: 'tiny-store', version: '1.0.0', main: 'index.js' }),
  );
  fs.writeFileSync(
    path.join(store, 'index.js'),
    `
const { DatabaseSync } = require('node:sqlite');
exports.open = (file) => {
  const db = new DatabaseSync(file);
  db.exec('CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT)');
  return {
    set: (key, value) => db.prepare('INSERT OR REPLACE INTO kv VALUES (?, ?)').run(key, value),
    get: (key) => db.prepare('SELECT value FROM kv WHERE key = ?').get(key)?.value,
    close: () => db.close(),
  };
};
`,
  );
  const file = path.join(root, 'shop.db');
  fs.writeFileSync(
    path.join(folder, 'main.js'),
    `
const store = require('tiny-store').open(${JSON.stringify(file)});
store.set('price', 'horseshoe 12');
console.log('stored ' + store.get('price'));
store.close();
`,
  );
  await host.startAll();
  assert.ok(
    logs.some(([resource, text]) => resource === 'shop' && text === 'stored horseshoe 12'),
    JSON.stringify(logs),
  );
  assert.ok(fs.existsSync(file));
});

// HTTP routes: a resource answers under /<resource>/..., only while it runs.
async function routedHost(t, code) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-routes-'));
  const logs = [];
  const host = new ResourceHost(root, {
    send: () => {},
    log: (resource, text) => logs.push([resource, text]),
    // Any free port of this machine only.
    httpPort: 0,
    httpBind: '127.0.0.1',
  });
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  const folder = path.join(root, 'shop');
  fs.mkdirSync(folder, { recursive: true });
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({ name: 'shop', kcds: { serverScripts: ['main.js'] } }),
  );
  fs.writeFileSync(path.join(folder, 'main.js'), code);
  await host.startAll();
  const base = `http://127.0.0.1:${host.httpRoutes.address().port}`;
  return { host, base, logs };
}

test('a resource answers its HTTP routes', async (t) => {
  const { host, base } = await routedHost(
    t,
    `
console.log('enabled ' + kcds.core.Http.enabled);
kcds.core.Http.route('GET', '/price', (request) => ({ body: { item: request.query.item, price: 12 } }));
kcds.core.Http.route('POST', '/order', async (request) => {
  await new Promise((resolve) => setTimeout(resolve, 5));
  return { status: 201, body: 'ordered ' + JSON.parse(request.body).item, headers: { 'x-shop': 'kcds' } };
});
kcds.core.Http.route('GET', '/broken', () => { throw new Error('broken on purpose'); });
`,
  );
  const price = await fetch(`${base}/shop/price?item=horseshoe`);
  assert.equal(price.status, 200);
  assert.deepEqual(await price.json(), { item: 'horseshoe', price: 12 });
  const order = await fetch(`${base}/shop/order`, {
    method: 'POST',
    body: JSON.stringify({ item: 'sabre' }),
  });
  assert.equal(order.status, 201);
  assert.equal(order.headers.get('x-shop'), 'kcds');
  assert.equal(await order.text(), 'ordered sabre');
  assert.equal((await fetch(`${base}/shop/price`, { method: 'DELETE' })).status, 405);
  assert.equal((await fetch(`${base}/shop/nothing`)).status, 404);
  assert.equal((await fetch(`${base}/shop/broken`)).status, 500);
  const big = await fetch(`${base}/shop/order`, {
    method: 'POST',
    body: 'x'.repeat(2 << 20),
  }).catch((error) => error);
  assert.ok(big instanceof Error || big.status === 413);
  await host.stop('shop');
  assert.equal((await fetch(`${base}/shop/price?item=a`)).status, 404);
});

test('no HTTP listener without a port', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'kcds-routes-off-'));
  const logs = [];
  const host = new ResourceHost(root, {
    send: () => {},
    log: (resource, text) => logs.push([resource, text]),
  });
  t.after(async () => {
    await host.shutdown();
    fs.rmSync(root, { recursive: true, force: true });
  });
  const folder = path.join(root, 'shop');
  fs.mkdirSync(folder, { recursive: true });
  fs.writeFileSync(
    path.join(folder, 'package.json'),
    JSON.stringify({ name: 'shop', kcds: { serverScripts: ['main.js'] } }),
  );
  fs.writeFileSync(
    path.join(folder, 'main.js'),
    `kcds.core.Http.route('GET', '/price', () => ({})); console.log('enabled ' + kcds.core.Http.enabled);`,
  );
  await host.startAll();
  assert.equal(host.httpRoutes.address(), null);
  assert.ok(logs.some(([resource, text]) => resource === 'shop' && text === 'enabled false'));
});

test('one address gets at most the burst, then 429 until the rate refills', async (t) => {
  const { HttpRoutes, kRateBurst, kRatePerSecond } = require('./web/http_routes.cjs');
  const routes = new HttpRoutes({ port: 0, bind: '127.0.0.1' });
  let now = 1000;
  routes.now = () => now;
  for (let i = 0; i < kRateBurst; ++i) {
    assert.equal(routes.admit('1.2.3.4'), true, `request ${i}`);
  }
  assert.equal(routes.admit('1.2.3.4'), false);
  assert.equal(routes.admit('5.6.7.8'), true);
  now += 1000 / kRatePerSecond;
  assert.equal(routes.admit('1.2.3.4'), true);
  assert.equal(routes.admit('1.2.3.4'), false);
  t.after(() => routes.close());
});

test("a resource's fetch cuts off an answer over the body limit", async (t) => {
  const { kMaxBody } = require('./web/resource_web.cjs');
  const server = http.createServer((request, response) => {
    response.writeHead(200, { 'content-type': 'application/octet-stream' });
    const chunk = Buffer.alloc(1 << 20);
    let sent = 0;
    const more = () => {
      while (sent <= kMaxBody) {
        sent += chunk.length;
        if (!response.write(chunk)) {
          response.once('drain', more);
          return;
        }
      }
      response.end();
    };
    more();
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => {
    server.closeAllConnections();
    await new Promise((resolve) => server.close(resolve));
  });
  const web = resourceWeb('shop');
  const url = `http://127.0.0.1:${server.address().port}/big`;
  const response = await web.globals.fetch(url);
  assert.ok(response instanceof web.globals.Response);
  assert.equal(response.url, url);
  await assert.rejects(response.arrayBuffer(), /over/);
  web.stop();
});

// a resource's WebSocket is Node's client, closed when the resource stops.
async function serveWebSocket(t) {
  const crypto = require('node:crypto');
  const sockets = new Set();
  const server = http.createServer();
  server.on('upgrade', (request, socket) => {
    sockets.add(socket);
    const accept = crypto
      .createHash('sha1')
      .update(request.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
      .digest('base64');
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
        `Sec-WebSocket-Accept: ${accept}\r\n\r\n`,
    );
    // One unmasked text frame, "hello".
    socket.write(Buffer.from([0x81, 5, ...Buffer.from('hello')]));
    socket.on('data', (data) => {
      // A close frame from the client is answered with one.
      if ((data[0] & 0x0f) === 8) {
        socket.end(Buffer.from([0x88, 0]));
      }
    });
    socket.on('error', () => {});
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => {
    for (const socket of sockets) {
      socket.destroy();
    }
    await new Promise((resolve) => server.close(resolve));
  });
  return `ws://127.0.0.1:${server.address().port}/`;
}

test('a resource WebSocket talks and is closed when the resource stops', async (t) => {
  const url = await serveWebSocket(t);
  const web = resourceWeb('discord');
  assert.equal(typeof web.globals.WebSocket, 'function');
  const socket = new web.globals.WebSocket(url);
  assert.ok(socket instanceof globalThis.WebSocket);
  const message = await new Promise((resolve, reject) => {
    socket.addEventListener('message', (event) => resolve(event.data), { once: true });
    socket.addEventListener('error', reject, { once: true });
  });
  assert.equal(message, 'hello');
  const closed = new Promise((resolve) =>
    socket.addEventListener('close', (event) => resolve(event.code), { once: true }),
  );
  web.stop();
  assert.ok([1000, 1005].includes(await closed));
  assert.throws(() => new web.globals.WebSocket(url), /stopped/);
});
