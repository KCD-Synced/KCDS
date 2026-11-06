'use strict';

// Reusable timer, console and execution-environment contract fixture.
//
// The fixture takes the SDK globals it should exercise and uses no Node APIs,
// so the exact same checks run inside the server resource vm context and inside
// the production client evaluator. It returns a promise for the number of
// contract assertions that passed; a violation rejects with a descriptive
// error. Delays are intentionally small but comfortably above timer resolution
// so the ordering and repetition checks stay stable under load.

function verifyTimerContract(env) {
  const { setTimeout, setInterval, clearTimeout, clearInterval, queueMicrotask, console, kcds } =
    env;
  const { Runtime } = kcds.core;

  let checks = 0;
  const pass = (condition, message) => {
    if (!condition) {
      throw new Error('Timer contract violation: ' + message);
    }
    checks += 1;
  };

  const delay = (milliseconds) =>
    new Promise((resolve) => {
      setTimeout(resolve, milliseconds);
    });

  const run = async () => {
    // Surface shape: every documented timer entry point is a callable global.
    pass(typeof setTimeout === 'function', 'setTimeout is callable');
    pass(typeof setInterval === 'function', 'setInterval is callable');
    pass(typeof clearTimeout === 'function', 'clearTimeout is callable');
    pass(typeof clearInterval === 'function', 'clearInterval is callable');
    pass(typeof queueMicrotask === 'function', 'queueMicrotask is callable');

    // console exposes exactly the five documented methods, all callable.
    for (const method of ['log', 'info', 'warn', 'error', 'debug']) {
      pass(typeof console[method] === 'function', 'console.' + method + ' is callable');
    }
    console.log('timer contract', 1, true);
    console.info('info channel');
    console.warn('warn channel');
    console.error('error channel');
    console.debug('debug channel');
    checks += 1;

    // Runtime reports a single authoritative side and versions.
    pass(typeof Runtime.isClient === 'boolean', 'isClient is boolean');
    pass(typeof Runtime.isServer === 'boolean', 'isServer is boolean');
    pass(Runtime.isClient !== Runtime.isServer, 'exactly one execution side is active');
    pass(
      typeof Runtime.apiVersion === 'string' && Runtime.apiVersion.length > 0,
      'apiVersion is a non-empty string',
    );
    pass(typeof Runtime.serverVersion === 'string', 'serverVersion is a string');

    // setTimeout runs once, forwards its trailing arguments and hands back a
    // handle the matching clear accepts without throwing.
    let timeoutCalls = 0;
    let forwarded = null;
    const timeoutHandle = setTimeout(
      (first, second) => {
        timeoutCalls += 1;
        forwarded = [first, second];
      },
      5,
      'henry',
      42,
    );
    await delay(30);
    pass(timeoutCalls === 1, 'setTimeout handler ran exactly once');
    pass(
      forwarded !== null && forwarded[0] === 'henry' && forwarded[1] === 42,
      'setTimeout forwarded its trailing arguments',
    );
    clearTimeout(timeoutHandle);
    checks += 1;

    // clearTimeout cancels a still-pending timeout.
    let cancelledRan = false;
    const cancelled = setTimeout(() => {
      cancelledRan = true;
    }, 20);
    clearTimeout(cancelled);
    await delay(40);
    pass(cancelledRan === false, 'clearTimeout cancelled a pending timeout');

    // setInterval repeats until cleared and then stops firing.
    let intervalCalls = 0;
    const interval = setInterval(() => {
      intervalCalls += 1;
    }, 8);
    await delay(40);
    const afterClear = intervalCalls;
    clearInterval(interval);
    pass(afterClear >= 2, 'setInterval fired repeatedly before it was cleared');
    await delay(40);
    pass(intervalCalls === afterClear, 'clearInterval stopped further ticks');

    // Microtasks drain before the next macrotask and keep FIFO order.
    const order = [];
    setTimeout(() => {
      order.push('macrotask');
    }, 0);
    queueMicrotask(() => {
      order.push('microtask-1');
    });
    queueMicrotask(() => {
      order.push('microtask-2');
    });
    await delay(30);
    pass(
      order[0] === 'microtask-1' && order[1] === 'microtask-2',
      'microtasks ran first in FIFO order',
    );
    pass(order[2] === 'macrotask', 'the macrotask ran after the microtasks drained');

    return checks;
  };

  return run();
}

module.exports = verifyTimerContract;
