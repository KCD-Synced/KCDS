'use strict';

// The web platform's globals for a server resource: `fetch` and its
// classes, `AbortController`, `URL`, `TextEncoder` and the rest that Node gives its own scripts
// but a resource's context did not have. Databases and npm packages need nothing here: a server
// resource already requires Node's built-ins (`node:sqlite`) and its own dependencies.
//
// A resource's `fetch` belongs to the resource: at most kMaxInFlight requests of one resource are
// in flight and kMaxQueued more wait (a further one is refused at once), and when the resource
// stops or fails every request it still has in flight or waiting is aborted, so a restarted
// resource never gets answers meant for its previous run. The requests themselves are Node's
// (undici, off the server's tick). A request that has no answer after kTimeoutMs is aborted (a
// resource's own signal still aborts it sooner), and an answer's body is cut off with an error
// after kMaxBody bytes, so one slow or huge answer cannot hold the server.
//
// `WebSocket` is Node's own client, and its sockets belong to the resource too: at
// most kMaxSockets of one resource are open at once (a further one is refused when it is made),
// and when the resource stops every socket it still has is closed (code 1000, the reason naming the stop), so a Discord bot
// or a launcher link of a restarted resource does not keep the old run's connection.

const kMaxInFlight = 16;
const kMaxQueued = 256;
const kTimeoutMs = 30000;
const kMaxBody = 16 << 20;
const kMaxSockets = 64;

// Node's own web classes, shared with the resource as they are (an `instanceof Response` in the
// resource holds for what its `fetch` returns).
const kShared = [
  'Headers',
  'Request',
  'Response',
  'FormData',
  'Blob',
  'File',
  'AbortController',
  'AbortSignal',
  'URL',
  'URLSearchParams',
  'TextEncoder',
  'TextDecoder',
  'Event',
  'EventTarget',
  'DOMException',
  'ReadableStream',
  'WritableStream',
  'TransformStream',
  'MessageEvent',
  'CloseEvent',
];

function stoppedError(name) {
  return new DOMException(`Resource ${name} stopped`, 'AbortError');
}

// The same answer with its body cut off after kMaxBody bytes.
function bounded(response, name) {
  if (!response.body) {
    return response;
  }
  let size = 0;
  const limit = new TransformStream({
    transform(chunk, controller) {
      size += chunk.byteLength;
      if (size > kMaxBody) {
        controller.error(new Error(`An answer to resource ${name} is over ${kMaxBody} bytes`));
        return;
      }
      controller.enqueue(chunk);
    },
  });
  const result = new Response(response.body.pipeThrough(limit), {
    status: response.status,
    statusText: response.statusText,
    headers: response.headers,
  });
  // A made Response has no url of its own; the answer's is kept.
  Object.defineProperty(result, 'url', { value: response.url });
  Object.defineProperty(result, 'redirected', { value: response.redirected });
  return result;
}

// The globals for one resource and stop(), which aborts what its requests still wait for.
function resourceWeb(name) {
  const queued = [];
  const controllers = new Set();
  const sockets = new Set();
  let inFlight = 0;
  let stopped = false;

  function run(request) {
    inFlight += 1;
    const controller = new AbortController();
    controllers.add(controller);
    const signal = request.init?.signal;
    const follow = () => controller.abort(signal.reason);
    const timer = setTimeout(
      () => controller.abort(new DOMException('The request timed out', 'TimeoutError')),
      kTimeoutMs,
    );
    if (signal) {
      if (signal.aborted) {
        follow();
      } else {
        signal.addEventListener('abort', follow, { once: true });
      }
    }
    globalThis
      .fetch(request.input, { ...request.init, signal: controller.signal })
      .then((response) => request.resolve(bounded(response, name)), request.reject)
      .finally(() => {
        clearTimeout(timer);
        signal?.removeEventListener('abort', follow);
        controllers.delete(controller);
        inFlight -= 1;
        pump();
      });
  }

  function pump() {
    while (!stopped && inFlight < kMaxInFlight && queued.length > 0) {
      run(queued.shift());
    }
  }

  function fetch(input, init) {
    if (stopped) {
      return Promise.reject(stoppedError(name));
    }
    if (inFlight >= kMaxInFlight && queued.length >= kMaxQueued) {
      return Promise.reject(
        new Error(`Too many requests of resource ${name} wait (${kMaxQueued} at most)`),
      );
    }
    return new Promise((resolve, reject) => {
      queued.push({ input, init, resolve, reject });
      pump();
    });
  }

  const globals = { fetch };
  for (const key of kShared) {
    if (globalThis[key] !== undefined) {
      globals[key] = globalThis[key];
    }
  }
  globals.structuredClone = globalThis.structuredClone;
  globals.queueMicrotask = globalThis.queueMicrotask;
  globals.atob = globalThis.atob;
  globals.btoa = globalThis.btoa;
  globals.crypto = globalThis.crypto;
  globals.performance = globalThis.performance;
  const NodeWebSocket = globalThis.WebSocket;
  if (NodeWebSocket !== undefined) {
    class WebSocket extends NodeWebSocket {
      constructor(url, protocols) {
        if (stopped) {
          throw stoppedError(name);
        }
        if (sockets.size >= kMaxSockets) {
          throw new Error(`Resource ${name} has ${kMaxSockets} WebSockets open already`);
        }
        super(url, protocols);
        sockets.add(this);
        this.addEventListener('close', () => sockets.delete(this));
      }
    }
    globals.WebSocket = WebSocket;
  }

  function stop() {
    if (stopped) {
      return;
    }
    stopped = true;
    const error = stoppedError(name);
    for (const request of queued.splice(0)) {
      request.reject(error);
    }
    for (const controller of controllers) {
      controller.abort(error);
    }
    for (const socket of sockets) {
      socket.close(1000, `Resource ${name} stopped`);
    }
    sockets.clear();
  }

  return { globals, stop };
}

module.exports = { resourceWeb, kMaxInFlight, kMaxQueued, kTimeoutMs, kMaxBody, kMaxSockets };
